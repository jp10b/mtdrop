/* SPDX-License-Identifier: MIT */
/* Radio link: the protocol between radiod (radio server) and its one client (awdld, radioctl) over a
 * message-mode named pipe (docs/adr/0003-awdld-on-windows.md). Plain C11, no OS headers, so both the MSVC
 * radio side and the MinGW AWDL side compile it unchanged. MIT so that any client may include it.
 *
 * The message set is modelled on raph559/winawdl's radio/radio_link.h (MIT, @ 0db7031: HELLO, RX, TX, SET_CHANNEL,
 * SET_BSSID_FILTER); the code and layout here are our own, extended by TX_STATUS, CHANNEL, HEARTBEAT, STATS, BYE.
 *
 * Every pipe message is one header followed by 'length' payload bytes. All integers little-endian.
 *
 *   radio -> client
 *     HELLO      once after connect: struct rl_hello (version, channel, band, own address, limits)
 *     RX         radiotap header (built by rl_radiotap_build) + 802.11 frame without FCS
 *     TX_STATUS  one per accepted or refused TX: struct rl_tx_status
 *     CHANNEL    answer to SET_CHANNEL: struct rl_channel
 *     STATS      answer to STATS_REQ: struct rl_stats
 *     BYE        the radio is stopping; payload: u32 reason (RL_BYE_*)
 *   client -> radio
 *     TX              u32 cookie (client's choice, echoed in TX_STATUS), radiotap header, 802.11 frame
 *                     without FCS. The radio decides ACK and retries from address 1 (group: no ACK; unicast:
 *                     ACK, retries, TX status). Address 2 must be the HELLO address.
 *     SET_CHANNEL     u8 channel
 *     SET_BSSID_FILTER  6 bytes; RX forwards only frames whose address 3 matches (all zero: everything)
 *     HEARTBEAT       empty. Any message counts; the radio stops after RL_LEASE_MS without one.
 *     STATS_REQ       empty
 */
#ifndef MTDROP_RADIO_LINK_H
#define MTDROP_RADIO_LINK_H

#include <stddef.h>
#include <stdint.h>

#define RL_PIPE_NAME "\\\\.\\pipe\\mtdrop-radio"
#define RL_VERSION 1u
#define RL_HEADER_SIZE 8u
#define RL_MAX_PAYLOAD 4096u
#define RL_MAX_MESSAGE (RL_HEADER_SIZE + RL_MAX_PAYLOAD)
#define RL_LEASE_MS 5000u /* the client must send something at least this often */

enum rl_type {
    RL_HELLO = 1,
    RL_RX = 2,
    RL_TX = 3,
    RL_TX_STATUS = 4,
    RL_SET_CHANNEL = 5,
    RL_CHANNEL = 6,
    RL_SET_BSSID_FILTER = 7,
    RL_HEARTBEAT = 8,
    RL_STATS_REQ = 9,
    RL_STATS = 10,
    RL_BYE = 11
};

enum rl_bye_reason {
    RL_BYE_TIME_LIMIT = 1,  /* the session's --max-seconds ran out */
    RL_BYE_RADIO_ERROR = 2, /* the chip reported an error (bus dead, poll error) */
    RL_BYE_PROTOCOL = 3     /* the client sent a malformed message */
};

struct rl_header {
    uint16_t type;
    uint16_t flags;   /* 0 */
    uint32_t length;  /* payload bytes */
};

#define RL_HELLO_SIZE 20u
struct rl_hello {
    uint32_t version;
    uint8_t channel;
    uint8_t band;        /* 0 = 2.4 GHz, 1 = 5 GHz */
    uint8_t width_mhz;   /* what the radio receives: 20 */
    uint8_t tx_allowed;  /* 1 if TX is allowed on the current channel */
    uint8_t addr[6];     /* the address the radio ACKs and transmits from */
    uint16_t max_frame;  /* largest 802.11 frame TX accepts */
    uint32_t lease_ms;
};

enum rl_tx_result {
    RL_TXR_DONE = 1,     /* the chip returned the frame */
    RL_TXR_LOST = 2,     /* no completion within the radio's timeout */
    RL_TXR_ABORTED = 3,  /* still in flight when the radio stopped */
    RL_TXR_REFUSED = 4   /* not sent: rejected by validation, channel not allowed, or no free slot */
};

enum rl_ack {
    RL_ACK_UNKNOWN = 0,        /* ACK requested, no TX status record */
    RL_ACK_YES = 1,
    RL_ACK_NO = 2,
    RL_ACK_NOT_REQUESTED = 3   /* group-addressed frame */
};

#define RL_TX_STATUS_SIZE 16u
struct rl_tx_status {
    uint32_t cookie;
    uint8_t result;      /* enum rl_tx_result */
    uint8_t acked;       /* enum rl_ack */
    uint8_t tx_count;    /* transmissions, 0 = not reported */
    uint8_t channel;     /* channel the radio was on when the frame was submitted */
    uint32_t elapsed_us; /* submit -> completion */
    int32_t error;       /* radio error code for RL_TXR_REFUSED, else 0 */
};

#define RL_CHANNEL_SIZE 8u
struct rl_channel {
    uint8_t channel;     /* the channel now tuned (unchanged on failure) */
    uint8_t ok;          /* 1 = switched */
    uint8_t tx_allowed;
    uint8_t reserved;
    uint32_t elapsed_us;
};

#define RL_STATS_COUNT 16u
#define RL_STATS_SIZE (4u * RL_STATS_COUNT)
enum rl_stat {
    RL_ST_RX_FRAMES = 0,   /* frames taken from the chip */
    RL_ST_RX_FORWARDED,    /* sent to the client */
    RL_ST_RX_FILTERED,     /* dropped by the BSSID filter */
    RL_ST_RX_BAD_FCS,      /* dropped: FCS error */
    RL_ST_RX_OWN,          /* dropped: transmitted by us */
    RL_ST_RX_PIPE_FULL,    /* dropped: the client did not read fast enough */
    RL_ST_TX_SUBMITTED,
    RL_ST_TX_REFUSED,
    RL_ST_TX_DONE,
    RL_ST_TX_LOST,
    RL_ST_TX_ACKED,
    RL_ST_TX_NOT_ACKED,
    RL_ST_CHANNEL_SWITCHES,
    RL_ST_CHANNEL_FAILURES,
    RL_ST_MESSAGES_IN,
    RL_ST_UPTIME_MS
};
struct rl_stats {
    uint32_t v[RL_STATS_COUNT];
};

/* ---- encoding (return bytes written, 0 if 'cap' is too small) ---- */
size_t rl_put_header(uint8_t *out, size_t cap, uint16_t type, uint32_t length);
size_t rl_put_hello(uint8_t *out, size_t cap, const struct rl_hello *h);
size_t rl_put_tx_status(uint8_t *out, size_t cap, const struct rl_tx_status *s);
size_t rl_put_channel(uint8_t *out, size_t cap, const struct rl_channel *c);
size_t rl_put_stats(uint8_t *out, size_t cap, const struct rl_stats *s);

/* ---- decoding (return 0, or -1 if the buffer is malformed) ---- */
/* Header of a complete message of 'len' bytes: type valid, length == len - 8, length <= RL_MAX_PAYLOAD. */
int rl_get_header(const uint8_t *msg, size_t len, struct rl_header *h);
int rl_get_hello(const uint8_t *p, size_t len, struct rl_hello *h);
int rl_get_tx_status(const uint8_t *p, size_t len, struct rl_tx_status *s);
int rl_get_channel(const uint8_t *p, size_t len, struct rl_channel *c);
int rl_get_stats(const uint8_t *p, size_t len, struct rl_stats *s);

/* ---- radiotap ---- */
struct rl_rx_meta {
    uint64_t tsft_us;      /* host clock (QueryPerformanceCounter, microseconds) */
    uint8_t fcs_bad;       /* radiotap FLAGS BADFCS */
    uint8_t channel;
    uint8_t band;          /* 0 = 2.4 GHz, 1 = 5 GHz */
    uint8_t has_signal;
    int8_t signal_dbm;
    uint8_t rate_500k;     /* legacy rate in 500 kb/s units, 0 = not legacy / unknown */
    uint8_t has_mcs;       /* HT */
    uint8_t mcs_index;
    uint8_t mcs_bw40;
};
/* Header + fields TSFT, FLAGS, RATE (if legacy), CHANNEL, DBM_ANTSIGNAL (if known), MCS (if HT), aligned as
 * radiotap requires. Returns its length (at most RL_RADIOTAP_MAX), 0 if 'cap' is too small. */
#define RL_RADIOTAP_MAX 32u
size_t rl_radiotap_build(uint8_t *out, size_t cap, const struct rl_rx_meta *m);
/* Frequency in MHz of a channel (2.4 GHz 1-14, 5 GHz 32-177), 0 if unknown. */
uint16_t rl_channel_freq(uint8_t channel, uint8_t band);

/* Parses a TX payload: cookie, radiotap header (only its length and an optional RATE field are used), frame.
 * Returns 0 or -1 (too short, bad radiotap version or length, frame shorter than 24 bytes). */
int rl_parse_tx(const uint8_t *p, size_t len, uint32_t *cookie, const uint8_t **frame, size_t *frame_len,
                uint8_t *rate_500k);
/* Builds a TX payload with a minimal radiotap header (RATE only if rate_500k != 0). Returns its length or 0. */
size_t rl_build_tx(uint8_t *out, size_t cap, uint32_t cookie, const uint8_t *frame, size_t frame_len,
                   uint8_t rate_500k);

#endif
