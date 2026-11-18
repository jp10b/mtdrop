/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radiod serve: the radio server of docs/adr/0003-awdld-on-windows.md.
 *
 * One client at a time on RL_PIPE_NAME (message mode, SYSTEM + Administrators only). The radio is brought up
 * and tuned before the client connects; it then forwards received frames (BSSID filter, own frames and FCS
 * errors dropped, radiotap built here with the per-frame channel), transmits the client's frames (ACK and
 * retries for unicast, none for group addresses), reports a TX status for each, and switches channel on
 * request. The session ends when the client disconnects, stays silent for RL_LEASE_MS, the time limit runs
 * out or the chip reports an error; the chip is then shut down orderly (session_close) and radiod exits.
 *
 * Single thread, polling: the chip library is polled for RX and TX completions, the pipe with overlapped
 * I/O. With nothing to do the loop spins ~100 us (hal sleep), so one core stays busy while serving.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "out.h"
#include "radio_link.h"
#include "radiod.h"

#define printf out_printf

#define OUTQ_SLOTS 512u         /* queued messages to the client */
#define OUTQ_RX_LIMIT 448u      /* RX frames are dropped beyond this; the rest is kept for control messages */
#define TXMAP_SLOTS 256u
#define RX_BURST 64u            /* frames taken per loop iteration */
#define PIPE_OUT_BUFFER (1u << 20)
#define PIPE_IN_BUFFER (64u << 10)
#define MAX_CHANNEL_LOGS 20u

static const uint8_t awdl_bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

struct outq {
    uint8_t (*msg)[RL_MAX_MESSAGE];
    uint32_t len[OUTQ_SLOTS];
    unsigned head, count;
    int writing;                /* an overlapped write of the head message is pending */
    OVERLAPPED ov;
};

struct txmap {
    uint32_t lib_cookie, client_cookie;
    uint8_t used, channel;
    uint8_t data;               /* unicast data frame: counted in the data rate summary */
};

/* Unicast data frames by the rate their TX status reports (F-030: which rate really went on air). */
struct data_txs {
    uint32_t by_rate_500k[256]; /* OFDM/CCK rate in 500 kb/s units, from TXS */
    uint32_t other_mode;        /* HT/VHT/HE in TXS */
    uint32_t no_txs;            /* completed without a TX status record */
    uint32_t frames, transmissions;
};

struct server {
    HANDLE pipe;
    struct outq q;
    OVERLAPPED rov;
    int reading;
    uint8_t rbuf[RL_MAX_MESSAGE];
    uint8_t filter[6];
    int filter_on;
    const uint8_t *own;
    struct txmap map[TXMAP_SLOTS];
    struct rl_stats st;
    unsigned channel_logs;
    uint8_t data_rate;          /* 0 = as the client asks (OWL: OFDM 12); else MT792X_RATE_* for unicast data (M9, F-030) */
    uint8_t data_rate_500k;
    struct data_txs dtx;
    int client_gone;
    int bye_reason;
};

/* ---- outgoing queue ---- */

static int outq_init(struct outq *q) {
    memset(q, 0, sizeof(*q));
    q->msg = calloc(OUTQ_SLOTS, RL_MAX_MESSAGE);
    q->ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    return q->msg && q->ov.hEvent ? 0 : -1;
}

static void outq_free(struct outq *q) {
    free(q->msg);
    if (q->ov.hEvent) CloseHandle(q->ov.hEvent);
    memset(q, 0, sizeof(*q));
}

/* Queues header + payload parts. Returns 0, or -1 if the queue is full (the message is dropped). */
static int send_msg(struct server *sv, uint16_t type, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
                    int is_rx) {
    struct outq *q = &sv->q;
    unsigned limit = is_rx ? OUTQ_RX_LIMIT : OUTQ_SLOTS, slot;
    uint8_t *m;
    if (q->count >= limit || alen + blen > RL_MAX_PAYLOAD) return -1;
    slot = (q->head + q->count) % OUTQ_SLOTS;
    m = q->msg[slot];
    rl_put_header(m, RL_HEADER_SIZE, type, (uint32_t)(alen + blen));
    if (alen) memcpy(m + RL_HEADER_SIZE, a, alen);
    if (blen) memcpy(m + RL_HEADER_SIZE + alen, b, blen);
    q->len[slot] = (uint32_t)(RL_HEADER_SIZE + alen + blen);
    q->count++;
    return 0;
}

/* Starts / completes overlapped writes until one is pending or the queue is empty. Returns -1 if the
 * client is gone. */
static int flush_out(struct server *sv) {
    struct outq *q = &sv->q;
    for (;;) {
        DWORD n = 0;
        if (q->writing) {
            if (!GetOverlappedResult(sv->pipe, &q->ov, &n, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return 0;
                return -1;
            }
            q->writing = 0;
            q->head = (q->head + 1u) % OUTQ_SLOTS;
            q->count--;
        }
        if (!q->count) return 0;
        ResetEvent(q->ov.hEvent);
        if (WriteFile(sv->pipe, q->msg[q->head], q->len[q->head], &n, &q->ov)) {
            q->head = (q->head + 1u) % OUTQ_SLOTS;
            q->count--;
            continue;
        }
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        q->writing = 1;
    }
}

/* ---- conversions ---- */

static uint8_t to_rl_band(uint8_t band) {
    return band == MT792X_BAND_5GHZ ? 1u : 0u;
}

/* RXD rate (mt76: CCK idx 0-3 long/short preamble, OFDM hardware values 8-15) -> 500 kb/s units */
static uint8_t legacy_rate_500k(uint8_t mode, uint8_t idx) {
    static const uint8_t cck[4] = {2, 4, 11, 22};
    static const uint8_t ofdm[8] = {96, 48, 24, 12, 108, 72, 36, 18}; /* hardware values 8..15 */
    if (mode == 0) return cck[idx & 3u];
    if (mode == 1 && idx >= 8 && idx <= 15) return ofdm[idx - 8];
    return 0;
}

static uint8_t tx_rate_from_500k(uint8_t r, uint8_t band) {
    switch (r) {
    case 2: return band == MT792X_BAND_2GHZ ? MT792X_RATE_CCK_1M : MT792X_RATE_OFDM_6M;
    case 4: return band == MT792X_BAND_2GHZ ? MT792X_RATE_CCK_2M : MT792X_RATE_OFDM_6M;
    case 11: return band == MT792X_BAND_2GHZ ? MT792X_RATE_CCK_5_5M : MT792X_RATE_OFDM_6M;
    case 22: return band == MT792X_BAND_2GHZ ? MT792X_RATE_CCK_11M : MT792X_RATE_OFDM_6M;
    case 12: return MT792X_RATE_OFDM_6M;
    case 18: return MT792X_RATE_OFDM_9M;
    case 24: return MT792X_RATE_OFDM_12M;
    case 36: return MT792X_RATE_OFDM_18M;
    case 48: return MT792X_RATE_OFDM_24M;
    case 72: return MT792X_RATE_OFDM_36M;
    case 96: return MT792X_RATE_OFDM_48M;
    case 108: return MT792X_RATE_OFDM_54M;
    default: return MT792X_RATE_OFDM_12M; /* OWL asks for 12 Mb/s */
    }
}

static uint8_t rl_ack_of(uint8_t acked) {
    switch (acked) {
    case MT792X_ACK_YES: return RL_ACK_YES;
    case MT792X_ACK_NO: return RL_ACK_NO;
    case MT792X_ACK_NOT_REQUESTED: return RL_ACK_NOT_REQUESTED;
    default: return RL_ACK_UNKNOWN;
    }
}

static uint8_t rl_result_of(uint8_t r) {
    return r == MT792X_TXR_DONE ? RL_TXR_DONE : r == MT792X_TXR_LOST ? RL_TXR_LOST : RL_TXR_ABORTED;
}

static void send_tx_status(struct server *sv, uint32_t client_cookie, uint8_t result, uint8_t acked, uint8_t count,
                           uint8_t channel, uint32_t elapsed, int32_t error) {
    uint8_t p[RL_TX_STATUS_SIZE];
    struct rl_tx_status s = {client_cookie, result, acked, count, channel, elapsed, error};
    rl_put_tx_status(p, sizeof(p), &s);
    (void)send_msg(sv, RL_TX_STATUS, p, sizeof(p), NULL, 0, 0);
}

/* ---- radio backend: the chip, or --loopback (synthetic radio, no hardware access at all) ---- */
/*
 * Loopback exists to test the server, the protocol and clients (radioctl, later awdld) offline: every 50 ms it
 * "receives" alternately an AWDL PSF from a fake peer and a beacon of another BSS (which the AWDL filter must
 * drop); every transmitted frame completes after 1 ms (group address: no ACK requested) or 15 ms (unicast:
 * NOT ACKed after 15 transmissions - nobody is listening); channel switches take 10 ms of nothing.
 */

static int loopback;
static uint64_t serve_t_start; /* qpc_us() when the client connected */
static uint8_t lb_channel, lb_band;
static uint8_t lb_own[6] = {0x02, 0x6d, 0x74, 0x64, 0x72, 0x70};
static const uint8_t lb_peer[6] = {0x02, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e};
static uint64_t lb_next_rx;
static unsigned lb_seq;
static uint8_t lb_frame[64];
#define LB_MAX_PENDING 64u
static struct {
    struct mt792x_tx_done d;
    uint64_t due;
} lb_pending[LB_MAX_PENDING];
static unsigned lb_npending;
static uint32_t lb_cookie;

static uint64_t qpc_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000000u + (uint64_t)(t.QuadPart % f.QuadPart) * 1000000u / (uint64_t)f.QuadPart;
}

/* Waits without Sleep(): its ~15 ms granularity would stall the radio. */
static void spin_us(uint32_t us) {
    uint64_t end = qpc_us() + us;
    while (qpc_us() < end) YieldProcessor();
}

static uint8_t r_channel(void) {
    return loopback ? lb_channel : g_dev.cur_channel;
}

static uint8_t r_band(void) {
    return loopback ? lb_band : g_dev.cur_band;
}

static const uint8_t *r_own(void) {
    return loopback ? lb_own : mt792x_own_mac(&g_dev);
}

static int r_rx_poll(struct mt792x_rx_frame *fr) {
    static const uint8_t awdl_hdr[8] = {127, 0x00, 0x17, 0xf2, 8, 0x10, 0, 0}; /* PSF */
    uint64_t now;
    if (!loopback) return mt792x_rx_poll(&g_dev, fr);
    now = qpc_us();
    if (now < lb_next_rx) return 0;
    lb_next_rx = now + 50000u;
    memset(fr, 0, sizeof(*fr));
    memset(lb_frame, 0, sizeof(lb_frame));
    memset(lb_frame + 4, 0xff, 6);
    memcpy(lb_frame + 10, lb_peer, 6);
    if (lb_seq & 1u) { /* beacon of another BSS */
        lb_frame[0] = 0x80;
        lb_frame[10] = 0x06;
        memcpy(lb_frame + 16, lb_frame + 10, 6);
        fr->len = 36;
    } else {           /* AWDL action frame (PSF), fixed part only */
        lb_frame[0] = 0xd0;
        memcpy(lb_frame + 16, awdl_bssid, 6);
        memcpy(lb_frame + 24, awdl_hdr, sizeof(awdl_hdr));
        fr->len = 40;
    }
    lb_frame[22] = (uint8_t)(lb_seq << 4);
    lb_frame[23] = (uint8_t)(lb_seq >> 4);
    lb_seq++;
    fr->data = lb_frame;
    fr->channel = lb_channel;
    fr->band = lb_band;
    fr->fcs_ok = 1;
    fr->rssi_dbm = -50;
    fr->flags = MT792X_RXF_RATE;
    fr->rate_mode = 1;
    fr->rate_idx = 11; /* OFDM 6 Mb/s */
    fr->host_time_us = now;
    return 1;
}

static int r_tx_frame(const struct mt792x_tx_req *q, uint32_t *cookie) {
    struct mt792x_tx_done *d;
    if (!loopback) return mt792x_tx_frame(&g_dev, q, cookie);
    if (!tx_channel_ok(lb_channel)) return MT792X_E_TX_DENIED;
    if (q->len < 24 || memcmp(q->frame + 10, lb_own, 6) != 0) return MT792X_E_TX_FRAME;
    if (lb_npending >= LB_MAX_PENDING) return MT792X_E_TX_BUSY;
    d = &lb_pending[lb_npending].d;
    memset(d, 0, sizeof(*d));
    d->cookie = ++lb_cookie;
    d->result = MT792X_TXR_DONE;
    d->acked = q->want_ack ? MT792X_ACK_NO : MT792X_ACK_NOT_REQUESTED;
    d->free_status = q->want_ack ? 1u : 0u;
    d->tx_count = q->want_ack ? (uint8_t)(q->tx_count ? q->tx_count : 15u) : 1u;
    d->has_txs = q->want_status;
    d->elapsed_us = q->want_ack ? 15000u : 1000u;
    lb_pending[lb_npending].due = qpc_us() + d->elapsed_us;
    lb_npending++;
    *cookie = d->cookie;
    return 0;
}

static int r_tx_done_get(struct mt792x_tx_done *d) {
    uint64_t now;
    if (!loopback) return mt792x_tx_done_get(&g_dev, d);
    now = qpc_us();
    for (unsigned i = 0; i < lb_npending; i++) {
        if (lb_pending[i].due > now) continue;
        *d = lb_pending[i].d;
        memmove(&lb_pending[i], &lb_pending[i + 1], (lb_npending - i - 1u) * sizeof(lb_pending[0]));
        lb_npending--;
        return 1;
    }
    return 0;
}

static int r_set_channel(uint8_t ch, uint32_t *elapsed) {
    if (!loopback) return mt792x_sniffer_set_channel(&g_dev, ch, band_of(ch), elapsed);
    if (!rl_channel_freq(ch, band_of(ch) == MT792X_BAND_5GHZ ? 1u : 0u)) return MT792X_E_INVAL;
    spin_us(10000);
    lb_channel = ch;
    lb_band = band_of(ch);
    *elapsed = 10000;
    return 0;
}

/* ---- radio side ---- */

/* Takes up to RX_BURST frames. Returns the number taken, or < 0 on a chip error. */
static int pump_rx(struct server *sv) {
    int taken = 0;
    while (taken < (int)RX_BURST) {
        struct mt792x_rx_frame fr;
        struct rl_rx_meta m;
        uint8_t rt[RL_RADIOTAP_MAX];
        size_t rtlen;
        int r = r_rx_poll(&fr);
        if (r < 0) return r;
        if (r == 0) break;
        taken++;
        sv->st.v[RL_ST_RX_FRAMES]++;
        if (fr.flags & MT792X_RXF_HDR_TRANS) {
            sv->st.v[RL_ST_RX_FILTERED]++;
            continue;
        }
        if (!fr.fcs_ok) {
            sv->st.v[RL_ST_RX_BAD_FCS]++;
            continue;
        }
        if (sv->filter_on && (fr.len < 24 || memcmp(fr.data + 16, sv->filter, 6) != 0)) {
            sv->st.v[RL_ST_RX_FILTERED]++;
            continue;
        }
        if (fr.len >= 16 && memcmp(fr.data + 10, sv->own, 6) == 0) {
            sv->st.v[RL_ST_RX_OWN]++;
            continue;
        }
        memset(&m, 0, sizeof(m));
        m.tsft_us = fr.host_time_us;
        m.channel = fr.channel;
        m.band = to_rl_band(fr.band);
        if (fr.rssi_dbm != -128) {
            m.has_signal = 1;
            m.signal_dbm = fr.rssi_dbm;
        }
        if (fr.flags & MT792X_RXF_RATE) {
            if (fr.rate_mode <= 1) m.rate_500k = legacy_rate_500k(fr.rate_mode, fr.rate_idx);
            else if (fr.rate_mode <= 3) {
                m.has_mcs = 1;
                m.mcs_index = fr.rate_idx;
                m.mcs_bw40 = fr.rate_bw == 1;
            }
        }
        rtlen = rl_radiotap_build(rt, sizeof(rt), &m);
        if (!rtlen || rtlen + fr.len > RL_MAX_PAYLOAD) {
            sv->st.v[RL_ST_RX_FILTERED]++;
            continue;
        }
        if (send_msg(sv, RL_RX, rt, rtlen, fr.data, fr.len, 1) != 0) sv->st.v[RL_ST_RX_PIPE_FULL]++;
        else sv->st.v[RL_ST_RX_FORWARDED]++;
    }
    return taken;
}

static int pump_tx_done(struct server *sv) {
    struct mt792x_tx_done d;
    int n = 0, r;
    while ((r = r_tx_done_get(&d)) == 1) {
        n++;
        if (d.result == MT792X_TXR_DONE) sv->st.v[RL_ST_TX_DONE]++;
        else if (d.result == MT792X_TXR_LOST) sv->st.v[RL_ST_TX_LOST]++;
        if (d.acked == MT792X_ACK_YES) sv->st.v[RL_ST_TX_ACKED]++;
        if (d.acked == MT792X_ACK_NO) sv->st.v[RL_ST_TX_NOT_ACKED]++;
        for (unsigned i = 0; i < TXMAP_SLOTS; i++) {
            struct txmap *e = &sv->map[i];
            if (!e->used || e->lib_cookie != d.cookie) continue;
            if (e->data) {
                sv->dtx.frames++;
                sv->dtx.transmissions += d.tx_count;
                if (!d.has_txs) sv->dtx.no_txs++;
                else if (((d.txs_rate >> 6) & 0xfu) <= 1u) {
                    uint8_t r500 = legacy_rate_500k((uint8_t)((d.txs_rate >> 6) & 0xfu), (uint8_t)(d.txs_rate & 0x3fu));
                    sv->dtx.by_rate_500k[r500]++;
                } else
                    sv->dtx.other_mode++;
            }
            send_tx_status(sv, e->client_cookie, rl_result_of(d.result), rl_ack_of(d.acked), d.tx_count, e->channel,
                           d.elapsed_us, 0);
            e->used = 0;
            break;
        }
    }
    return r < 0 ? r : n;
}

static void do_tx(struct server *sv, const uint8_t *p, size_t len) {
    uint32_t cookie = 0, lib_cookie = 0;
    const uint8_t *frame;
    size_t flen;
    uint8_t rate500k;
    struct mt792x_tx_req q;
    unsigned slot = TXMAP_SLOTS;
    int err;

    if (rl_parse_tx(p, len, &cookie, &frame, &flen, &rate500k) != 0 || flen > MT792X_TX_MAX_FRAME) {
        sv->st.v[RL_ST_TX_REFUSED]++;
        send_tx_status(sv, cookie, RL_TXR_REFUSED, RL_ACK_UNKNOWN, 0, r_channel(), 0, MT792X_E_TX_FRAME);
        return;
    }
    for (unsigned i = 0; i < TXMAP_SLOTS; i++)
        if (!sv->map[i].used) {
            slot = i;
            break;
        }
    memset(&q, 0, sizeof(q));
    q.frame = frame;
    q.len = (uint32_t)flen;
    q.rate = tx_rate_from_500k(rate500k, r_band());
    /* --data-rate (ADR-0006 A1): unicast data frames only (type 2, receiver address not group); management and
     * action frames keep the client's rate, so AWDL sync is unchanged. OFDM only: valid on both bands. */
    int unicast_data = flen >= 10 && ((frame[0] >> 2) & 3u) == 2u && !(frame[4] & 1u);
    if (sv->data_rate && unicast_data) q.rate = sv->data_rate;
    q.want_ack = (frame[4] & 1u) ? 0u : 1u; /* group-addressed: no ACK */
    q.want_status = q.want_ack;
    q.tx_count = 0;                          /* mt76's 15 */
    err = slot == TXMAP_SLOTS ? MT792X_E_TX_BUSY : r_tx_frame(&q, &lib_cookie);
    if (err) {
        sv->st.v[RL_ST_TX_REFUSED]++;
        send_tx_status(sv, cookie, RL_TXR_REFUSED, RL_ACK_UNKNOWN, 0, r_channel(), 0, err);
        return;
    }
    sv->st.v[RL_ST_TX_SUBMITTED]++;
    sv->map[slot].used = 1;
    sv->map[slot].data = (uint8_t)unicast_data;
    sv->map[slot].lib_cookie = lib_cookie;
    sv->map[slot].client_cookie = cookie;
    sv->map[slot].channel = r_channel();
}

static void do_set_channel(struct server *sv, uint8_t ch) {
    uint8_t p[RL_CHANNEL_SIZE];
    struct rl_channel c;
    uint32_t elapsed = 0;
    int err = r_set_channel(ch, &elapsed);
    memset(&c, 0, sizeof(c));
    c.channel = r_channel();
    c.ok = err == 0;
    c.tx_allowed = (uint8_t)tx_channel_ok(r_channel());
    c.elapsed_us = elapsed;
    if (err) sv->st.v[RL_ST_CHANNEL_FAILURES]++;
    else sv->st.v[RL_ST_CHANNEL_SWITCHES]++;
    if (err || sv->channel_logs < MAX_CHANNEL_LOGS) {
        sv->channel_logs++;
        printf("serve: channel %u requested: %s, now %u, %u us%s\n", ch, err ? mt792x_strerror(err) : "ok",
               r_channel(), elapsed, sv->channel_logs == MAX_CHANNEL_LOGS ? " (further switches not logged)" : "");
    }
    rl_put_channel(p, sizeof(p), &c);
    (void)send_msg(sv, RL_CHANNEL, p, sizeof(p), NULL, 0, 0);
}

/* Handles one complete client message. Returns -1 on a protocol error. */
static int handle_msg(struct server *sv, const uint8_t *msg, size_t len) {
    struct rl_header h;
    const uint8_t *p = msg + RL_HEADER_SIZE;
    if (rl_get_header(msg, len, &h) != 0) return -1;
    sv->st.v[RL_ST_MESSAGES_IN]++;
    switch (h.type) {
    case RL_TX:
        do_tx(sv, p, h.length);
        return 0;
    case RL_SET_CHANNEL:
        if (h.length != 1) return -1;
        do_set_channel(sv, p[0]);
        return 0;
    case RL_SET_BSSID_FILTER: {
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        if (h.length != 6) return -1;
        memcpy(sv->filter, p, 6);
        sv->filter_on = memcmp(sv->filter, zero, 6) != 0;
        printf("serve: BSSID filter %s\n", sv->filter_on ? "set" : "off (everything forwarded)");
        return 0;
    }
    case RL_HEARTBEAT:
        return h.length == 0 ? 0 : -1;
    case RL_STATS_REQ: {
        uint8_t s[RL_STATS_SIZE];
        if (h.length != 0) return -1;
        sv->st.v[RL_ST_UPTIME_MS] = (uint32_t)((qpc_us() - serve_t_start) / 1000u);
        rl_put_stats(s, sizeof(s), &sv->st);
        (void)send_msg(sv, RL_STATS, s, sizeof(s), NULL, 0, 0);
        return 0;
    }
    default:
        return -1; /* a radio -> client type */
    }
}

/* Completes / restarts the overlapped read. Returns 1 if a message was handled, 0 if none, -1 on error
 * (client gone or protocol violation; sv->client_gone / sv->bye_reason say which). */
static int pump_read(struct server *sv) {
    DWORD n = 0;
    int handled = 0;
    if (sv->reading) {
        if (!GetOverlappedResult(sv->pipe, &sv->rov, &n, FALSE)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return 0;
            if (e == ERROR_MORE_DATA) {
                sv->bye_reason = RL_BYE_PROTOCOL;
                return -1;
            }
            sv->client_gone = 1;
            return -1;
        }
        sv->reading = 0;
        if (handle_msg(sv, sv->rbuf, n) != 0) {
            sv->bye_reason = RL_BYE_PROTOCOL;
            return -1;
        }
        handled = 1;
    }
    ResetEvent(sv->rov.hEvent);
    if (ReadFile(sv->pipe, sv->rbuf, sizeof(sv->rbuf), &n, &sv->rov)) {
        /* completed at once: handled on the next call through GetOverlappedResult */
        sv->reading = 1;
        return handled;
    }
    switch (GetLastError()) {
    case ERROR_IO_PENDING:
        sv->reading = 1;
        return handled;
    case ERROR_MORE_DATA:
        sv->bye_reason = RL_BYE_PROTOCOL;
        return -1;
    default:
        sv->client_gone = 1;
        return -1;
    }
}

/* Hardware sessions: SYSTEM and Administrators only. Loopback touches no hardware, so interactive users may
 * connect too (offline tests run unelevated). */
static HANDLE create_pipe(int allow_interactive) {
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE h;
    /* protected DACL: full access for LocalSystem and Administrators (+ interactive users in loopback) */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            allow_interactive ? "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)" : "D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd,
                                                              NULL))
        return INVALID_HANDLE_VALUE;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    h = CreateNamedPipeA(RL_PIPE_NAME,
                         PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                         PIPE_OUT_BUFFER, PIPE_IN_BUFFER, 0, &sa);
    LocalFree(sd);
    return h;
}

/* Waits up to 'ms' for the client. Returns 1 connected, 0 timeout, -1 error. */
static int wait_client(HANDLE pipe, DWORD ms) {
    OVERLAPPED ov;
    int rc;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;
    if (ConnectNamedPipe(pipe, &ov)) rc = 1;
    else {
        DWORD e = GetLastError(), n;
        if (e == ERROR_PIPE_CONNECTED) rc = 1;
        else if (e != ERROR_IO_PENDING) rc = -1;
        else if (WaitForSingleObject(ov.hEvent, ms) == WAIT_OBJECT_0) rc = GetOverlappedResult(pipe, &ov, &n, FALSE) ? 1 : -1;
        else {
            CancelIo(pipe);
            rc = 0;
        }
    }
    CloseHandle(ov.hEvent);
    return rc;
}

static void print_stats(const char *what, const struct rl_stats *s) {
    printf("%s: RX frames %u, forwarded %u, filtered %u, bad FCS %u, own %u, pipe full %u; TX submitted %u, refused %u, "
           "done %u, lost %u, ACKed %u, not ACKed %u; channel switches %u (failed %u); messages in %u\n",
           what, s->v[RL_ST_RX_FRAMES], s->v[RL_ST_RX_FORWARDED], s->v[RL_ST_RX_FILTERED], s->v[RL_ST_RX_BAD_FCS],
           s->v[RL_ST_RX_OWN], s->v[RL_ST_RX_PIPE_FULL], s->v[RL_ST_TX_SUBMITTED], s->v[RL_ST_TX_REFUSED],
           s->v[RL_ST_TX_DONE], s->v[RL_ST_TX_LOST], s->v[RL_ST_TX_ACKED], s->v[RL_ST_TX_NOT_ACKED],
           s->v[RL_ST_CHANNEL_SWITCHES], s->v[RL_ST_CHANNEL_FAILURES], s->v[RL_ST_MESSAGES_IN]);
}

int serve(int argc, char **argv) {
    static struct server sv; /* large: keep it off the stack */
    struct session s;
    struct tx_cli cli;
    unsigned max_seconds = 120, wait_seconds = 60;
    int rc = 1, st;
    long chl = strtol(argv[2], NULL, 10);

    tx_cli_init(&cli);
    /* F-019 / F-020 / F-021: unicast data on ALTX0 at a fixed rate is required (AC queues held it 115-560 ms and
     * the firmware picked CCK 1 Mb/s, dead on 5 GHz). Default for serve; --data-fw-rate-ac restores mt76. */
    cli.extra.tx_data_altx = 1;
    cli.extra.tx_data_fixed_rate = 1;
    memset(&sv, 0, sizeof(sv));
    memset(&s, 0, sizeof(s));
    sv.pipe = INVALID_HANDLE_VALUE;
    memcpy(sv.filter, awdl_bssid, 6);
    sv.filter_on = 1;
    for (int i = 5; i < argc; i++) {
        int used = tx_cli_parse(&cli, argc, argv, &i);
        if (used < 0) return 2;
        if (used) continue;
        if (strcmp(argv[i], "--max-seconds") == 0 && i + 1 < argc) max_seconds = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--wait-client") == 0 && i + 1 < argc) wait_seconds = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-filter") == 0) sv.filter_on = 0;
        else if (strcmp(argv[i], "--loopback") == 0) loopback = 1;
        else if (strcmp(argv[i], "--data-fw-rate-ac") == 0) cli.extra.tx_data_altx = cli.extra.tx_data_fixed_rate = 0;
        else if (strcmp(argv[i], "--data-rate") == 0 && i + 1 < argc) {
            long mbps = strtol(argv[++i], NULL, 10);
            uint8_t r500 = (uint8_t)(mbps * 2);
            if (mbps != 6 && mbps != 9 && mbps != 12 && mbps != 18 && mbps != 24 && mbps != 36 && mbps != 48 && mbps != 54) {
                printf("RESULT: FAIL - --data-rate must be an OFDM rate: 6 9 12 18 24 36 48 54\n");
                return 2;
            }
            sv.data_rate = tx_rate_from_500k(r500, MT792X_BAND_5GHZ);
            sv.data_rate_500k = r500;
        }
        else {
            printf("RESULT: FAIL - unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (max_seconds < 5 || max_seconds > 900) {
        printf("RESULT: FAIL - --max-seconds must be 5..900\n");
        return 2;
    }
    if (wait_seconds < 1 || wait_seconds > 300) {
        printf("RESULT: FAIL - --wait-client must be 1..300\n");
        return 2;
    }
    if (outq_init(&sv.q) != 0) {
        printf("RESULT: FAIL - out of memory\n");
        outq_free(&sv.q);
        return 2;
    }
    sv.rov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    /* The pipe first: if another server holds the name, nothing is touched. */
    sv.pipe = create_pipe(loopback);
    if (sv.pipe == INVALID_HANDLE_VALUE || !sv.rov.hEvent) {
        printf("RESULT: FAIL - cannot create %s (error %lu; is another radiod serving?)\n", RL_PIPE_NAME, GetLastError());
        outq_free(&sv.q);
        if (sv.rov.hEvent) CloseHandle(sv.rov.hEvent);
        return 2;
    }

    if (loopback) {
        /* no hardware: nothing is opened, read or written outside this process */
        if (chl < 1 || chl > 177 || !rl_channel_freq((uint8_t)chl, band_of((uint8_t)chl) == MT792X_BAND_5GHZ ? 1u : 0u)) {
            printf("RESULT: FAIL - unknown channel %ld\n", chl);
            rc = 2;
            goto done_pipe;
        }
        lb_channel = (uint8_t)chl;
        lb_band = band_of(lb_channel);
        if (cli.mac_random) memcpy(lb_own, cli.mac, 6);
        printf("serve: LOOPBACK - synthetic radio, no hardware access\n");
    } else {
        st = tx_start(&s, argv[3], argv[4], chl, &cli, 0, cli.mac_random ? cli.mac : NULL);
        if (st > 0) {
            rc = 2;
            goto done_pipe;
        }
        if (st < 0) goto out;
    }
    {
        uint64_t t_start, t_end, last_msg, next_status;
        uint8_t hp[RL_HELLO_SIZE];
        struct rl_hello hello;
        int c;

        sv.own = r_own();
        printf("serve: channel %u, address ", r_channel());
        print_masked_addr(sv.own);
        printf(" (%s), BSSID filter %s, TX power %s, unicast data rate %s%u Mb/s; waiting up to %u s for a client on %s\n",
               cli.mac_random ? "random" : loopback ? "fixed" : "chip", sv.filter_on ? "AWDL" : "off", tx_power_desc(&cli),
               sv.data_rate ? "" : "as asked, OWL ", sv.data_rate ? sv.data_rate_500k / 2u : 12u, wait_seconds, RL_PIPE_NAME);
        c = wait_client(sv.pipe, wait_seconds * 1000u);
        if (c <= 0) {
            printf("RESULT: FAIL - %s\n", c == 0 ? "no client connected in time" : "pipe connect failed");
            goto out;
        }
        memset(&hello, 0, sizeof(hello));
        hello.version = RL_VERSION;
        hello.channel = r_channel();
        hello.band = to_rl_band(r_band());
        hello.width_mhz = 20;
        hello.tx_allowed = (uint8_t)tx_channel_ok(r_channel());
        memcpy(hello.addr, sv.own, 6);
        hello.max_frame = (uint16_t)MT792X_TX_MAX_FRAME;
        hello.lease_ms = RL_LEASE_MS;
        rl_put_hello(hp, sizeof(hp), &hello);
        send_msg(&sv, RL_HELLO, hp, sizeof(hp), NULL, 0, 0);
        printf("serve: client connected, serving for at most %u s\n", max_seconds);

        t_start = serve_t_start = qpc_us();
        t_end = t_start + (uint64_t)max_seconds * 1000000u;
        last_msg = t_start;
        next_status = t_start + 10000000u;
        for (;;) {
            uint64_t now = qpc_us();
            int busy = 0, r;
            if (now >= t_end) {
                sv.bye_reason = RL_BYE_TIME_LIMIT;
                printf("serve: time limit reached\n");
                break;
            }
            if (now - last_msg > (uint64_t)RL_LEASE_MS * 1000u) {
                printf("serve: lease expired (no message from the client for %u ms)\n", RL_LEASE_MS);
                break;
            }
            r = pump_read(&sv);
            if (r < 0) {
                printf("serve: %s\n", sv.client_gone ? "client disconnected" : "protocol error: malformed client message");
                break;
            }
            if (r > 0) {
                last_msg = now;
                busy = 1;
            }
            r = pump_rx(&sv);
            if (r < 0) {
                sv.bye_reason = RL_BYE_RADIO_ERROR;
                printf("serve: receive error: %s (%d)\n", mt792x_strerror(r), r);
                break;
            }
            busy |= r > 0;
            r = pump_tx_done(&sv);
            if (r < 0) {
                sv.bye_reason = RL_BYE_RADIO_ERROR;
                printf("serve: TX completion error: %s (%d)\n", mt792x_strerror(r), r);
                break;
            }
            busy |= r > 0;
            if (flush_out(&sv) != 0) {
                sv.client_gone = 1;
                printf("serve: client disconnected (write failed)\n");
                break;
            }
            if (now >= next_status) {
                sv.st.v[RL_ST_UPTIME_MS] = (uint32_t)((now - t_start) / 1000u);
                print_stats("serve status", &sv.st);
                next_status += 10000000u;
            }
            if (!busy) spin_us(100);
        }
        sv.st.v[RL_ST_UPTIME_MS] = (uint32_t)((qpc_us() - t_start) / 1000u);
        if (!sv.client_gone && sv.bye_reason) {
            uint8_t b[4] = {(uint8_t)sv.bye_reason, 0, 0, 0};
            uint64_t until = qpc_us() + 200000u;
            send_msg(&sv, RL_BYE, b, sizeof(b), NULL, 0, 0);
            while (sv.q.count && qpc_us() < until && flush_out(&sv) == 0) spin_us(500);
        }
        print_stats("serve summary", &sv.st);
        printf("unicast data: %u frame(s), %u transmission(s), %u without TX status; rates in TX status:", sv.dtx.frames,
               sv.dtx.transmissions, sv.dtx.no_txs);
        for (int r = 255; r > 0; r--)
            if (sv.dtx.by_rate_500k[r]) printf(" %u%s Mb/s %u,", r / 2u, r % 2u ? ".5" : "", sv.dtx.by_rate_500k[r]);
        if (sv.dtx.by_rate_500k[0]) printf(" unknown %u,", sv.dtx.by_rate_500k[0]);
        printf(" HT/VHT %u\n", sv.dtx.other_mode);
        if (sv.bye_reason == RL_BYE_RADIO_ERROR) printf("RESULT: FAIL - radio error while serving\n");
        else if (sv.bye_reason == RL_BYE_PROTOCOL) printf("RESULT: FAIL - the client broke the protocol\n");
        else {
            printf("RESULT: PASS - served %u s, %u frame(s) forwarded, %u TX submitted\n",
                   sv.st.v[RL_ST_UPTIME_MS] / 1000u, sv.st.v[RL_ST_RX_FORWARDED], sv.st.v[RL_ST_TX_SUBMITTED]);
            rc = 0;
        }
    }

out:
    if (!loopback) session_close(&s);
done_pipe:
    if (sv.reading || sv.q.writing) CancelIo(sv.pipe);
    DisconnectNamedPipe(sv.pipe);
    CloseHandle(sv.pipe);
    CloseHandle(sv.rov.hEvent);
    outq_free(&sv.q);
    return rc;
}
