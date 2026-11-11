/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mt792x/mt792x.h - public entry point of the portable MT7921 chip logic.
 *
 * Staged firmware bring-up of an MT7921 (PCI 14c3:7961) from a polling,
 * interrupt-less host, following docs/MT7921-BRINGUP.md (steps S0-S23, which
 * mirror Linux mt76: openwrt/mt76 @ 0936617, BSD-3-Clause-Clear).
 *
 * Typical use:
 *
 *     static struct mt792x dev;                // ~6 KiB, caller-owned
 *     mt792x_init(&dev, hal);                  // layout + zero the DMA region, no register access
 *     err = mt792x_bringup(&dev, &fw, MT792X_STAGE_FIRMWARE);
 *     ...
 *     mt792x_shutdown(&dev);                   // always; safe after any outcome
 *
 * A host that can lose the process must also arrange for mt792x_stop_ops() to be
 * replayed when it dies (the Windows proxy does that on handle close).
 */
#ifndef MT792X_MT792X_H
#define MT792X_MT792X_H

#include <stddef.h>
#include <stdint.h>

#include "mt792x/fw.h"
#include "mt792x/hal.h"
#include "mt792x/layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns the library version as a static string, e.g. "0.1.0". */
const char *mt792x_version(void);

#define MT792X_CHIP_ID 0x7961u

/* How far mt792x_bringup goes; for gated hardware tests. */
enum mt792x_stage {
    MT792X_STAGE_OWNERSHIP = 1, /* S1-S3: fw-own/drv-own handshake, chip id check. No resets, no DMA. */
    MT792X_STAGE_RESET = 2,     /* + S4-S7: WFSYS reset, interrupts masked, DMA disabled */
    MT792X_STAGE_RINGS = 3,     /* + S8-S13: rings programmed and DMA enabled, no command sent */
    MT792X_STAGE_MCU = 4,       /* + S14-S16: NIC_POWER_CTRL, wait FW state, patch semaphore get (+ release) */
    MT792X_STAGE_FIRMWARE = 5,  /* + S17-S23: patch + RAM download, firmware ready */
    MT792X_STAGE_POSTBOOT = 6   /* + S24-S31: NIC capability, EFUSE mode, MAC init, KeepFullPwr 1, MAC enable */
};

/* Error codes: 0 = success, negative = failure. */
enum mt792x_error {
    MT792X_OK = 0,
    MT792X_E_INVAL = -1,          /* bad argument */
    MT792X_E_STATE = -2,          /* call not allowed in the current state (bring-up runs once per init) */
    MT792X_E_LAYOUT = -3,         /* the HAL DMA region cannot hold the ring layout */
    MT792X_E_DMA_ADDR = -4,       /* a bus address outside the DMA region was requested (internal guard) */
    MT792X_E_BUS_DEAD = -5,       /* all-ones register read or ring index >= ring size */
    MT792X_E_FW_OWN = -6,         /* S1 */
    MT792X_E_DRV_OWN = -7,        /* S2 */
    MT792X_E_CHIP_ID = -8,        /* S3 */
    MT792X_E_WFSYS_RESET = -9,    /* S4 */
    MT792X_E_INT_MASK = -10,      /* S5: HOST_INT_ENA does not read back 0 */
    MT792X_E_DMA_BUSY = -11,      /* S7 */
    MT792X_E_RING_PROGRAM = -12,  /* S8: ring registers do not read back */
    MT792X_E_DMA_ENABLE = -13,    /* S10: GLO_CFG enable bits do not read back */
    MT792X_E_DRV_OWN2 = -14,      /* S12 */
    MT792X_E_TX_TIMEOUT = -15,    /* the chip did not consume a TX descriptor */
    MT792X_E_RING_FULL = -16,
    MT792X_E_MCU_TIMEOUT = -17,   /* no response event within 3 s */
    MT792X_E_MCU_EVENT = -18,     /* malformed response event */
    MT792X_E_FW_STATE = -19,      /* S15: MCU not ready for download */
    MT792X_E_SEM_GET = -20,       /* S16 */
    MT792X_E_SEM_RELEASE = -21,   /* S20 */
    MT792X_E_PATCH_FINISH = -22,  /* S19 */
    MT792X_E_N9_RDY = -23,        /* S23 */
    MT792X_E_FW_MISSING = -24,    /* stage needs firmware files that were not given */
    /* firmware file validation */
    MT792X_E_FW_TRUNCATED = -30,  /* file shorter than its fixed header/trailer */
    MT792X_E_FW_COUNT = -31,      /* section/region count zero, above the limit, or table outside the file */
    MT792X_E_FW_SECTION_TYPE = -32,
    MT792X_E_FW_BOUNDS = -33,     /* section/region data outside the file or inside the header/table */
    MT792X_E_FW_OVERLAP = -34,    /* file ranges or load-address ranges overlap */
    MT792X_E_FW_LENGTH = -35,     /* zero length or address + length wraps 32 bits */
    MT792X_E_FW_ENC_TYPE = -36,   /* unknown patch encryption type */
    MT792X_E_FW_OVERRIDE = -37,   /* override flag with address 0 (cannot be expressed in FW_START) */
    /* post-boot / sniffer / RX */
    MT792X_E_MCU_STATUS = -40,    /* a UNI command answered with a non-zero status */
    MT792X_E_NIC_CAPAB = -41,     /* S24: GET_NIC_CAPAB reply unusable (shorter than its header) */
    MT792X_E_NO_MAC = -42,        /* no usable MAC address (none given, none valid from the chip) */
    MT792X_E_FW_DEAD = -43,       /* liveness check: CONN_ON_MISC no longer reports N9_RDY */
    MT792X_E_DMA_REINIT = -44,    /* liveness check: WFDMA_DUMMY_CR NEED_REINIT cleared (spec R6) */
    /* raw TX (spec 12) */
    MT792X_E_TX_DENIED = -45,     /* TX not enabled (cfg.allow_tx), sniffer not started, or channel not allowed */
    MT792X_E_TX_BUSY = -46,       /* no free TX slot / completion record space (poll, then try again) */
    MT792X_E_TX_FRAME = -47       /* frame rejected by validation (length, type, address, flags) */
};

/* How the radio is tuned (spec 11.6). AUTO follows mt76: firmware with the CNM feature bit in the
 * RAM file (MT792X_FW_CAP_CNM) gets the sniffer config TLV, otherwise EXT CHANNEL_SWITCH. */
enum mt792x_tune_mode {
    MT792X_TUNE_AUTO = 0,
    MT792X_TUNE_CHANNEL_SWITCH = 1, /* mt7921_set_channel -> MCU_EXT_CMD(CHANNEL_SWITCH) (non-CNM firmware) */
    MT792X_TUNE_SNIFFER_CONFIG = 2, /* mt7921_change_chanctx -> sniffer enable + sniffer config TLV (CNM firmware) */
    MT792X_TUNE_BOTH = 3            /* experiment: CHANNEL_SWITCH, then sniffer enable + config */
};

/* How the 4-byte FCS at the end of a received frame is treated (spec 11.8). */
enum mt792x_fcs_mode {
    MT792X_FCS_MT76 = 0,   /* mt76: hardware does not declare RX_INCLUDES_FCS -> frame is taken as FCS-less */
    MT792X_FCS_STRIP = 1,  /* always remove the last 4 bytes */
    MT792X_FCS_AUTO = 2    /* remove the last 4 bytes only if they are the CRC-32 of the rest */
};

/* Behaviour switches for points the spec marks ASSUMPTION / TODO-VERIFY. All default to 0. */
struct mt792x_options {
    uint8_t lenient_fw_state;     /* S15 timeout: 0 = fail with MT792X_E_FW_STATE, 1 = warn and go on (mt76) */
    uint8_t skip_pcie_mac_int;    /* S6: 1 = do not write MT_PCIE_MAC_INT_ENABLE = 0xff (risk R1) */
    uint8_t skip_aspm_sleep;      /* S2: 1 = no 2.5 ms sleep after CLR_OWN (mt76 sleeps only with ASPM) */
    uint8_t skip_mcu2host_sw_int; /* S10: 1 = do not set MT_MCU2HOST_SW_INT_ENA bit 0 */
    uint8_t strict_hw_rev;        /* S3: 1 = fail unless HW_REV & 0xff == 0x10 (as measured) */
    /* post-boot / sniffer (spec 11) */
    uint8_t no_rx_hdr_trans;      /* S28: 1 = do not set MT_MDP_DCR0_RX_HDR_TRANS_EN (mt76 sets it) */
    uint8_t skip_fw_log;          /* S26: 1 = do not send FWLOG_2_HOST 1 (mt76 sends it) */
    uint8_t skip_thermal_protect; /* S25: 1 = do not send the DowngradeTxStreamTemp chip config */
    uint8_t skip_chan_domain;     /* SN1: 1 = do not send SET_CHAN_DOMAIN */
    uint8_t skip_radio_ctrl;      /* SN3: 1 = do not send ID_RADIO_ON_OFF_CTRL 1/2 */
    uint8_t skip_add_dev;         /* SN5: 1 = no DEV_INFO / BSS_INFO for the monitor interface */
    uint8_t tune_mode;            /* enum mt792x_tune_mode */
    uint8_t fcs_mode;             /* enum mt792x_fcs_mode */
    uint8_t antenna_mask;         /* 0 = from GET_NIC_CAPAB (BIT(nss) - 1), else override */
    uint8_t skip_set_timing;      /* 1 = no mt792x_mac_set_timeing after CHANNEL_SWITCH (mt76 runs it) */
    /* raw TX and the monitor vif (spec 12) */
    uint8_t vif_mode;             /* enum mt792x_vif_mode (affects RX-only operation too) */
    uint8_t tx_power_mode;        /* enum mt792x_tx_power_mode; only used when cfg.allow_tx */
    uint8_t tx_power_max_half_db; /* 0 = no extra cap; else every TX power limit is min(mt76 value, this), 0.5 dB units */
    uint8_t tx_spe_mode;          /* enum mt792x_tx_spe_mode */
    uint8_t tx_data_fixed_rate;   /* 1 = FIX_RATE on unicast data frames too (mt76: only non-data and group frames) */
    uint8_t tx_wcid_mode;         /* enum mt792x_tx_wcid_mode */
    uint16_t tx_timeout_ms;       /* no TX-free notification within this time -> LOST, slot quarantined; 0 = 2000 */
};

/* Which firmware "vifs" (OMAC + BSS entries) the sniffer start creates (spec 12.6). */
enum mt792x_vif_mode {
    MT792X_VIF_SINGLE = 0,    /* one monitor vif: OMAC 0 = our MAC (what the library always did) */
    MT792X_VIF_LINUX_PAIR = 1 /* what Linux does for a plain monitor + an active monitor (the configuration that
                                 ACKed in docs/FINDINGS.md F-001): OMAC 0 with address 00:00:00:00:00:00 (mac80211
                                 virtual monitor vif), then after tuning OMAC 1 = our MAC (the active vif) */
};

/* Per-rate TX power limits (MCU_CE_CMD(SET_RATE_TX_POWER), spec 12.5). */
enum mt792x_tx_power_mode {
    MT792X_TXPWR_MT76_PL = 0, /* send the limits mt76 computes for regulatory domain PL without a device tree */
    MT792X_TXPWR_NONE = 1     /* send nothing: firmware/efuse defaults (absolute power unknown) */
};

/* TXD7 SPE_IDX of fixed-rate frames (antenna path selection, spec 12.2). */
enum mt792x_tx_spe_mode {
    MT792X_SPE_MT76 = 0,      /* current mt76 (openwrt/mt76 @ 0936617): ant_to_spe[mask], 24 when 0 and 2 chains */
    MT792X_SPE_LINUX_7_0 = 1  /* Linux v7.0 mt76 (used in F-001): SPE_IDX left 0 for MT7921 */
};

/* TXD1 WLAN_IDX of transmitted frames (spec 12.3). */
enum mt792x_tx_wcid_mode {
    MT792X_WCID_VIF = 0,      /* mt792x_tx with a vif and no station: the vif's wcid (19, or 18 for OMAC 1) */
    MT792X_WCID_GLOBAL = 1    /* the global wcid 0 ("Beacon and mgmt frames should occupy wcid 0") */
};

/* One ring: host-side state. */
struct mt792x_ring {
    uint32_t regs;      /* BAR offset of the ring register block */
    uint32_t ndesc;
    uint32_t desc_off;  /* descriptor array offset in the DMA region */
    uint32_t buf_off;   /* first slot buffer offset; meaningful only if buf_size != 0 */
    uint32_t buf_size;  /* bytes per slot buffer (0 = ring has no buffers) */
    uint32_t head;
    uint32_t tail;
    uint32_t queued;
    uint32_t rearm_pending; /* RX: slots re-armed in memory but not yet announced through cpu_idx */
    uint8_t hw_idx;
    uint8_t is_rx;
    uint8_t in_frag;        /* RX: inside a multi-descriptor packet that is being dropped */
};

/* ---- post-boot: what GET_NIC_CAPAB reported (mt7921_mcu_get_nic_capability, mt7921/mcu.c:597-667) ---- */
struct mt792x_nic_caps {
    uint8_t valid;            /* a reply was parsed */
    uint8_t truncated;        /* parsing stopped early on a short TLV (mt76 stops the same way) */
    uint16_t n_element;       /* as announced */
    uint16_t n_parsed;
    uint32_t types_seen;      /* bit t set for every TLV type t < 32 seen */
    uint8_t has_mac;
    uint8_t mac[6];           /* MT_NIC_CAP_MAC_ADDR */
    uint8_t has_phy;          /* MT_NIC_CAP_PHY (>= 11 bytes) */
    uint8_t phy_nss, phy_hw_path, phy_max_bw, phy_he; /* raw fields of struct mt7921_phy_cap */
    uint8_t antenna_mask;     /* BIT(nss) - 1 */
    uint8_t has_2ghz, has_5ghz, has_6ghz;
    uint8_t has_chip_cap;
    uint64_t chip_cap;        /* MT_NIC_CAP_CHIP_CAP (MT792x_CHIP_CAP_* bits, mt792x.h:29-36) */
    uint8_t hw_ver_len, sw_ver_len; /* raw bytes of MT_NIC_CAP_HW_VER / SW_VER (format not parsed by mt76) */
    uint8_t hw_ver[16], sw_ver[16];
};

/* ---- sniffer ---- */
#define MT792X_BAND_2GHZ 0u
#define MT792X_BAND_5GHZ 1u
#define MT792X_BAND_6GHZ 2u

/* mac80211 FIF_* equivalents that mt7921_configure_filter maps to MT7921_FILTER_* (mt7921/main.c:666-691) */
#define MT792X_SNIFF_FCSFAIL   0x1u   /* deliver FCS-error frames (also clears drop_err in the sniffer config) */
#define MT792X_SNIFF_CONTROL   0x2u   /* deliver control frames */
#define MT792X_SNIFF_OTHER_BSS 0x4u   /* deliver frames of other BSSs */

struct mt792x_sniffer_cfg {
    uint8_t channel;          /* primary channel number */
    uint8_t band;             /* MT792X_BAND_2GHZ / MT792X_BAND_5GHZ */
    uint8_t bw;               /* must be 0 (20 MHz) for now */
    const uint8_t *mac;       /* 6 bytes for DEV_INFO, or NULL = the address GET_NIC_CAPAB reported */
    uint32_t flags;           /* MT792X_SNIFF_* */
    uint8_t allow_tx;         /* 1 = mt792x_tx_frame may transmit; only 2.4 GHz 1-13 and 5 GHz 36/40/44/48 are
                                 then accepted (sniffer start, channel switches and every frame are checked) */
};

/* ---- RX ---- */
/* mt792x_rx_frame.flags */
#define MT792X_RXF_FCS_ERR          0x0001u /* RXD1 FCS_ERR */
#define MT792X_RXF_ICV_ERR          0x0002u /* RXD1 ICV_ERR (mt76: RX_FLAG_ONLY_MONITOR) */
#define MT792X_RXF_TKIP_MIC_ERR     0x0004u
#define MT792X_RXF_HDR_TRANS        0x0008u /* RXD2 HDR_TRANS: data is 802.3, not 802.11 */
#define MT792X_RXF_TIMESTAMP        0x0010u /* group 2 present: timestamp valid */
#define MT792X_RXF_RATE             0x0020u /* group 3 present: rate fields and chain RSSI valid */
#define MT792X_RXF_GROUP5           0x0040u /* group 5 present (RCPI taken from it, as mt76) */
#define MT792X_RXF_G5_WITHOUT_G3    0x0080u /* RXD1 announces group 5 without group 3 (mt76 ignores it) */
#define MT792X_RXF_FCS_STRIPPED     0x0100u /* 4 bytes removed per fcs_mode */
#define MT792X_RXF_FCS_TRAILER      0x0200u /* the last 4 bytes are the CRC-32 of the rest (FCS present) */
#define MT792X_RXF_AMSDU            0x0400u /* RXD4 payload format != 0 */
#define MT792X_RXF_FRAG             0x0800u /* RXD2 FRAG */
#define MT792X_RXF_DECRYPTED        0x1000u /* SEC_MODE != 0 and no CLM/CM */
#define MT792X_RXF_UNICAST_TO_ME    0x2000u /* RXD3 ADDR_TYPE == U2M */
#define MT792X_RXF_LEN_MISMATCH     0x4000u /* RXD0 LENGTH differs from the DMA length */
#define MT792X_RXF_RSSI_CHAIN0      0x8000u /* antenna mask unknown: rssi_dbm is chain 0 only */

struct mt792x_rx_frame {
    const uint8_t *data;      /* 802.11 frame (or 802.3 if HDR_TRANS); library-owned, valid until the next call */
    uint32_t len;
    uint8_t channel;          /* RXD3 CH_FREQ, converted like mt792x_get_status_freq_info (6 GHz numbering) */
    uint8_t band;             /* MT792X_BAND_* */
    uint8_t fcs_ok;           /* !FCS_ERR */
    uint8_t ring;             /* hardware RX ring */
    int8_t rssi_dbm;          /* mt76_rx_signal over the antenna mask; -128 without group 3 */
    int8_t chain_rssi[4];     /* (RCPI - 220) / 2, floor */
    uint8_t rate_mode;        /* P-RXV TX_MODE (0 CCK, 1 OFDM, 2 HT, ...) */
    uint8_t rate_idx;         /* P-RXV TX_RATE */
    uint8_t rate_nss;         /* NSTS + 1 */
    uint8_t rate_bw;          /* FRAME_MODE */
    uint32_t flags;           /* MT792X_RXF_* */
    uint32_t timestamp;       /* group 2 word 0 (chip clock) */
    uint32_t rxd[6];          /* raw base RXD words */
    uint32_t group4[4];       /* raw group 4 words (0 if absent) */
    uint32_t prxv[2];         /* raw group 3 words (0 if absent) */
    uint32_t raw_len;         /* bytes the chip wrote into the buffer */
    uint32_t hdr_gap;         /* bytes before the frame: RXD + groups + 2 * HDR_OFFSET */
    uint64_t host_time_us;    /* hal->now_us when the descriptor was taken */
};

/* Reasons a packet on a data ring is counted as malformed and dropped. */
enum mt792x_rx_error {
    MT792X_RXE_SHORT = 1,     /* shorter than the 24-byte base RXD */
    MT792X_RXE_BAND_IDX,      /* RXD1 BAND_IDX (mt76: -EINVAL) */
    MT792X_RXE_AMSDU_ERR,     /* RXD2 AMSDU_ERR */
    MT792X_RXE_HDR_TRANS_CM,  /* HDR_TRANS with CM */
    MT792X_RXE_MAX_LEN_ERR,   /* RXD2 MAX_LEN_ERROR */
    MT792X_RXE_GROUP_OVERRUN, /* an announced group reaches the end of the buffer */
    MT792X_RXE_PAD_OVERRUN,   /* RXD + groups + header padding >= length */
    MT792X_RXE_FRAME_SHORT,   /* fewer than 10 frame bytes (an ACK without FCS) */
    MT792X_RXE_AMSDU_HDR,     /* A-MSDU header move impossible within the length */
    MT792X_RXE_HDR_TRANS_FRAG,/* HDR_TRANS on a first fragment: mt76 needs a station entry, drops it */
    MT792X_RXE_COUNT
};

struct mt792x_rx_counters {
    uint64_t frames;              /* returned by mt792x_rx_poll */
    uint32_t fcs_err;             /* frames returned with FCS_ERR */
    uint32_t icv_err;
    uint32_t hdr_trans;           /* frames returned as 802.3 */
    uint32_t fcs_trailer;         /* frames whose last 4 bytes are a valid CRC-32 */
    uint32_t malformed;           /* dropped by the parser */
    uint32_t malformed_by[MT792X_RXE_COUNT];
    uint32_t frag_packets;        /* multi-descriptor packets dropped (LAST_SEC0 clear) */
    uint32_t frag_descs;          /* descriptors belonging to them */
    uint32_t bad_desc_len;        /* descriptors with length 0 or larger than the buffer */
    uint32_t pkt_type[32];        /* every packet taken from any RX ring, by RXD0 PKT_TYPE */
    uint32_t events;              /* MCU events (any ring, solicited or not) */
    uint32_t event_by_id[256];    /* ... by event id */
    uint32_t unsolicited;         /* events that were not a response being waited for */
    uint32_t txs_records;         /* PKT_TYPE_TXS: 8-word records */
    uint32_t tx_free_msdus;       /* PKT_TYPE_TXRX_NOTIFY: MSDU entries */
    uint32_t other_pkts;          /* other packet types on a data ring */
    uint32_t discarded_in_cmd;    /* frames consumed (not returned) while waiting for an MCU response */
    uint32_t rx2_full;            /* polls that found every armed RX ring 2 descriptor completed */
    uint32_t liveness_checks;
};

/* ---- raw TX (spec 12) ---- */
/* Legacy rates; the TXD6 TX_RATE value is MODE << 6 | IDX as in mt76's rate table (mac80211.c:160-173). */
enum mt792x_tx_rate {
    MT792X_RATE_CCK_1M = 1, MT792X_RATE_CCK_2M, MT792X_RATE_CCK_5_5M, MT792X_RATE_CCK_11M,
    MT792X_RATE_OFDM_6M, MT792X_RATE_OFDM_9M, MT792X_RATE_OFDM_12M, MT792X_RATE_OFDM_18M,
    MT792X_RATE_OFDM_24M, MT792X_RATE_OFDM_36M, MT792X_RATE_OFDM_48M, MT792X_RATE_OFDM_54M
};

struct mt792x_tx_req {
    const uint8_t *frame;     /* complete 802.11 MAC header + body, no FCS (the hardware appends it) */
    uint32_t len;             /* 24 .. MT792X_TX_MAX_FRAME */
    uint8_t rate;             /* enum mt792x_tx_rate (CCK only on 2.4 GHz). Used for every frame mt76 sends with
                                 FIX_RATE (management and group-addressed frames); unicast data frames follow
                                 mt76 and leave the rate to the firmware unless opt.tx_data_fixed_rate */
    uint8_t want_ack;         /* 1 = unicast: wait for ACK, retry; 0 = TXD3 NO_ACK (required for group addresses) */
    uint8_t tx_count;         /* TXD3 REM_TX_COUNT, transmissions incl. retries: 1..15, 0 = mt76's 15 */
    uint8_t want_status;      /* 1 = ask for a TX status report (TXS: ACKed or not, final rate); needs want_ack */
};

enum mt792x_tx_result {
    MT792X_TXR_DONE = 1,      /* the chip returned the token (TX-free notification) */
    MT792X_TXR_LOST = 2,      /* no TX-free within the timeout: buffer quarantined, fate unknown */
    MT792X_TXR_ABORTED = 3    /* still in flight at shutdown (DMA stopped, chip reset) */
};

enum mt792x_tx_ack {
    MT792X_ACK_UNKNOWN = 0,       /* ACK requested but no TX status report */
    MT792X_ACK_YES = 1,           /* TXS0 ACK_ERROR bits (18:16) clear */
    MT792X_ACK_NO = 2,            /* TXS0 ACK_TIMEOUT / RTS_TIMEOUT / QUEUE_TIMEOUT */
    MT792X_ACK_NOT_REQUESTED = 3  /* NO_ACK frame */
};

struct mt792x_tx_done {
    uint32_t cookie;          /* as returned by mt792x_tx_frame */
    uint8_t result;           /* enum mt792x_tx_result */
    uint8_t acked;            /* enum mt792x_tx_ack */
    uint8_t free_status;      /* TX-free STATUS (0 = success, else dropped), 0xff = no TX-free seen */
    uint8_t tx_count;         /* TX-free COUNT = transmissions (retries + 1), 0 = not reported */
    uint8_t has_txs;          /* a TX status record was matched */
    uint16_t txs_rate;        /* TXS0 TX_RATE: NSS 12:10, MODE 9:6 (0 CCK, 1 OFDM), IDX 5:0 */
    uint32_t txs[4];          /* raw TXS words 0-3 (0 without a record) */
    uint32_t elapsed_us;      /* from mt792x_tx_frame to the event that completed the record */
};

struct mt792x_tx_counters {
    uint32_t submitted, done, lost, aborted;
    uint32_t refused;         /* mt792x_tx_frame calls that returned an error */
    uint32_t acked, not_acked;
    uint32_t txfree_entries;  /* MSDU entries in TX-free notifications */
    uint32_t txfree_unknown;  /* ... whose token is not in flight (logged) */
    uint32_t txfree_late;     /* ... that released a quarantined slot */
    uint32_t txs_records;     /* TXS records seen (PKT_TYPE_TXS and MCU TX_DONE events) */
    uint32_t txs_unmatched;   /* ... not matching a frame that asked for one */
    uint32_t quarantined;     /* slots currently quarantined */
    uint32_t desc_reclaimed;  /* TX ring 0 descriptors the chip consumed */
};

enum {
    MT792X_RING_TX0 = 0,
    MT792X_RING_TX16 = 1,
    MT792X_RING_TX17 = 2,
    MT792X_RING_RX0 = 3,
    MT792X_RING_RX4 = 4,
    MT792X_RING_RX2 = 5,
    MT792X_RING_COUNT = 6
};

#define MT792X_TX_DONE_QUEUE 64u

struct mt792x_tx_slot {
    uint8_t state;            /* 0 free, 1 in flight, 2 quarantined */
    uint8_t want_ack, want_status, pid;
    uint8_t freed;            /* TX-free seen */
    uint8_t free_status, tx_count, has_txs;
    uint16_t token, gen;
    uint32_t cookie;
    uint32_t txs[4];
    uint64_t t_submit, t_event;
};

/* Device state. Public so the caller can allocate it; treat the members as private. */
struct mt792x {
    struct mt_hal *hal;
    struct mt792x_options opt;
    int state;
    struct mt792x_ring ring[MT792X_RING_COUNT];

    uint32_t remap_saved;     /* MT_HIF_REMAP_L1 as found at S0 */
    int remap_saved_valid;
    int bus_dead;             /* sticky: no further register writes during bring-up */
    int in_shutdown;          /* mt792x_shutdown is running: writes allowed best-effort */

    uint32_t chip_id;
    uint32_t hw_bound;
    uint32_t hw_rev;          /* (chip_id << 16) | (HW_REV & 0xff), as mt76 computes it */
    int last_step;            /* highest step S<n> completed, -1 = none */
    int failed_step;          /* step that failed, -1 = none */
    int error;                /* result of the last mt792x_bringup */
    int shutdown_error;       /* 0, or the first problem seen by mt792x_shutdown */

    uint32_t msg_seq;         /* mt76 dev->mcu.msg_seq */
    int sem_held;             /* patch semaphore obtained (result 2) and not yet released */
    int patch_was_loaded;     /* semaphore answered PATCH_IS_DL */
    int fw_valid;
    struct mt792x_patch_info patch;
    struct mt792x_ram_info ram;

    uint8_t evt[MT792X_RX_BUF_SIZE]; /* last MCU event copied out of the RX ring */
    uint32_t evt_len;
    uint32_t evt_ring;        /* hardware RX ring the last response came from */

    /* post-boot / sniffer / RX (spec 11) */
    struct mt792x_nic_caps nic;
    int postboot_done;        /* S31 reached */
    int mac_enabled;          /* MAC_INIT_CTRL enable sent (may be in effect) */
    int radio_on;             /* ID_RADIO_ON_OFF_CTRL 1/2 sent */
    int dev_active;           /* DEV_INFO active=1 sent (may be in effect) */
    int bss_active;           /* BSS_INFO active=1 sent (may be in effect) */
    int sniffer_on;           /* sniffer enable sent (may be in effect) */
    int sniff_started;        /* mt792x_sniffer_start completed */
    int rx_active;            /* sniffer started: data ring 2 is drained during command waits */
    uint8_t tune_used;        /* resolved enum mt792x_tune_mode */
    uint8_t own_mac[6];       /* address given to DEV_INFO */
    uint8_t cur_channel, cur_band;
    uint32_t sniff_flags;
    int sniff_failed_step;    /* SN<n> that failed, -1 = none */
    int last_status;          /* status of the last UNI status reply */
    uint64_t next_liveness_us;
    struct mt792x_rx_counters rxc;
    uint8_t rx_raw[MT792X_RX_BUF_SIZE]; /* last data-ring packet; mt792x_rx_frame.data points into it */

    /* raw TX (spec 12) */
    int tx_allowed;           /* cfg.allow_tx of the running sniffer */
    int pair_active;          /* MT792X_VIF_LINUX_PAIR: DEV/BSS 1 sent (may be in effect) */
    uint8_t tx_omac, tx_wcid; /* OMAC / WLAN index used in TXD1 */
    uint8_t next_pid;
    uint32_t next_cookie;
    struct mt792x_tx_slot slot[MT792X_TX0_SLOTS];
    struct mt792x_tx_done done_q[MT792X_TX_DONE_QUEUE];
    uint32_t done_head, done_count;
    struct mt792x_tx_counters txc;
};

/* Firmware images in memory. Untrusted: they are validated completely before any register access. */
struct mt792x_fw {
    const uint8_t *patch;
    size_t patch_len;
    const uint8_t *ram;
    size_t ram_len;
};

/* S0: check the HAL, lay out and zero the DMA region. No register access. */
int mt792x_init(struct mt792x *dev, struct mt_hal *hal);

/* Run steps S0.. up to the end of 'until'. Once per mt792x_init. fw may be NULL below
 * MT792X_STAGE_FIRMWARE; if given it is validated first in every case. */
int mt792x_bringup(struct mt792x *dev, const struct mt792x_fw *fw, enum mt792x_stage until);

/* Spec 7.3: (best effort, only if started and the bus is alive) sniffer off, BSS/DEV deactivated,
 * radio LED off, MAC_INIT_CTRL disable; then drv-own, stop DMA, WFSYS reset (with real polls and
 * the 50 ms delay), restore the L1 remap register. Safe at any point, idempotent. */
void mt792x_shutdown(struct mt792x *dev);

/* ---- sniffer and RX (spec 11); require MT792X_STAGE_POSTBOOT ---- */

/* Configures the monitor interface and tunes the radio (sequence SN1-SN12 of spec 11.5). */
int mt792x_sniffer_start(struct mt792x *dev, const struct mt792x_sniffer_cfg *cfg);
/* Retunes (command per dev->opt.tune_mode / firmware CNM bit). *elapsed_us (may be NULL) = time
 * from the first command sent until the last awaited response arrived. */
int mt792x_sniffer_set_channel(struct mt792x *dev, uint8_t channel, uint8_t band, uint32_t *elapsed_us);
/* Sniffer disable, BSS + DEV deactivation (mt76 order). The MAC stays enabled until shutdown. */
int mt792x_sniffer_stop(struct mt792x *dev);

/* Returns 1 and one frame in *out, 0 if none is pending, < 0 on error. Also drains RX rings 0
 * and 4 (events are counted, never block) and the non-frame packets of ring 2. */
int mt792x_rx_poll(struct mt792x *dev, struct mt792x_rx_frame *out);
const struct mt792x_rx_counters *mt792x_rx_counters(const struct mt792x *dev);
/* MAC address given to the firmware for our vif (valid after mt792x_sniffer_start). */
const uint8_t *mt792x_own_mac(const struct mt792x *dev);

/* ---- raw TX (spec 12); requires a sniffer started with cfg.allow_tx ---- */

/* Validates the request completely, copies the frame into a free TX slot and hands it to TX ring 0.
 * Never waits for the chip (at most one dma_idx read to reclaim descriptors). Returns 0 and a cookie
 * that reappears in exactly one struct mt792x_tx_done, or an error (no cookie, nothing sent). The
 * frame's address 2 must be our own MAC; group-addressed frames need want_ack = 0; the sequence
 * number in the header is transmitted as given (TXD3 SN_VALID, like an injected frame in mt76). */
int mt792x_tx_frame(struct mt792x *dev, const struct mt792x_tx_req *req, uint32_t *cookie);
/* Completion records. TX-free notifications and TX status reports are taken from the RX rings by
 * mt792x_rx_poll (and while commands wait), so call that regularly. This function reclaims consumed
 * ring descriptors, applies the timeouts and returns 1 with one record, 0 if none is ready, < 0 on
 * error. Also usable after mt792x_shutdown (records of aborted frames). */
int mt792x_tx_done_get(struct mt792x *dev, struct mt792x_tx_done *out);
const struct mt792x_tx_counters *mt792x_tx_counters(const struct mt792x *dev);
/* TXD6 TX_RATE value (MODE << 6 | IDX) of a rate, 0xffff if invalid. Exposed for tests and logs. */
uint16_t mt792x_tx_rate_value(uint8_t rate);
const struct mt792x_nic_caps *mt792x_nic_caps(const struct mt792x *dev);
/* Resolved tune mode (MT792X_TUNE_CHANNEL_SWITCH / _SNIFFER_CONFIG / _BOTH) after sniffer start. */
int mt792x_tune_mode_used(const struct mt792x *dev);

/* Pure parser behind mt792x_rx_poll, exposed for tests: parses one PKT_TYPE_NORMAL packet of
 * exactly 'len' bytes in place (an A-MSDU header move may modify 'buf'). Never reads outside
 * [buf, buf + len). Returns 0 and fills *out (out->data points into buf), or -enum mt792x_rx_error. */
int mt792x_rx_parse(uint8_t *buf, uint32_t len, uint8_t fcs_mode, uint8_t antenna_mask, struct mt792x_rx_frame *out);
/* Pure GET_NIC_CAPAB payload parser (bytes after the 36-byte event header), exposed for tests. */
int mt792x_nic_caps_parse(const uint8_t *p, uint32_t len, struct mt792x_nic_caps *out);
/* IEEE 802.3 CRC-32 (the 802.11 FCS) of a buffer. */
uint32_t mt792x_crc32(const uint8_t *p, uint32_t len);

/* Spec 7.2: register operations a proxy replays to stop all DMA when the client dies. */
#define MT792X_STOP_WRITE 1u  /* register = value */
#define MT792X_STOP_SET   2u  /* register |= value */
#define MT792X_STOP_CLEAR 3u  /* register &= ~value */
#define MT792X_STOP_OPS_COUNT 10u
struct mt792x_stop_op {
    uint8_t kind;
    uint32_t bar_offset;
    uint32_t value;
};
/* Copies min(max, MT792X_STOP_OPS_COUNT) operations; returns MT792X_STOP_OPS_COUNT. */
size_t mt792x_stop_ops(struct mt792x_stop_op *ops, size_t max);

const char *mt792x_strerror(int err);

/* Read-only status. */
int mt792x_last_step(const struct mt792x *dev);
int mt792x_failed_step(const struct mt792x *dev);
uint32_t mt792x_chip_id(const struct mt792x *dev);
uint32_t mt792x_hw_rev(const struct mt792x *dev);
int mt792x_patch_was_loaded(const struct mt792x *dev);
const struct mt792x_patch_info *mt792x_patch_info(const struct mt792x *dev); /* NULL if no fw parsed */
const struct mt792x_ram_info *mt792x_ram_info(const struct mt792x *dev);     /* NULL if no fw parsed */

/* The one function that turns a region offset into a bus address for the chip.
 * Fails (MT792X_E_DMA_ADDR, logged, nothing written) if [offset, offset+len) is not inside the
 * region, len is 0, the address is not 'align'-aligned (align = power of two), or the region
 * would end above 4 GiB. Exposed for tests. */
int mt792x_dma_bus(const struct mt792x *dev, uint32_t offset, uint32_t len, uint32_t align, uint32_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* MT792X_MT792X_H */
