/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fake_chip.h - a simulated MT7921 on top of the mock HAL, strict enough to catch
 * host mistakes. Behaviour follows docs/MT7921-BRINGUP.md (registers, rings, MCU
 * messages); every deviation by the host is recorded as a "violation".
 *
 * What it models:
 *  - LPCTL own/sync handshake, TOP_LPCR_HOST_BAND0 driver own (via L1)
 *  - the L1 remap window (BAR 0x40000-0x4ffff routed by MT_HIF_REMAP_L1 bits 15:0):
 *    chip id / bound / rev, WFSYS reset register with INIT_DONE after a delay
 *  - CONN_ON_MISC firmware state (1 after reset, 3 after FW_START)
 *  - GLO_CFG enable/busy bits, WFDMA logic reset, RST_DTX_PTR
 *  - ring registers of TX 0/16/17 and RX 0/2/4; consuming TX descriptors from ring memory by
 *    translating bus addresses back into the HAL region (violation if outside / misaligned /
 *    malformed); MCU command parsing per spec 4; patch semaphore and download state; events
 *    written into RX rings only if the host armed them correctly; DMA_DONE write-back and
 *    dma_idx like the real chip.
 */
#ifndef MT792X_TEST_FAKE_CHIP_H
#define MT792X_TEST_FAKE_CHIP_H

#include <stdint.h>

#include "hal_mock.h"

#define FAKE_MAX_DL        64u
#define FAKE_MAX_CMDS      512u
#define FAKE_MAX_ENABLING  65536u

struct fake_dl {
    int phase;          /* 0 = patch (semaphore held), 1 = RAM */
    uint8_t cmd;        /* 0x05 or 0x01 */
    uint32_t addr, len, mode;
    uint8_t *data;      /* received bytes (malloc'd, len bytes) */
    uint32_t got;
    uint32_t chunks;
};

/* command formats as the fake decodes them from the header */
#define FAKE_FMT_CMD 0u    /* 64-byte header, set_query 3 (bring-up) */
#define FAKE_FMT_CE  1u    /* 64-byte header, ext_cid 0, set_query 0/1 */
#define FAKE_FMT_EXT 2u    /* 64-byte header, cid 0xed, ext_cid != 0 */
#define FAKE_FMT_UNI 3u    /* 48-byte header */
/* key for knobs: FAKE_KEY(fmt, id, ext) */
#define FAKE_KEY(fmt, id, ext) (((uint32_t)(fmt) << 16) | ((uint32_t)(id) << 8) | (uint32_t)(ext))

struct fake_cmd {
    uint8_t cid, seq;
    uint8_t fmt, ext;   /* FAKE_FMT_*, ext id for EXT */
    uint8_t set_query;  /* 64-byte formats only */
    uint32_t total_len;
    uint32_t ring;      /* 17 or 16 (16 = FW_SCATTER chunk) */
    uint8_t *raw;       /* copy of the whole command as fetched by DMA (TX17 only), malloc'd */
    uint32_t pl_first;  /* first payload byte, for quick checks */
};

/* One frame the chip would have radiated (TX ring 0), as decoded from the TXD/TXP it fetched. */
#define FAKE_MAX_AIR 1024u
struct fake_air {
    uint8_t *bytes;     /* frame as fetched through the TXP pointer (malloc'd) */
    uint32_t len;
    uint32_t txd[8];    /* raw TXD words */
    uint8_t txp[32];    /* raw hardware TXP */
    uint16_t token;
    uint8_t fix_rate;   /* TXD2 FIX_RATE */
    uint16_t rate;      /* TXD6 TX_RATE (valid if fix_rate) */
    uint8_t no_ack, q_idx, wcid, omac, pid, rem_tx;
    uint8_t channel, band;
    uint64_t time_us;
};

/* how the simulated chip completes a TX ring 0 frame */
#define FAKE_TX_ACK    0u   /* TX-free (status 0, count 1); TXS "acked" if a PID asked for it */
#define FAKE_TX_NOACK  1u   /* TX-free (status 0, count = REM_TX_COUNT); TXS with ACK_TIMEOUT */
#define FAKE_TX_SILENT 2u   /* never reported */
#define FAKE_TX_DEFER  3u   /* reported only by fake_tx_release() */
#define FAKE_MAX_DEFER 64u

struct fake_chip {
    struct mock *m;

    /* ---- configuration / fault knobs (set after fake_init, before the run) ---- */
    uint32_t chip_id, hw_bound, hw_rev;
    uint32_t remap_initial;
    int fw_own_never;           /* SET_OWN never sets OWN_SYNC */
    unsigned drv_own_fail_count;/* the first N CLR_OWN requests are ignored */
    int drv_own_never;
    int wfsys_never_done;
    uint32_t wfsys_done_delay_us;
    int dma_busy_stuck;
    int drv_own2_never;
    int fw_state_never_ready;
    int sem_get_result;         /* -1 = natural behaviour */
    int sem_release_result;     /* -1 = natural behaviour */
    int patch_loaded;           /* semaphore answers PATCH_IS_DL */
    unsigned drop_response_nth; /* 1-based TX17 command index whose response is not sent; 0 = none */
    unsigned short_response_nth;/* 1-based TX17 command index answered with a 33-byte event */
    int finish_result;
    unsigned fw_start_ring;     /* RX ring for the FW_START response (0 or 4) */
    unsigned event_ring;        /* RX ring for all other responses */
    int rx_never_armed;         /* host RX cpu_idx writes have no effect */
    int n9_never;
    int tx_stall_ring;          /* -1 = none; kicks on this TX ring are never consumed */
    uint32_t bus_dead_after;    /* 0 = never; after this many register accesses the bus reads all ones */
    int inject_unsolicited;     /* before each response: DBG_MSG event with seq 0 */
    int inject_stale;           /* before each response: a response with another seq */
    int nic_power_responds;     /* answer NIC_POWER_CTRL although mt76 does not wait for it */
    /* post-boot / sniffer knobs */
    const uint8_t *nic_reply;   /* GET_NIC_CAPAB payload to return (NULL = the default one) */
    uint32_t nic_reply_len;
    uint32_t uni_status_dev;    /* status in the DEV_INFO_UPDATE reply */
    uint32_t uni_status_bss;    /* status in the BSS_INFO_UPDATE reply */
    int uni_wrong_cid;          /* first DEV_INFO reply carries a wrong cid before the right one */
    uint32_t drop_key;          /* FAKE_KEY of a command never answered (0 = none) */
    int drop_all_from_cmd;      /* 1-based TX17 command index from which nothing is answered (0 = never) */
    int tune_by;                /* 0: SET_RX_PATH/CHANNEL_SWITCH and sniffer config both retune;
                                   1: only SET_RX_PATH/CHANNEL_SWITCH; 2: only sniffer config */
    uint32_t tune_delay_us;     /* virtual time spent before answering a retuning command */
    int wtbl_busy_never;        /* WTBL_UPDATE BUSY never clears */
    /* raw TX knobs */
    unsigned txc_ring;          /* RX ring for TX-free / TXS packets (default 4) */
    unsigned tx_mode;           /* FAKE_TX_* */
    int txs_first;              /* TXS before the TX-free notification */
    int txs_never;              /* no TXS even when a PID asks for it */
    int txs_via_event;          /* TXS inside an MCU TX_DONE event (eid 0x0f) instead of PKT_TYPE_TXS */
    int tx_no_consume;          /* TX ring 0 kicks are never consumed (descriptors stay with the host) */

    /* ---- state ---- */
    uint32_t accesses;
    int dead;
    int dead_read_seen;
    unsigned writes_after_dead_read;
    int own_sync;
    int driver_owned_once;
    unsigned clr_own_requests;
    unsigned set_own_writes;
    int wfsys_asserted;
    uint64_t wfsys_assert_time, wfsys_release_time;
    int wfsys_released_once;
    unsigned wfsys_resets;
    int top_lpcr_fw_own;
    int nic_powered;
    int fw_started;
    uint32_t tx_dma[18];
    uint32_t rx_dma[8], rx_cpu[8];
    int sem_held, patch_done;
    uint32_t seq_counter;       /* emulates the host's msg_seq to check every sequence number */
    unsigned cmd_count;         /* TX17 commands */
    struct fake_cmd cmds[FAKE_MAX_CMDS];
    unsigned n_cmds;            /* TX17 commands and TX16 chunks, in order */
    struct fake_dl dl[FAKE_MAX_DL];
    unsigned n_dl;
    uint32_t fw_start_option, fw_start_addr;
    unsigned events_sent[8];
    unsigned events_dropped;
    unsigned responses_dropped_by_knob;
    uint32_t enabling_idx[FAKE_MAX_ENABLING]; /* trace indices of DMA-enabling writes */
    unsigned n_enabling;
    unsigned tx_kicks[18];
    uint32_t fault_idx;         /* trace index at which a knob fault was injected/observed; 0 = none */
    /* post-boot / sniffer state */
    int nic_queried, efuse_mode_set, thermal_set, fwlog_on, rts_set;
    int keep_full_pwr;          /* -1 = never configured, else the KeepFullPwr value */
    int mac_enabled;
    unsigned chan_domain_n2, chan_domain_n5;
    uint8_t channel, band;      /* what the radio is tuned to (per tune_by) */
    unsigned tunes;             /* number of effective retunes */
    int sniffer_on;
    int drop_err;               /* last sniffer config drop_err */
    int dev_active, bss_active;
    uint8_t dev_mac[6];
    uint32_t rx_filter_fif;
    int other_beacon_drop_cleared;
    unsigned bss_aborts;
    int radio_led;              /* last ID_RADIO_ON_OFF_CTRL ctrlid */
    unsigned rf_pin_inits;
    unsigned wtbl_updates;
    unsigned rx_injected[8], rx_inject_dropped[8];
    unsigned uni_wrong_cid_sent;
    /* pair vif (OMAC 1) */
    int dev1_active, bss1_active;
    uint8_t dev1_mac[6];
    /* raw TX state */
    struct fake_air air[FAKE_MAX_AIR];
    unsigned n_air;
    uint8_t tok_inflight[4096];  /* bitmap of 32768 tokens */
    unsigned tx_inflight;
    struct { uint16_t token; uint8_t pid, wcid, ack, rem; uint16_t rate, sn; } deferred[FAKE_MAX_DEFER];
    unsigned n_deferred;
    unsigned txfree_sent, txs_sent;
    unsigned pwr_msgs[2];       /* SET_RATE_TX_POWER messages per band (0 = 2.4, 1 = 5 GHz) */
    unsigned pwr_channels[2];   /* channel entries per band */
    int8_t pwr_value[2][256];   /* limit programmed per channel (0 = never) */
    unsigned reg_reads;
    int pwr_pending_read;       /* a SET_RATE_TX_POWER waits for its REG_READ */

    /* ---- violations ---- */
    unsigned violations;
    char first_violation[256];
};

/* Wires the fake into the mock (hooks + initial register values). Defaults model the
 * MEASURED start state: LPCTL 0, MISC 1, GLO_CFG 0x1010b870, remap 0x18451800. */
void fake_init(struct fake_chip *f, struct mock *m);
void fake_free(struct fake_chip *f);

/* Bytes the chip "has" for a load address after downloads (NULL if never downloaded). */
const struct fake_dl *fake_find_dl(const struct fake_chip *f, uint32_t addr);

/* Number of DMA-enabling writes with trace index > idx. */
unsigned fake_enabling_after(const struct fake_chip *f, uint32_t idx);

/* RX injection (as the chip would DMA a packet): returns 1 if written, 0 if the ring had no armed
 * descriptor (dropped, like a full ring). 'chunk' > 0 splits the packet over several descriptors
 * (LAST_SEC0 only on the last). */
int fake_inject(struct fake_chip *f, unsigned ring, const uint8_t *pkt, uint32_t len);
int fake_inject_frag(struct fake_chip *f, unsigned ring, const uint8_t *pkt, uint32_t len, uint32_t chunk);
/* A descriptor completion that claims 'claim_len' bytes (0 or > buffer) without data. */
int fake_inject_badlen(struct fake_chip *f, unsigned ring, uint32_t claim_len);
/* Index of the n-th (0-based) TX17 command with this key at or after 'from', or -1. */
int fake_find_cmd(const struct fake_chip *f, uint32_t key, unsigned from, unsigned nth);

/* Emits the completions held back by FAKE_TX_DEFER (in submission order). Returns how many. */
unsigned fake_tx_release(struct fake_chip *f);
/* Emits a TX-free notification for one token (as a late or spurious report would arrive). */
int fake_tx_free_token(struct fake_chip *f, uint16_t token, uint8_t wcid, uint32_t status, uint32_t count);

/* GLO_CFG as the chip holds it. */
uint32_t fake_glo_cfg(const struct fake_chip *f);
/* Current low-level firmware state register (CONN_ON_MISC). */
uint32_t fake_misc(const struct fake_chip *f);

#endif /* MT792X_TEST_FAKE_CHIP_H */
