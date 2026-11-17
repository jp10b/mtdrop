/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Raw 802.11 TX (spec section 12) against the simulated chip: TXD/TXP bytes as mt76 writes them
 * (expected words computed by hand in the comments), completion through TX-free notifications and
 * TX status reports, timeouts and quarantine, ring wrap, refusals, shutdown and bus death. */
#include "test.h"
#include "rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t chip_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 }; /* the fake's GET_NIC_CAPAB MAC */
static const uint8_t peer[6] = { 0x0a, 0x99, 0x88, 0x77, 0x66, 0x55 };
static const uint8_t awdl_bssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* 802.11 header: fc, duration 0, a1, a2, a3, sequence control = sn << 4 */
static uint32_t hdr(uint8_t *f, uint16_t fc, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3, uint16_t sn)
{
    memset(f, 0, 24);
    f[0] = (uint8_t)fc;
    f[1] = (uint8_t)(fc >> 8);
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    f[22] = (uint8_t)(sn << 4);
    f[23] = (uint8_t)(sn >> 4);
    return 24;
}

/* AWDL action frame to broadcast (docs/PROTOCOL.md 3.4): 24 + 40 bytes */
static uint32_t awdl_action(uint8_t *f, uint16_t sn)
{
    uint32_t n = hdr(f, 0x00d0, bcast, chip_mac, awdl_bssid, sn), i;
    static const uint8_t fixed[8] = { 127, 0x00, 0x17, 0xf2, 8, 0x10, 3, 0 };
    memcpy(f + n, fixed, sizeof(fixed));
    for (i = 8; i < 40; i++) f[n + i] = (uint8_t)(i * 7u);
    return n + 40;
}

/* AWDL unicast data frame (fc 0x0008, no QoS): 24 + 76 bytes */
static uint32_t awdl_data(uint8_t *f, uint16_t sn, uint32_t body)
{
    uint32_t n = hdr(f, 0x0008, peer, chip_mac, awdl_bssid, sn), i;
    for (i = 0; i < body; i++) f[n + i] = (uint8_t)(0xa0u ^ i);
    return n + body;
}

static int tx_rig(struct rig *r, uint8_t channel, int paged, int allow_tx)
{
    struct mt792x_sniffer_cfg cfg;
    int err = paged ? rig_init_paged(r) : rig_init(r, 0);
    if (err) return err;
    err = rig_postboot(r);
    if (err) return err;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = channel;
    cfg.band = channel > 14 ? MT792X_BAND_5GHZ : MT792X_BAND_2GHZ;
    cfg.flags = MT792X_SNIFF_CONTROL | MT792X_SNIFF_OTHER_BSS;
    cfg.allow_tx = (uint8_t)allow_tx;
    return mt792x_sniffer_start(r->dev, &cfg);
}

/* Like tx_rig but lets the caller set options between init and bring-up. */
static int tx_rig_start(struct rig *r, uint8_t channel)
{
    struct mt792x_sniffer_cfg cfg;
    int err = rig_postboot(r);
    if (err) return err;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = channel;
    cfg.band = channel > 14 ? MT792X_BAND_5GHZ : MT792X_BAND_2GHZ;
    cfg.allow_tx = 1;
    return mt792x_sniffer_start(r->dev, &cfg);
}

/* Drains the RX rings (frames are discarded), then returns the next completion record (1) or 0. */
static int next_done(struct rig *r, struct mt792x_tx_done *d)
{
    struct mt792x_rx_frame fr;
    int rc, guard = 0;
    while ((rc = mt792x_rx_poll(r->dev, &fr)) == 1 && guard++ < 10000) {
    }
    if (rc < 0) return rc;
    return mt792x_tx_done_get(r->dev, d);
}

static int tx_send(struct rig *r, const uint8_t *f, uint32_t len, uint8_t rate, int ack, int status, uint32_t *cookie)
{
    struct mt792x_tx_req q;
    memset(&q, 0, sizeof(q));
    q.frame = f;
    q.len = len;
    q.rate = rate;
    q.want_ack = (uint8_t)ack;
    q.want_status = (uint8_t)status;
    return mt792x_tx_frame(r->dev, &q, cookie);
}

static uint32_t le32_at(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

TEST(tx_broadcast_awdl_action_12m_txd_as_mt76)
{
    struct rig r;
    uint8_t f[128];
    uint32_t len, cookie = 0, slot_bus = 0;
    struct mt792x_tx_done d;
    const struct fake_air *a;
    CHECK(tx_rig(&r, 6, 0, 1) == 0);
    len = awdl_action(f, 0x123);
    CHECK_EQ_U32(len, 64);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == 0);
    CHECK(cookie != 0);
    CHECK_EQ_U32(r.f->n_air, 1);
    a = &r.f->air[0];
    CHECK_EQ_U32(a->len, len);
    CHECK_MEM_EQ(a->bytes, f, len);                         /* radiated bytes = request */
    /* Hand-computed from mt76_connac2_mac_write_txwi for an injected broadcast action frame on the
     * monitor vif (omac 0, wcid 19, wmm 0), antenna mask 3:
     *  action -> bufferable MMPDU -> not the PSD queue: AC VO -> Q_IDX 0 * 4 + (3 - 0) = 3
     *  TXD0 = 96 | PKT_FMT 0 | 3 << 25                                       = 0x06000060
     *  TXD1 = LONG | HDR_FORMAT 2 << 16 | HDR_INFO 12 << 11 | TID 7 << 20 | 19 = 0x80726013
     *  TXD2 = subtype 13 | MULTICAST | HTC_VLD | FIX_RATE                    = 0x8000240d
     *  TXD3 = SN_VALID | 0x123 << 16 | BA_DISABLE | 15 << 11 | NO_ACK        = 0x91237801
     *  TXD5 = 0 (NO_ACK -> MT_PACKET_ID_NO_ACK)
     *  TXD6 = OFDM 12 Mb/s ((1 << 6) | 10 = 0x4a) << 16 | FIXED_BW            = 0x004a0004
     *  TXD7 = TYPE 0, SUB_TYPE 13 << 16, SPE_IDX 24 << 11                    = 0x000dc000 */
    CHECK_EQ_U32(a->txd[0], 0x06000060u);
    CHECK_EQ_U32(a->txd[1], 0x80726013u);
    CHECK_EQ_U32(a->txd[2], 0x8000240du);
    CHECK_EQ_U32(a->txd[3], 0x91237801u);
    CHECK_EQ_U32(a->txd[4], 0);
    CHECK_EQ_U32(a->txd[5], 0);
    CHECK_EQ_U32(a->txd[6], 0x004a0004u);
    CHECK_EQ_U32(a->txd[7], 0x000dc000u);
    /* TXP: msdu_id[0] = token 0 | VALID, buf0 = slot 0 + 64, len0 = 64 | LAST */
    CHECK_EQ_U32(a->txp[0] | (a->txp[1] << 8), 0x8000u);
    CHECK(mt792x_dma_bus(r.dev, MT792X_OFF_TX0_SLOT + 64u, len, 4u, &slot_bus) == 0);
    CHECK_EQ_U32(le32_at(a->txp + 8), slot_bus);
    CHECK_EQ_U32(a->txp[12] | (a->txp[13] << 8), 0x8040u);
    CHECK_EQ_U32(mt792x_tx_rate_value(MT792X_RATE_OFDM_12M), 0x4a);

    CHECK(next_done(&r, &d) == 1);
    CHECK_EQ_U32(d.cookie, cookie);
    CHECK_EQ_U32(d.result, MT792X_TXR_DONE);
    CHECK_EQ_U32(d.acked, MT792X_ACK_NOT_REQUESTED);
    CHECK_EQ_U32(d.free_status, 0);
    CHECK_EQ_U32(d.tx_count, 1);
    CHECK(!d.has_txs);
    CHECK(next_done(&r, &d) == 0);

    /* TX power limits as mt76 programs them for PL: 2 + 6 messages, one REG_READ after each */
    CHECK_EQ_U32(r.f->pwr_msgs[0], 2);
    CHECK_EQ_U32(r.f->pwr_msgs[1], 6);
    CHECK_EQ_U32(r.f->pwr_channels[0], 14);
    CHECK_EQ_U32(r.f->pwr_channels[1], 48);
    CHECK_EQ_U32(r.f->reg_reads, 8);
    CHECK_EQ_U32(r.f->pwr_value[0][6], 40);
    CHECK_EQ_U32(r.f->pwr_value[1][44], 46);
    CHECK_EQ_U32(r.f->pwr_value[1][149], 28);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);
}

static void unicast_case(unsigned variant)
{
    struct rig r;
    uint8_t f[256];
    uint32_t len, cookie = 0;
    struct mt792x_tx_done d;
    const struct fake_air *a;
    CHECK(tx_rig(&r, 44, 0, 1) == 0);
    r.f->txs_via_event = variant == 1;
    r.f->txs_first = variant == 2;
    if (variant == 3) r.f->txc_ring = 2;                      /* completions on the data ring */
    len = awdl_data(f, 0x45, 76);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 1, 1, &cookie) == 0);
    a = &r.f->air[0];
    CHECK_MEM_EQ(a->bytes, f, len);
    /* unicast data, no QoS: mac80211 priority 0 -> AC BE -> Q_IDX 3 - 2 = 1, TID 0; no FIX_RATE
     * (mt76 sets it only for non-data or group frames), PID 3 with TX_STATUS_HOST:
     *  TXD0 = 132 | 1 << 25 = 0x02000084; TXD1 = 0x80026013; TXD2 = type 2 << 4 = 0x20;
     *  TXD3 = SN_VALID | 0x45 << 16 | 15 << 11 = 0x80457800; TXD5 = 0x403; TXD6 = 0; TXD7 = 0x00200000 */
    CHECK_EQ_U32(a->txd[0], 0x02000084u);
    CHECK_EQ_U32(a->txd[1], 0x80026013u);
    CHECK_EQ_U32(a->txd[2], 0x00000020u);
    CHECK_EQ_U32(a->txd[3], 0x80457800u);
    CHECK_EQ_U32(a->txd[5], 0x00000403u);
    CHECK_EQ_U32(a->txd[6], 0);
    CHECK_EQ_U32(a->txd[7], 0x00200000u);
    CHECK(next_done(&r, &d) == 1);
    CHECK_EQ_U32(d.cookie, cookie);
    CHECK_EQ_U32(d.result, MT792X_TXR_DONE);
    CHECK_EQ_U32(d.acked, MT792X_ACK_YES);
    CHECK(d.has_txs);
    CHECK_EQ_U32(d.tx_count, 1);
    CHECK_EQ_U32(r.dev->txc.txs_unmatched, 0);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);
}

TEST(tx_unicast_data_with_ack_and_status)
{
    unicast_case(0);
}

TEST(tx_status_via_mcu_event_and_before_txfree_and_on_ring2)
{
    unicast_case(1);
    unicast_case(2);
    unicast_case(3);
}

TEST(tx_not_acked_reports_retries)
{
    struct rig r;
    uint8_t f[256];
    uint32_t len, cookie = 0;
    struct mt792x_tx_done d;
    CHECK(tx_rig(&r, 1, 0, 1) == 0);
    r.f->tx_mode = FAKE_TX_NOACK;
    len = awdl_data(f, 7, 40);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 1, 1, &cookie) == 0);
    CHECK(next_done(&r, &d) == 1);
    CHECK_EQ_U32(d.acked, MT792X_ACK_NO);
    CHECK_EQ_U32(d.tx_count, 15);
    CHECK_EQ_U32(r.dev->txc.not_acked, 1);
    /* without a TXS the ACK state is unknown; the record waits MT_TXS_WAIT_US for it */
    r.f->tx_mode = FAKE_TX_ACK;
    r.f->txs_never = 1;
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 1, 1, &cookie) == 0);
    CHECK(next_done(&r, &d) == 0);
    mock_advance(&r.m, 600000);
    CHECK(next_done(&r, &d) == 1);
    CHECK_EQ_U32(d.acked, MT792X_ACK_UNKNOWN);
    CHECK_EQ_U32(d.result, MT792X_TXR_DONE);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

/* radiod acktest: a directed probe request with ACK + TX status and a retry limit, in both vif setups;
 * the access point ACKs (TXS ACKed, one transmission), the ghost does not (TXS ACK_TIMEOUT, all tries). */
static void directed_probe_case(int pair, uint8_t tries)
{
    struct rig r;
    uint8_t f[64];
    uint32_t len, cookie = 0, i;
    struct mt792x_tx_done d;
    CHECK(rig_init(&r, 0) == 0);
    if (pair) r.dev->opt.vif_mode = MT792X_VIF_LINUX_PAIR;
    CHECK(tx_rig_start(&r, 1) == 0);
    for (i = 0; i < 2u; i++) {
        struct mt792x_tx_req q;
        const struct fake_air *a;
        r.f->tx_mode = i ? FAKE_TX_NOACK : FAKE_TX_ACK;
        len = hdr(f, 0x0040, peer, chip_mac, peer, (uint16_t)(0x200 + i));
        f[len++] = 0; /* SSID, wildcard */
        f[len++] = 0;
        memset(&q, 0, sizeof(q));
        q.frame = f;
        q.len = len;
        q.rate = MT792X_RATE_OFDM_6M;
        q.want_ack = 1;
        q.want_status = 1;
        q.tx_count = tries;
        CHECK(mt792x_tx_frame(r.dev, &q, &cookie) == 0);
        a = &r.f->air[i];
        CHECK_EQ_U32(a->txd[0] >> 25, 0x10);                       /* not bufferable -> ALTX0 */
        CHECK_EQ_U32(a->txd[1] & 0x3ffu, pair ? 18u : 19u);        /* the vif's wcid */
        CHECK(a->txd[2] & 0x80000000u);                            /* FIX_RATE (management) */
        CHECK_EQ_U32(a->txd[3] & 1u, 0);                           /* NO_ACK clear */
        CHECK_EQ_U32((a->txd[3] >> 11) & 0xfu, tries ? tries : 15u); /* REM_TX_COUNT */
        CHECK(a->txd[5] & 0x400u);                                 /* TX_STATUS_HOST */
        CHECK((a->txd[5] & 0xffu) >= 3u);                          /* PID from 3 */
        CHECK_EQ_U32(a->txd[6], 0x004b0004u);                      /* OFDM 6 = (1 << 6) | 11, FIXED_BW */
        CHECK(next_done(&r, &d) == 1);
        CHECK_EQ_U32(d.cookie, cookie);
        CHECK_EQ_U32(d.result, MT792X_TXR_DONE);
        CHECK(d.has_txs);
        CHECK_EQ_U32(d.acked, i ? MT792X_ACK_NO : MT792X_ACK_YES);
        CHECK_EQ_U32(d.tx_count, i ? (tries ? tries : 15u) : 1u);
    }
    CHECK_EQ_U32(r.dev->txc.acked, 1);
    CHECK_EQ_U32(r.dev->txc.not_acked, 1);
    CHECK_EQ_U32(r.dev->txc.txs_unmatched, 0);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);
}

TEST(tx_directed_probe_ack_status_single_and_pair)
{
    directed_probe_case(0, 15);
    directed_probe_case(1, 15);
    directed_probe_case(0, 4);
    directed_probe_case(1, 0);
}

TEST(tx_probe_request_options_and_pair_mode)
{
    struct rig r;
    uint8_t f[128];
    uint32_t len, cookie = 0;
    /* probe request: not bufferable -> MT_TXQ_PSD -> Q_IDX ALTX0 (0x10), CCK 1 Mb/s, mt76 v7.0 SPE */
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.tx_spe_mode = MT792X_SPE_LINUX_7_0;
    r.dev->opt.tx_wcid_mode = MT792X_WCID_GLOBAL;
    r.dev->opt.tx_power_max_half_db = 20;
    CHECK(tx_rig_start(&r, 11) == 0);
    len = hdr(f, 0x0040, bcast, chip_mac, bcast, 9);
    f[len++] = 0;                                              /* SSID, wildcard */
    f[len++] = 0;
    CHECK(tx_send(&r, f, len, MT792X_RATE_CCK_1M, 0, 0, &cookie) == 0);
    CHECK_EQ_U32(r.f->air[0].txd[0] >> 25, 0x10);
    CHECK_EQ_U32(r.f->air[0].txd[1] & 0x3ffu, 0);              /* global wcid */
    CHECK_EQ_U32(r.f->air[0].txd[6], 0x00000004u);             /* CCK 1 Mb/s = mode 0 idx 0 */
    CHECK_EQ_U32(r.f->air[0].txd[7], 0x00040000u);             /* no SPE_IDX */
    CHECK_EQ_U32(r.f->pwr_value[0][11], 20);                   /* extra cap applied */
    CHECK_EQ_U32(r.f->pwr_value[1][36], 20);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);

    /* Linux pair: OMAC 0 = 00:..:00, OMAC 1 = our MAC; frames on OMAC 1 / wcid 18 / WMM set 1 */
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.vif_mode = MT792X_VIF_LINUX_PAIR;
    r.dev->opt.tx_data_fixed_rate = 1;
    CHECK(tx_rig_start(&r, 44) == 0);
    {
        static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
        CHECK(memcmp(r.f->dev_mac, zero, 6) == 0);
        CHECK(memcmp(r.f->dev1_mac, chip_mac, 6) == 0);
        CHECK(r.f->dev_active && r.f->bss_active && r.f->dev1_active && r.f->bss1_active && r.f->sniffer_on);
    }
    len = awdl_action(f, 1);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == 0);
    CHECK_EQ_U32(r.f->air[0].txd[0] >> 25, 7);                 /* 1 * 4 + 3 (VO) */
    CHECK_EQ_U32(r.f->air[0].txd[1] & 0x3f0003ffu, 0x01000012u); /* OWN_MAC 1, wcid 18 */
    len = awdl_data(f, 2, 20);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_24M, 1, 0, &cookie) == 0);
    CHECK(r.f->air[1].txd[2] & 0x80000000u);                   /* tx_data_fixed_rate */
    CHECK_EQ_U32(r.f->air[1].txd[6], 0x00490004u);             /* OFDM 24 = (1 << 6) | 9 */
    CHECK_EQ_U32(r.f->air[1].txd[0] >> 25, 5);                 /* 1 * 4 + (3 - BE) */
    mt792x_shutdown(r.dev);
    CHECK(!r.f->dev1_active && !r.f->bss1_active && !r.f->dev_active && !r.f->mac_enabled);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);
}

TEST(tx_timeout_quarantine_and_late_free)
{
    struct rig r;
    uint8_t f[128];
    uint32_t len, cookie = 0, c2 = 0, i;
    uint16_t token;
    struct mt792x_tx_done d;
    CHECK(tx_rig(&r, 6, 0, 1) == 0);
    r.f->tx_mode = FAKE_TX_SILENT;
    len = awdl_action(f, 3);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == 0);
    token = r.f->air[0].token;
    CHECK(next_done(&r, &d) == 0);
    mock_advance(&r.m, 2100000);
    CHECK(next_done(&r, &d) == 1);
    CHECK_EQ_U32(d.cookie, cookie);
    CHECK_EQ_U32(d.result, MT792X_TXR_LOST);
    CHECK_EQ_U32(r.dev->txc.quarantined, 1);
    /* the next 15 frames get the other slots; a 17th has none */
    for (i = 0; i < 15; i++) CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &c2) == 0);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &c2) == MT792X_E_TX_BUSY);
    for (i = 1; i < 16; i++) CHECK(r.f->air[i].token != token);
    /* the quarantined token comes back late: the slot becomes usable again */
    CHECK(fake_tx_free_token(r.f, token, 19, 0, 1));
    CHECK(next_done(&r, &d) == 0);
    CHECK_EQ_U32(r.dev->txc.quarantined, 0);
    CHECK_EQ_U32(r.dev->txc.txfree_late, 1);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &c2) == 0);
    CHECK(r.f->air[16].token != token);                        /* new generation */
    CHECK_EQ_U32(r.f->air[16].token % MT792X_TX0_SLOTS, token % MT792X_TX0_SLOTS);
    /* an unknown token is counted, not acted on */
    CHECK(fake_tx_free_token(r.f, 4000, 19, 0, 1));
    CHECK(next_done(&r, &d) == 0);
    CHECK_EQ_U32(r.dev->txc.txfree_unknown, 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(tx_ring_wrap_hundreds_of_frames)
{
    struct rig r;
    uint8_t f[512];
    uint32_t i, cookie = 0, got = 0, expect_cookie = 0;
    struct mt792x_tx_done d;
    int ok = 1;
    CHECK(tx_rig(&r, 36, 0, 1) == 0);
    for (i = 0; i < 700 && ok; i++) {
        uint32_t len = (i & 1) ? awdl_data(f, (uint16_t)i, 20u + (i % 400u)) : awdl_action(f, (uint16_t)i);
        int rc = tx_send(&r, f, len, MT792X_RATE_OFDM_12M, (int)(i & 1), (int)(i & 1), &cookie);
        if (rc) {
            ok = 0;
            break;
        }
        if (!expect_cookie) expect_cookie = cookie;
        if (i < FAKE_MAX_AIR && (r.f->air[i].len != len || memcmp(r.f->air[i].bytes, f, len) != 0)) ok = 0;
        while (next_done(&r, &d) == 1) {
            if (d.cookie != expect_cookie++ || d.result != MT792X_TXR_DONE) ok = 0;
            if ((d.cookie & 1u) == 0 && d.acked != MT792X_ACK_YES) ok = 0;   /* odd frames (even cookies) unicast */
            got++;
        }
    }
    CHECK(ok);
    CHECK_EQ_U32(got, 700);
    CHECK_EQ_U32(r.dev->txc.submitted, 700);
    CHECK(r.dev->txc.desc_reclaimed >= 690u);                  /* TX ring 0 (256 descriptors) wrapped */
    CHECK(r.f->tx_kicks[0] >= 700u);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);
}

TEST(tx_refusals)
{
    struct rig r;
    uint8_t f[4200];
    uint32_t len, cookie = 77;
    struct mt792x_sniffer_cfg cfg;
    struct mt792x_tx_req q;
    /* before the sniffer: state error */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    len = awdl_action(f, 1);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == MT792X_E_STATE);
    CHECK_EQ_U32(cookie, 0);
    /* channels outside the allow-list refuse a TX-enabled sniffer, without any command */
    memset(&cfg, 0, sizeof(cfg));
    cfg.allow_tx = 1;
    cfg.channel = 14;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_TX_DENIED);
    cfg.channel = 52;
    cfg.band = MT792X_BAND_5GHZ;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_TX_DENIED);
    cfg.channel = 149;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_TX_DENIED);
    rig_free(&r);

    /* sniffer without allow_tx: no power commands, TX denied */
    CHECK(tx_rig(&r, 6, 0, 0) == 0);
    CHECK_EQ_U32(r.f->pwr_msgs[0] + r.f->pwr_msgs[1] + r.f->reg_reads, 0);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == MT792X_E_TX_DENIED);
    rig_free(&r);

    CHECK(tx_rig(&r, 40, 0, 1) == 0);
    r.m.log_errors = 0;
    /* switching to a channel outside the list is refused while TX is enabled */
    CHECK(mt792x_sniffer_set_channel(r.dev, 149, MT792X_BAND_5GHZ, NULL) == MT792X_E_TX_DENIED);
    CHECK(mt792x_sniffer_set_channel(r.dev, 13, MT792X_BAND_2GHZ, NULL) == 0);
    CHECK(tx_send(&r, f, len, MT792X_RATE_CCK_1M, 0, 0, &cookie) == 0);           /* CCK fine on 2.4 GHz */
    CHECK(mt792x_sniffer_set_channel(r.dev, 48, MT792X_BAND_5GHZ, NULL) == 0);
    CHECK(tx_send(&r, f, len, MT792X_RATE_CCK_1M, 0, 0, &cookie) == MT792X_E_TX_FRAME);  /* CCK on 5 GHz */
    memset(f, 0, sizeof(f));
    len = awdl_data(f, 1, MT792X_TX_MAX_FRAME - 24u);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == 0);           /* exactly the maximum */
    len = awdl_data(f, 1, MT792X_TX_MAX_FRAME - 23u);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == MT792X_E_TX_FRAME);
    CHECK(tx_send(&r, f, 23, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == MT792X_E_TX_FRAME);
    len = awdl_data(f, 1, 30);
    f[10] ^= 0x02;                                                              /* address 2 not ours */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == MT792X_E_TX_FRAME);
    f[10] ^= 0x02;
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 1, &cookie) == MT792X_E_TX_FRAME); /* status without ACK */
    CHECK(tx_send(&r, f, len, 0, 1, 0, &cookie) == MT792X_E_TX_FRAME);             /* no rate */
    f[1] = 0x40;                                                                /* Protected */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == MT792X_E_TX_FRAME);
    f[1] = 0x04;                                                                /* More Fragments */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == MT792X_E_TX_FRAME);
    f[1] = 0;
    f[0] = 0xd4;                                                                /* control (ACK) */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    f[0] = 0x80;                                                                /* beacon */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = awdl_action(f, 2);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 1, 0, &cookie) == MT792X_E_TX_FRAME); /* group + ACK */
    memset(&q, 0, sizeof(q));
    q.frame = f;
    q.len = len;
    q.rate = MT792X_RATE_OFDM_6M;
    q.tx_count = 16;
    CHECK(mt792x_tx_frame(r.dev, &q, &cookie) == MT792X_E_TX_FRAME);
    CHECK(mt792x_tx_frame(r.dev, NULL, &cookie) == MT792X_E_INVAL);
    CHECK_EQ_U32(r.f->n_air, 2);                                                /* only the two valid frames */
    CHECK(r.dev->txc.refused >= 12u);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

/* Probe request (or another frame type via fc) with a chosen address 2: header + wildcard SSID element. */
static uint32_t frame_from(uint8_t *f, uint16_t fc, const uint8_t *a1, const uint8_t *sa, uint16_t sn)
{
    uint32_t n = hdr(f, fc, a1, sa, bcast, sn);
    f[n++] = 0;
    f[n++] = 0;
    return n;
}

TEST(tx_foreign_sa_only_for_control_probe_requests)
{
    struct rig r;
    uint8_t f[128];
    static const uint8_t la_unicast[6] = { 0x06, 0xaa, 0xbb, 0xcc, 0xdd, 0x01 };  /* bit 1 set, bit 0 clear */
    static const uint8_t global_unicast[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0x02 };
    static const uint8_t la_multicast[6] = { 0x07, 0xaa, 0xbb, 0xcc, 0xdd, 0x03 };
    static const uint8_t global_multicast[6] = { 0x01, 0xaa, 0xbb, 0xcc, 0xdd, 0x04 };
    uint32_t len, cookie = 0, refused;
    CHECK(tx_rig(&r, 6, 0, 1) == 0);
    r.m.log_errors = 0;

    /* default (option 0): any address 2 but ours is refused, nothing reaches the air */
    CHECK_EQ_U32(r.dev->opt.tx_allow_foreign_sa, 0);
    len = frame_from(f, 0x0040, bcast, la_unicast, 1);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    CHECK_EQ_U32(cookie, 0);
    CHECK_EQ_U32(r.f->n_air, 0);
    len = frame_from(f, 0x0040, bcast, chip_mac, 2);                            /* our own MAC still works */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == 0);
    CHECK_EQ_U32(r.f->n_air, 1);

    /* option 1: a locally administered unicast probe request goes out unchanged */
    r.dev->opt.tx_allow_foreign_sa = 1;
    r.f->allow_foreign_sa = 1;                                                  /* the fake otherwise flags a foreign SA */
    len = frame_from(f, 0x0040, bcast, la_unicast, 3);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == 0);
    CHECK(cookie != 0);
    CHECK_EQ_U32(r.f->n_air, 2);
    CHECK_EQ_U32(r.f->air[1].len, len);
    CHECK_MEM_EQ(r.f->air[1].bytes, f, len);
    CHECK_MEM_EQ(r.f->air[1].bytes + 10, la_unicast, 6);
    CHECK_EQ_U32(r.f->air[1].txd[2] & 0xfu, 4);                                 /* TXD2 subtype 4 */

    /* still refused with the option: globally administered, multicast, and every other frame type */
    refused = r.dev->txc.refused;
    len = frame_from(f, 0x0040, bcast, global_unicast, 4);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = frame_from(f, 0x0040, bcast, la_multicast, 5);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = frame_from(f, 0x0040, bcast, global_multicast, 6);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = frame_from(f, 0x0050, peer, la_unicast, 7);                           /* probe response */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = frame_from(f, 0x00b0, peer, la_unicast, 8);                           /* authentication */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = frame_from(f, 0x00d0, bcast, la_unicast, 9);                          /* action */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    len = awdl_data(f, 10, 20);                                                 /* data frame ... */
    memcpy(f + 10, la_unicast, 6);                                              /* ... from a foreign address */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_6M, 0, 0, &cookie) == MT792X_E_TX_FRAME);
    CHECK_EQ_U32(r.dev->txc.refused - refused, 7);
    CHECK_EQ_U32(r.f->n_air, 2);                                                /* nothing else was radiated */
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(tx_shutdown_with_frames_in_flight)
{
    struct rig r;
    uint8_t f[128];
    uint32_t len, cookie = 0, i, sd;
    unsigned resets, aborted = 0;
    struct mt792x_tx_done d;
    CHECK(tx_rig(&r, 6, 0, 1) == 0);
    r.f->tx_mode = FAKE_TX_DEFER;
    len = awdl_action(f, 1);
    for (i = 0; i < 5; i++) CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == 0);
    CHECK(next_done(&r, &d) == 0);
    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    mt792x_shutdown(r.dev);
    CHECK(rig_check_shutdown(&r, rig_shutdown_regs_from(&r, sd), resets, 0x18451800u) == 0);
    while (mt792x_tx_done_get(r.dev, &d) == 1)
        if (d.result == MT792X_TXR_ABORTED) aborted++;
    CHECK_EQ_U32(aborted, 5);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == MT792X_E_STATE);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(tx_completion_consumed_during_command_wait)
{
    struct rig r;
    uint8_t f[128];
    uint32_t len, cookie = 0;
    struct mt792x_tx_done d;
    CHECK(tx_rig(&r, 6, 0, 1) == 0);
    r.f->tx_mode = FAKE_TX_DEFER;
    r.f->event_ring = 4;                                       /* responses behind the completions (F-007) */
    len = awdl_data(f, 5, 50);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 1, 1, &cookie) == 0);
    CHECK_EQ_U32(fake_tx_release(r.f), 1);
    /* the channel switch waits for its response and drains the rings meanwhile */
    CHECK(mt792x_sniffer_set_channel(r.dev, 1, MT792X_BAND_2GHZ, NULL) == 0);
    CHECK(mt792x_tx_done_get(r.dev, &d) == 1);
    CHECK_EQ_U32(d.cookie, cookie);
    CHECK_EQ_U32(d.acked, MT792X_ACK_YES);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(tx_bus_dead_during_tx)
{
    struct rig r;
    uint8_t f[128];
    uint32_t len, cookie = 0, sd;
    unsigned resets, aborted = 0;
    struct mt792x_tx_done d;
    CHECK(tx_rig(&r, 6, 0, 1) == 0);
    r.f->bus_dead_after = r.f->accesses;                       /* the cpu_idx write goes nowhere */
    len = awdl_action(f, 1);
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == 0);
    CHECK_EQ_U32(r.f->n_air, 0);
    CHECK(mt792x_tx_done_get(r.dev, &d) == MT792X_E_BUS_DEAD); /* dma_idx reads all ones */
    CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 0, 0, &cookie) == MT792X_E_BUS_DEAD);
    r.f->dead = 0;
    r.f->bus_dead_after = 0;
    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    mt792x_shutdown(r.dev);
    CHECK(rig_check_shutdown(&r, rig_shutdown_regs_from(&r, sd), resets, 0x18451800u) == 0);
    while (mt792x_tx_done_get(r.dev, &d) == 1)
        if (d.result == MT792X_TXR_ABORTED) aborted++;
    CHECK_EQ_U32(aborted, 1);
    rig_free(&r);
}

TEST(tx_paged_region)
{
    struct rig r;
    uint8_t f[4096];
    uint32_t i, cookie = 0, n = 0;
    struct mt792x_tx_done d;
    CHECK(tx_rig(&r, 44, 1, 1) == 0);
    for (i = 0; i < 40; i++) {
        uint32_t len = awdl_data(f, (uint16_t)i, i == 0 ? MT792X_TX_MAX_FRAME - 24u : 100u + i);
        CHECK(tx_send(&r, f, len, MT792X_RATE_OFDM_12M, 1, 1, &cookie) == 0);
        CHECK_MEM_EQ(r.f->air[i].bytes, f, len);
        while (next_done(&r, &d) == 1) n++;
    }
    CHECK_EQ_U32(n, 40);
    CHECK(rig_check_no_violations(&r) == 0);                    /* every TXWI and frame inside one page */
    rig_free(&r);
}

TEST(tx_power_mode_none_sends_no_limits)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.tx_power_mode = MT792X_TXPWR_NONE;
    CHECK(tx_rig_start(&r, 6) == 0);
    CHECK_EQ_U32(r.f->pwr_msgs[0] + r.f->pwr_msgs[1] + r.f->reg_reads, 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}
