/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tx.c - raw 802.11 transmission on TX ring 0 for a polling host: TXD/TXP construction, token and
 * slot bookkeeping, completion from TX-free notifications and TX status reports, per-rate TX power
 * limits.
 *
 * Derived from Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear, Copyright (C) 2016 Felix
 * Fietkau, (C) 2020-2022 MediaTek Inc.): mt7921/pci_mac.c:8-54 (mt7921e_tx_prepare_skb),
 * mt76_connac_mac.c:162-199 (mt76_connac_write_hw_txp), 409-627 (mt76_connac2_mac_write_txwi and its
 * 802.11 helper), 755-783 (add_txs_skb), mt7921/mac.c:438-551 (add_txs, tx_free), dma.c:322-389,
 * 422-461, 666-745 (TX enqueue / cleanup), tx.c:122-170 (PID), 700-740 (queue selection),
 * mt76_connac_mcu.c:2104-2353,3031-3046 (rate TX power), mt76_connac.h:44-49,211-237,399-427,
 * mt76_connac2_mac.h:40-174. Queue/priority facts from Linux mac80211 v7.0 (wme.c:117-142,
 * tx.c:1765-1781, include/linux/ieee80211.h:2374-2406); no mac80211 code is used.
 * Spec: docs/MT7921-BRINGUP.md section 12.
 */
#include "internal.h"
#include "regs.h"

#define SLOT_QUAR 2u
#define TX_GENS   (8192u / MT792X_TX0_SLOTS) /* token = gen * SLOTS + slot < MT7921_TOKEN_SIZE */

/* mt76_rates (mac80211.c:160-173): CCK long-preamble hw_value 0-3, OFDM 11,15,10,14,9,13,8,12 */
static const uint8_t rate_mode[13] = { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t rate_idx[13] = { 0, 0, 1, 2, 3, 11, 15, 10, 14, 9, 13, 8, 12 };
static const char *const rate_name[13] = { "?", "CCK 1", "CCK 2", "CCK 5.5", "CCK 11", "OFDM 6", "OFDM 9",
                                           "OFDM 12", "OFDM 18", "OFDM 24", "OFDM 36", "OFDM 48", "OFDM 54" };

uint16_t mt792x_tx_rate_value(uint8_t rate)
{
    if (rate < MT792X_RATE_CCK_1M || rate > MT792X_RATE_OFDM_54M) return 0xffffu;
    return (uint16_t)((rate_mode[rate] << MT_TX_RATE_MODE_SHIFT) | rate_idx[rate]);
}

int mt_tx_channel_allowed(uint8_t ch, uint8_t band)
{
    if (band == MT792X_BAND_2GHZ) return ch >= 1 && ch <= 13;
    if (band == MT792X_BAND_5GHZ) return ch == 36 || ch == 40 || ch == 44 || ch == 48;
    return 0;
}

void mt_tx_init(struct mt792x *dev)
{
    uint32_t i;
    mt_memset(dev->slot, 0, sizeof(dev->slot));
    mt_memset(dev->done_q, 0, sizeof(dev->done_q));
    mt_memset(&dev->txc, 0, sizeof(dev->txc));
    for (i = 0; i < MT792X_TX0_SLOTS; i++) dev->slot[i].token = (uint16_t)i;
    dev->done_head = dev->done_count = 0;
    dev->next_pid = MT_PACKET_ID_FIRST;
    dev->next_cookie = 1;
    dev->tx_allowed = 0;
}

const struct mt792x_tx_counters *mt792x_tx_counters(const struct mt792x *dev)
{
    return dev ? &dev->txc : NULL;
}

/* ---- frame classification (what mt76 + mac80211 decide from the header) ---- */

struct tx_class {
    uint8_t type, subtype;
    uint8_t group;        /* address 1 is a group address */
    uint8_t hdrlen;
    uint8_t tid;          /* skb->priority & 7 */
    uint8_t psd;          /* management queue (ALTX0) */
    uint8_t ac;           /* mac80211 AC (0 VO, 1 VI, 2 BE, 3 BK) for the AC queues */
    uint8_t fix_rate;
    uint8_t wcid;         /* TXD1 WLAN_IDX: the vif's entry, or a station from mt792x_sta_add (ADR-0007) */
};

/* ieee802_1d_to_ac (mac80211 wme.c:23): BE BK BK BE VI VI VO VO with VO 0 .. BK 3 */
static const uint8_t up_to_ac[8] = { 2, 3, 3, 2, 1, 1, 0, 0 };

static int same_mac(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < 6u; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static int tx_reject(struct mt792x *dev, const char *why)
{
    mt_logf(dev, MT_LOG_ERROR, "TX refused: %s", why);
    dev->txc.refused++;
    return MT792X_E_TX_FRAME;
}

static int classify(struct mt792x *dev, const struct mt792x_tx_req *req, struct tx_class *c)
{
    const uint8_t *f = req->frame;
    uint16_t fc, sc;
    uint8_t flags;

    mt_memset(c, 0, sizeof(*c));
    if (!f || req->len < 24u) return tx_reject(dev, "frame shorter than a 24-byte 802.11 header");
    if (req->len > MT792X_TX_MAX_FRAME) return tx_reject(dev, "frame longer than MT792X_TX_MAX_FRAME");
    fc = mt_get_le16(f);
    sc = mt_get_le16(f + 22);
    flags = (uint8_t)(fc >> 8);
    c->type = (uint8_t)((fc >> 2) & 3u);
    c->subtype = (uint8_t)((fc >> 4) & 0xfu);
    c->group = (uint8_t)(f[4] & 1u);
    if (fc & 3u) return tx_reject(dev, "protocol version not 0");
    if (c->type != 0 && c->type != 2) return tx_reject(dev, "only management and data frames are supported");
    /* [HOST] features mt76 handles that this library does not: fragments (TXD2 FRAG), protection (keys),
     * +HTC (Order bit), beacons (FW queue), block-ack sessions (TXD5 ADD_BA) */
    if (flags & 0x04u) return tx_reject(dev, "More Fragments set (fragments not supported)");
    if (sc & 0xfu) return tx_reject(dev, "fragment number not 0");
    if (flags & 0x40u) return tx_reject(dev, "Protected Frame set (no keys)");
    if (flags & 0x80u) return tx_reject(dev, "Order (+HTC) set");
    if (!same_mac(f + 10, dev->own_mac)) {
        /* ACK control experiment (opt.tx_allow_foreign_sa): a probe request from a locally administered unicast
         * address the chip does not acknowledge; nothing else may carry a foreign address 2. */
        int control_probe = dev->opt.tx_allow_foreign_sa && c->type == 0 && c->subtype == 4 && (f[10] & 3u) == 2u;
        if (!control_probe) return tx_reject(dev, "address 2 is not our MAC");
    }
    if (c->group && req->want_ack) return tx_reject(dev, "ACK requested for a group-addressed frame");
    if (req->want_status && !req->want_ack) return tx_reject(dev, "TX status requested without ACK");
    if (req->tx_count > MT_TX_REM_COUNT_DEFAULT) return tx_reject(dev, "tx_count above 15");
    if (mt792x_tx_rate_value(req->rate) == 0xffffu) return tx_reject(dev, "unknown rate");
    if (req->rate <= MT792X_RATE_CCK_11M && dev->cur_band != MT792X_BAND_2GHZ) return tx_reject(dev, "CCK rate on 5 GHz");

    if (c->type == 0) {
        int bufferable = 0;
        if (flags & 0x03u) return tx_reject(dev, "management frame with ToDS/FromDS");
        if (c->subtype == 8) return tx_reject(dev, "beacons are not supported");
        c->hdrlen = 24;
        /* ieee80211_is_bufferable_mmpdu (include/linux/ieee80211.h:2374-2406) */
        if (c->subtype == 10 || c->subtype == 12) {
            bufferable = 1;
        } else if (c->subtype == 13) {
            if (req->len < 26u) bufferable = 1;
            else if (f[24] != 4 && f[24] != 9) bufferable = 1;           /* not Public / Protected Dual */
            else bufferable = !(f[25] == 32 || f[25] == 33);             /* FTM request / response */
            if (req->len >= 25u && f[24] == 3) return tx_reject(dev, "block-ack action frames are not supported");
        }
        /* mt76_txq_schedule_pending_wcid (tx.c:718-725): no data present and (not bufferable or deauth)
         * -> MT_TXQ_PSD; the vif and global wcids both have MT_WCID_TX_INFO_SET */
        c->psd = (uint8_t)(!bufferable || c->subtype == 12);
        c->tid = 7;          /* ieee80211_select_queue_80211: !data -> priority 7 (wme.c:132-135) */
        c->ac = up_to_ac[7]; /* VO */
        c->fix_rate = 1;     /* !ieee80211_is_data (mt76_connac_mac.c:459-461) */
    } else {
        int qos = (c->subtype & 0x8u) != 0, data_present = (c->subtype & 0x4u) == 0;
        c->hdrlen = (uint8_t)(24u + ((flags & 0x03u) == 0x03u ? 6u : 0u) + (qos ? 2u : 0u));
        if (req->len < c->hdrlen) return tx_reject(dev, "data frame shorter than its header");
        c->tid = qos ? (uint8_t)(f[c->hdrlen - 2u] & 7u) : 0u;        /* wme.c:136-142 */
        c->ac = up_to_ac[c->tid];
        c->psd = (uint8_t)!data_present;                                 /* tx.c:718-725 */
        c->fix_rate = (uint8_t)(c->group || dev->opt.tx_data_fixed_rate);
        if (dev->opt.tx_data_altx && !req->sta) c->psd = 1; /* [HOST] opt.tx_data_altx (F-019): queue ALTX0, TID as computed */
    }
    c->wcid = dev->tx_wcid;
    if (req->sta) {
        /* ADR-0007: unicast data to a known station goes with its WTBL entry on its AC queue, like mt76's data path */
        if (c->type != 2u || c->group) return tx_reject(dev, "a station index on a management or group frame");
        if (!mt_sta_lookup(dev, req->sta, f + 4)) return tx_reject(dev, "station index unknown or not address 1");
        c->wcid = req->sta;
    }
    return 0;
}

/* mt76_connac2_mac_write_txwi + mt76_connac_write_hw_txp for one linear frame. */
static void build_txwi(const struct mt792x *dev, const struct mt792x_tx_req *req, const struct tx_class *c,
                       uint8_t pid, uint16_t token, uint32_t frame_bus, uint8_t *w)
{
    uint32_t t[8], q_idx, wmm = dev->tx_omac % 4u; /* wmm_idx = idx % MT76_CONNAC_MAX_WMM_SETS (mt7921/main.c:321) */
    uint16_t sc = mt_get_le16(req->frame + 22);
    uint8_t mask = dev->opt.antenna_mask ? dev->opt.antenna_mask : dev->nic.antenna_mask;
    uint32_t i;

    q_idx = c->psd ? MT_LMAC_ALTX0 : wmm * 4u + (3u - c->ac);          /* mt76_connac_lmac_mapping: 3 - ac */
    t[0] = ((req->len + MT_TXD_SIZE) & MT_TXD0_TX_BYTES) | (MT_TX_TYPE_CT << MT_TXD0_PKT_FMT_SHIFT) |
           (q_idx << MT_TXD0_Q_IDX_SHIFT);
    t[1] = MT_TXD1_LONG_FORMAT | (c->wcid & MT_TXD1_WLAN_IDX) | ((uint32_t)dev->tx_omac << MT_TXD1_OWN_MAC_SHIFT) |
           (MT_HDR_FORMAT_802_11 << MT_TXD1_HDR_FORMAT_SHIFT) | ((uint32_t)(c->hdrlen / 2u) << MT_TXD1_HDR_INFO_SHIFT) |
           ((uint32_t)c->tid << MT_TXD1_TID_SHIFT);
    t[2] = ((uint32_t)c->type << MT_TXD2_FRAME_TYPE_SHIFT) | c->subtype | (c->group ? MT_TXD2_MULTICAST : 0u);
    t[3] = ((uint32_t)(req->tx_count ? req->tx_count : MT_TX_REM_COUNT_DEFAULT) << MT_TXD3_REM_TX_COUNT_SHIFT) |
           (req->want_ack ? 0u : MT_TXD3_NO_ACK) |
           MT_TXD3_SN_VALID | ((uint32_t)(sc >> 4) << MT_TXD3_SEQ_SHIFT);   /* IEEE80211_TX_CTL_INJECTED */
    t[4] = 0;
    t[5] = pid | (pid >= MT_PACKET_ID_FIRST ? MT_TXD5_TX_STATUS_HOST : 0u);
    t[6] = 0;
    t[7] = ((uint32_t)c->type << MT_TXD7_TYPE_SHIFT) | ((uint32_t)c->subtype << MT_TXD7_SUB_TYPE_SHIFT);
    if (c->fix_rate) {
        /* ant_to_spe (mt76_connac.h:418-427) */
        static const uint8_t ant_to_spe[16] = { 0, 0, 1, 0, 3, 2, 4, 0, 9, 8, 6, 10, 16, 12, 18, 0 };
        uint8_t spe = mask < 16u ? ant_to_spe[mask] : 0u, n = 0, m = mask;
        while (m) {
            n = (uint8_t)(n + (m & 1u));
            m >>= 1;
        }
        if (!spe && n > 1u) spe = 24u;                                  /* 24 + phy_idx (0) */
        t[2] |= MT_TXD2_FIX_RATE | MT_TXD2_HTC_VLD;
        t[6] |= MT_TXD6_FIXED_BW | ((uint32_t)mt792x_tx_rate_value(req->rate) << MT_TXD6_TX_RATE_SHIFT);
        t[3] |= MT_TXD3_BA_DISABLE;
        if (dev->opt.tx_spe_mode != MT792X_SPE_LINUX_7_0) t[7] |= (uint32_t)spe << MT_TXD7_SPE_IDX_SHIFT;
    }
    for (i = 0; i < 8u; i++) mt_put_le32(w + 4u * i, t[i]);
    /* struct mt76_connac_hw_txp: msdu_id[4] (LE16), ptr[2] = {le32 buf0; le16 len0; le16 len1; le32 buf1} */
    mt_memset(w + MT_TXD_SIZE, 0, MT_HW_TXP_SIZE);
    mt_put_le16(w + MT_TXD_SIZE, (uint16_t)(token | MT_MSDU_ID_VALID));
    mt_put_le32(w + MT_TXD_SIZE + 8u, frame_bus);
    mt_put_le16(w + MT_TXD_SIZE + 12u, (uint16_t)((req->len & MT_TXD_LEN_MASK) | MT_TXD_LEN_LAST));
}

/* ---- completion ---- */

static int queue_record(struct mt792x *dev, struct mt792x_tx_slot *s, uint8_t result, uint64_t t)
{
    struct mt792x_tx_done *d;
    if (dev->done_count >= MT792X_TX_DONE_QUEUE) return 0; /* cannot happen: submission keeps room */
    d = &dev->done_q[(dev->done_head + dev->done_count) % MT792X_TX_DONE_QUEUE];
    mt_memset(d, 0, sizeof(*d));
    d->cookie = s->cookie;
    d->result = result;
    d->free_status = s->freed ? s->free_status : 0xffu;
    d->tx_count = s->tx_count;
    d->has_txs = s->has_txs;
    if (s->has_txs) {
        mt_memcpy(d->txs, s->txs, sizeof(d->txs));
        d->txs_rate = (uint16_t)(s->txs[0] & MT_TXS0_TX_RATE);
    }
    if (!s->want_ack) d->acked = MT792X_ACK_NOT_REQUESTED;
    else if (!s->has_txs) d->acked = MT792X_ACK_UNKNOWN;
    else d->acked = (s->txs[0] & MT_TXS0_ACK_ERROR_MASK) ? MT792X_ACK_NO : MT792X_ACK_YES;
    d->elapsed_us = (uint32_t)(t - s->t_submit);
    dev->done_count++;
    if (result == MT792X_TXR_DONE) dev->txc.done++;
    else if (result == MT792X_TXR_LOST) dev->txc.lost++;
    else dev->txc.aborted++;
    if (d->acked == MT792X_ACK_YES) dev->txc.acked++;
    if (d->acked == MT792X_ACK_NO) dev->txc.not_acked++;
    mt_logf(dev, result == MT792X_TXR_DONE ? MT_LOG_DEBUG : MT_LOG_WARN,
            "TX cookie %u token %u: %s, ack %u, TX-free status %u count %u, TXS %u (0x%08x), %u us", d->cookie, s->token,
            result == MT792X_TXR_DONE ? "done" : result == MT792X_TXR_LOST ? "LOST (no TX-free, slot quarantined)" : "aborted",
            d->acked, d->free_status, d->tx_count, d->has_txs, d->txs[0], d->elapsed_us);
    return 1;
}

static void next_gen(struct mt792x_tx_slot *s, uint32_t i)
{
    s->gen = (uint16_t)((s->gen + 1u) % TX_GENS);
    s->token = (uint16_t)(s->gen * MT792X_TX0_SLOTS + i);
}

static void slot_release(struct mt792x_tx_slot *s, uint32_t i)
{
    uint16_t gen = s->gen;
    mt_memset(s, 0, sizeof(*s));
    s->gen = gen;
    next_gen(s, i);
    s->state = SLOT_FREE;
}

/* Finalises a BUSY slot whose record is complete. */
static void try_finish(struct mt792x *dev, uint32_t i, uint64_t now)
{
    struct mt792x_tx_slot *s = &dev->slot[i];
    if (s->state != SLOT_BUSY || !s->freed) return;
    if (s->want_status && !s->has_txs && now - s->t_event < MT_TXS_WAIT_US) return;
    if (s->want_status && !s->has_txs)
        mt_logf(dev, MT_LOG_INFO, "TX cookie %u: no TX status report within %u ms of the TX-free", s->cookie,
                MT_TXS_WAIT_US / 1000u);
    if (queue_record(dev, s, MT792X_TXR_DONE, s->t_event)) slot_release(s, i);
}

void mt_tx_on_free(struct mt792x *dev, const uint8_t *p, uint32_t len, uint32_t ring)
{
    /* struct mt76_connac_tx_free {le16 rx_byte_cnt; le16 ctrl; le32 txd;} + le32 entries
     * (mt7921/mac.c:476-551). Pair entries (BIT(31)) do not count against MSDU_CNT. */
    uint32_t count, i, avail;
    uint64_t now = mt_now(dev);
    if (len < 8u) return;
    count = mt_get_le16(p + 2) & MT_TX_FREE_MSDU_CNT;
    avail = (len - 8u) / 4u;
    if (count > avail) {
        mt_logf(dev, MT_LOG_WARN, "RX ring %u: TX-free announces %u entries, %u present: truncated", ring, count, avail);
    }
    for (i = 0; i < count && i < avail; i++) {
        uint32_t e = mt_get_le32(p + 8u + 4u * i), msdu, k;
        struct mt792x_tx_slot *s;
        if (e & MT_TX_FREE_PAIR) {
            count++;
            continue;
        }
        dev->txc.txfree_entries++;
        msdu = (e >> MT_TX_FREE_MSDU_ID_SHIFT) & MT_TX_FREE_MSDU_ID_MASK;
        k = msdu % MT792X_TX0_SLOTS;
        s = &dev->slot[k];
        if (msdu >= TX_GENS * MT792X_TX0_SLOTS || s->token != msdu || s->state == SLOT_FREE || s->freed) {
            dev->txc.txfree_unknown++;
            mt_logf(dev, MT_LOG_WARN, "RX ring %u: TX-free for token %u which is not in flight (entry 0x%08x)", ring, msdu, e);
            continue;
        }
        if (s->state == SLOT_QUAR) {
            dev->txc.txfree_late++;
            dev->txc.quarantined--;
            mt_logf(dev, MT_LOG_INFO, "TX-free for quarantined token %u arrived late: slot %u reusable again", msdu, k);
            slot_release(s, k);
            continue;
        }
        s->freed = 1;
        s->free_status = (uint8_t)((e >> MT_TX_FREE_STATUS_SHIFT) & 3u);
        s->tx_count = (uint8_t)((e & MT_TX_FREE_COUNT) > 255u ? 255u : (e & MT_TX_FREE_COUNT));
        s->t_event = now;
        try_finish(dev, k, now);
    }
}

void mt_tx_on_txs(struct mt792x *dev, const uint8_t *rec, uint32_t ring)
{
    uint32_t w[4], i, wcid, pid;
    uint64_t now = mt_now(dev);
    for (i = 0; i < 4u; i++) w[i] = mt_get_le32(rec + 4u * i);
    dev->txc.txs_records++;
    wcid = (w[2] >> MT_TXS2_WCID_SHIFT) & 0x3ffu;
    pid = w[3] >> MT_TXS3_PID_SHIFT;
    /* mt7921_mac_add_txs (mt7921/mac.c:438-474): format > 1 and pid < MT_PACKET_ID_FIRST ignored */
    if (((w[0] >> MT_TXS0_TXS_FORMAT_SHIFT) & 3u) <= 1u && pid >= MT_PACKET_ID_FIRST) {
        for (i = 0; i < MT792X_TX0_SLOTS; i++) {
            struct mt792x_tx_slot *s = &dev->slot[i];
            if (s->state == SLOT_BUSY && s->want_status && !s->has_txs && s->pid == pid && wcid == s->wcid) {
                mt_memcpy(s->txs, w, sizeof(w));
                s->has_txs = 1;
                s->t_event = now;
                mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: TXS for cookie %u (pid %u): word0 0x%08x", ring, s->cookie, pid, w[0]);
                try_finish(dev, i, now);
                return;
            }
        }
    }
    dev->txc.txs_unmatched++;
    mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: TXS not matched (wcid %u pid %u word0 0x%08x)", ring, wcid, pid, w[0]);
}

/* mt76_dma_tx_cleanup: every descriptor from tail up to dma_idx is consumed. */
static int reclaim(struct mt792x *dev)
{
    struct mt792x_ring *r = &dev->ring[MT792X_RING_TX0];
    uint32_t idx;
    if (!r->queued) return 0;
    idx = mt_rr(dev, r->regs + MT_RING_DMA_IDX);
    if (dev->bus_dead || idx == 0xffffffffu) {
        dev->bus_dead = 1;
        return MT792X_E_BUS_DEAD;
    }
    if (idx >= r->ndesc) {
        mt_logf(dev, MT_LOG_ERROR, "TX ring 0: dma_idx reads %u >= size %u (hung bus)", idx, r->ndesc);
        dev->bus_dead = 1;
        return MT792X_E_BUS_DEAD;
    }
    while (r->queued && r->tail != idx) {
        r->tail = (r->tail + 1u) % r->ndesc;
        r->queued--;
        dev->txc.desc_reclaimed++;
    }
    return 0;
}

static void timeouts(struct mt792x *dev)
{
    uint64_t now = mt_now(dev);
    uint64_t limit = dev->opt.tx_timeout_ms ? (uint64_t)dev->opt.tx_timeout_ms * 1000u : MT_TX_TIMEOUT_DEFAULT_US;
    uint32_t i;
    for (i = 0; i < MT792X_TX0_SLOTS; i++) {
        struct mt792x_tx_slot *s = &dev->slot[i];
        if (s->state != SLOT_BUSY) continue;
        if (s->freed) {
            try_finish(dev, i, now);
        } else if (now - s->t_submit >= limit && dev->done_count < MT792X_TX_DONE_QUEUE) {
            /* The chip may still read this buffer: never reuse it unless its token comes back. */
            if (queue_record(dev, s, MT792X_TXR_LOST, now)) {
                s->state = SLOT_QUAR;
                dev->txc.quarantined++;
            }
        }
    }
}

int mt792x_tx_done_get(struct mt792x *dev, struct mt792x_tx_done *out)
{
    if (!dev || !out) return MT792X_E_INVAL;
    if (dev->state == MT_ST_TOUCHED && dev->postboot_done && !dev->bus_dead) {
        int err = reclaim(dev);
        if (err) return err;
        timeouts(dev);
    }
    if (!dev->done_count) return 0;
    *out = dev->done_q[dev->done_head];
    dev->done_head = (dev->done_head + 1u) % MT792X_TX_DONE_QUEUE;
    dev->done_count--;
    return 1;
}

void mt_tx_abort_all(struct mt792x *dev)
{
    uint64_t now = mt_now(dev);
    uint32_t i, n = 0;
    for (i = 0; i < MT792X_TX0_SLOTS; i++) {
        struct mt792x_tx_slot *s = &dev->slot[i];
        if (s->state == SLOT_BUSY) {
            (void)queue_record(dev, s, s->freed ? MT792X_TXR_DONE : MT792X_TXR_ABORTED, now);
            n++;
        }
        if (s->state != SLOT_FREE) slot_release(s, i);
    }
    dev->txc.quarantined = 0;
    dev->tx_allowed = 0;
    if (n) mt_logf(dev, MT_LOG_INFO, "shutdown: %u frame(s) still in flight reported as aborted", n);
}

/* ---- submission ---- */

int mt792x_tx_frame(struct mt792x *dev, const struct mt792x_tx_req *req, uint32_t *cookie)
{
    struct mt792x_ring *r;
    struct tx_class c;
    uint32_t i, k = MT792X_TX0_SLOTS, inflight = 0, off, bus_txwi = 0, bus_frame = 0, desc;
    uint8_t txwi[MT792X_TXWI_SIZE], pid = 0;
    struct mt792x_tx_slot *s;
    int err;

    if (cookie) *cookie = 0;
    if (!dev || !req || !cookie) return MT792X_E_INVAL;
    if (dev->state != MT_ST_TOUCHED || !dev->sniff_started) return MT792X_E_STATE;
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    if (!dev->tx_allowed || !mt_tx_channel_allowed(dev->cur_channel, dev->cur_band)) {
        dev->txc.refused++;
        mt_logf(dev, MT_LOG_ERROR, "TX refused: %s (channel %u band %u)",
                dev->tx_allowed ? "channel not in the TX allow-list" : "TX not enabled for this sniffer",
                dev->cur_channel, dev->cur_band);
        return MT792X_E_TX_DENIED;
    }
    err = classify(dev, req, &c);
    if (err) return err;

    r = &dev->ring[MT792X_RING_TX0];
    if (r->queued + 1u >= r->ndesc - 1u) {
        err = reclaim(dev);
        if (err) return err;
    }
    for (i = 0; i < MT792X_TX0_SLOTS; i++) {
        if (dev->slot[i].state == SLOT_BUSY) inflight++;
        if (dev->slot[i].state == SLOT_FREE && k == MT792X_TX0_SLOTS) k = i;
    }
    /* Every in-flight frame produces one record: keep room for all of them. */
    if (k == MT792X_TX0_SLOTS || dev->done_count + inflight + 1u > MT792X_TX_DONE_QUEUE ||
        r->queued + 1u >= r->ndesc - 1u) {
        dev->txc.refused++;
        mt_logf(dev, MT_LOG_DEBUG, "TX busy: %u in flight, %u quarantined, %u records unread, ring queued %u", inflight,
                dev->txc.quarantined, dev->done_count, r->queued);
        return MT792X_E_TX_BUSY;
    }
    if (req->want_status) {
        /* idr_alloc(&wcid->pktid, .., MT_PACKET_ID_FIRST, MT_PACKET_ID_MASK): 3..126, never one in flight */
        uint32_t tries;
        for (tries = 0; tries < MT_PACKET_ID_MASK; tries++) {
            int used = 0;
            uint8_t cand = dev->next_pid;
            dev->next_pid = (uint8_t)(dev->next_pid + 1u >= MT_PACKET_ID_MASK ? MT_PACKET_ID_FIRST : dev->next_pid + 1u);
            for (i = 0; i < MT792X_TX0_SLOTS; i++)
                if (dev->slot[i].state == SLOT_BUSY && dev->slot[i].want_status && dev->slot[i].pid == cand) used = 1;
            if (!used) {
                pid = cand;
                break;
            }
        }
    }

    s = &dev->slot[k];
    off = MT792X_OFF_TX0_SLOT + k * MT792X_TX0_SLOT_SIZE;
    err = mt792x_dma_bus(dev, off, MT792X_TXWI_SIZE, 4u, &bus_txwi);
    if (!err) err = mt792x_dma_bus(dev, off + MT792X_TXWI_SIZE, req->len, 4u, &bus_frame);
    if (err) return err;
    build_txwi(dev, req, &c, pid, s->token, bus_frame, txwi);
    mt_dma_write(dev, off, txwi, MT792X_TXWI_SIZE);
    mt_dma_write(dev, off + MT792X_TXWI_SIZE, req->frame, req->len);

    s->state = SLOT_BUSY;
    s->want_ack = req->want_ack ? 1u : 0u;
    s->want_status = req->want_status ? 1u : 0u;
    s->wcid = c.wcid;
    s->pid = pid;
    s->freed = s->has_txs = 0;
    s->cookie = dev->next_cookie++;
    if (!dev->next_cookie) dev->next_cookie = 1;
    s->t_submit = s->t_event = mt_now(dev);

    /* mt76_dma_add_buf with one segment: buf0 = TXWI (txwi_size 64), LAST_SEC0, buf1/info 0 */
    desc = r->desc_off + r->head * MT792X_DESC_SIZE;
    mt_dma_st32(dev, desc + MT_DESC_BUF0, bus_txwi);
    mt_dma_st32(dev, desc + MT_DESC_BUF1, 0);
    mt_dma_st32(dev, desc + MT_DESC_INFO, 0);
    mt_dma_st32(dev, desc + MT_DESC_CTRL, (MT792X_TXWI_SIZE << MT_DMA_CTL_SD_LEN0_SHIFT) | MT_DMA_CTL_LAST_SEC0);
    r->head = (r->head + 1u) % r->ndesc;
    r->queued++;
    mt_wr(dev, r->regs + MT_RING_CPU_IDX, r->head);
    if (dev->bus_dead) {
        /* The chip may have seen the descriptor: the slot is never reused, no cookie is returned. */
        s->state = SLOT_QUAR;
        dev->txc.quarantined++;
        dev->txc.refused++;
        return MT792X_E_BUS_DEAD;
    }
    dev->txc.submitted++;
    *cookie = s->cookie;
    mt_logf(dev, dev->txc.submitted == 1u ? MT_LOG_INFO : MT_LOG_DEBUG,
            "TX cookie %u: %u bytes, fc 0x%04x, %s, %s, q 0x%02x, wcid %u omac %u, token %u pid %u, channel %u",
            s->cookie, req->len, mt_get_le16(req->frame), c.fix_rate ? rate_name[req->rate] : "firmware rate",
            req->want_ack ? "ACK" : "no ACK", c.psd ? MT_LMAC_ALTX0 : (dev->tx_omac % 4u) * 4u + 3u - c.ac, c.wcid,
            dev->tx_omac, s->token, pid, dev->cur_channel);
    return 0;
}

/* ---- per-rate TX power limits (mt76_connac_mcu_rate_txpower_band) ---- */

static const uint8_t pwr_ch_2g[14] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static const uint8_t pwr_ch_5g[48] = { 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 100,
                                       102, 104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 132, 134,
                                       136, 138, 140, 142, 144, 149, 151, 153, 155, 157, 159, 161, 165, 169, 173, 177 };

/* Limit in 0.5 dB for one channel: what mt76 programs for regulatory domain PL (wireless-regdb db.txt:
 * 2400-2483.5 MHz 100 mW = 20 dBm, 5150-5250 MHz 200 mW = 23 dBm -> 2 * max_reg_power = 40 / 46).
 * [HOST] Channels we never transmit on get the band's most restrictive PL value (5725-5875 MHz
 * 25 mW = 14 dBm -> 28) - never more than mt76 would program. */
static int8_t pwr_limit(const struct mt792x *dev, uint8_t band, uint8_t ch)
{
    int v;
    if (band == MT792X_BAND_2GHZ) v = 40;
    else if (ch >= 36 && ch <= 48) v = 46;
    else v = 28;
    if (dev->opt.tx_power_max_half_db && dev->opt.tx_power_max_half_db < v) v = dev->opt.tx_power_max_half_db;
    return (int8_t)v;
}

/* mt76_connac_mcu_build_sku for connac2 without a device tree: every rate entry = the limit, the
 * gaps 127 (memset max_power), CCK entries only on 2.4 GHz. */
static void build_sku(uint8_t *sku, uint8_t band, int8_t v)
{
    uint32_t i, k, off;
    mt_memset(sku, MT_SKU_MAX_POWER, MT_SKU_POWER_LIMIT);
    if (band == MT792X_BAND_2GHZ)
        for (i = 0; i < 4u; i++) sku[i] = (uint8_t)v;               /* cck[4] */
    off = 4;
    for (i = 0; i < 8u; i++) sku[off++] = (uint8_t)v;               /* ofdm[8] */
    for (i = 0; i < 16u; i++) sku[off++] = (uint8_t)v;              /* ht: 2 x 8 */
    sku[off++] = (uint8_t)v;                                        /* mcs[0][0] */
    for (k = 0; k < 4u; k++) {                                      /* vht: 4 x (10 of 12) */
        for (i = 0; i < 10u; i++) sku[off + i] = (uint8_t)v;
        off += 12u;
    }
    for (i = 0; i < 84u; i++) sku[off++] = (uint8_t)v;              /* he: 7 x 12 */
}

static int power_band(struct mt792x *dev, uint8_t band)
{
    uint8_t msg[MT_TX_POWER_TLV_LEN + MT_TX_POWER_BATCH * (1u + MT_SKU_POWER_LIMIT)];
    const uint8_t *list = band == MT792X_BAND_2GHZ ? pwr_ch_2g : pwr_ch_5g;
    uint32_t n = band == MT792X_BAND_2GHZ ? (uint32_t)sizeof(pwr_ch_2g) : (uint32_t)sizeof(pwr_ch_5g);
    uint8_t last_ch = dev->nic.has_5ghz ? 177u : 14u;  /* has_6ghz is never set for MT7921 here */
    uint32_t idx = 0, b, batches = (n + MT_TX_POWER_BATCH - 1u) / MT_TX_POWER_BATCH;
    int err;

    for (b = 0; b < batches; b++) {
        uint32_t num = (b == batches - 1u) ? n - b * MT_TX_POWER_BATCH : MT_TX_POWER_BATCH, j, len;
        uint8_t req_rr[8];
        mt_memset(msg, 0, MT_TX_POWER_TLV_LEN);
        /* struct mt76_connac_tx_power_limit_tlv: ver 0, pad0, le16 len 0, n_chan, band (1 = 2.4, 2 = 5 GHz),
         * last_msg, pad1, alpha2[4] (dev->alpha2 "00": the library's world domain, spec 11.5), pad2[32] */
        msg[4] = (uint8_t)num;
        msg[5] = band == MT792X_BAND_2GHZ ? 1u : 2u;
        msg[8] = '0';
        msg[9] = '0';
        for (j = 0; j < num; j++, idx++) {
            uint8_t *e = msg + MT_TX_POWER_TLV_LEN + j * (1u + MT_SKU_POWER_LIMIT);
            msg[6] = list[idx] == last_ch ? 1u : 0u;
            e[0] = list[idx];
            build_sku(e + 1, band, pwr_limit(dev, band, list[idx]));
        }
        len = MT_TX_POWER_TLV_LEN + num * (1u + MT_SKU_POWER_LIMIT);
        err = mt_mcu_send(dev, "SET_RATE_TX_POWER", MT_MCU_CE(MCU_CE_SET_RATE_TX_POWER), msg, len, 0, MT_MCU_TIMEOUT_US,
                          NULL);
        if (err) return err;
        /* "read a CR to avoid PSE buffer underflow" (mt76_connac_mcu.c:2319-2320): MCU_CE_QUERY(REG_READ) */
        mt_put_le32(req_rr, MT_PSE_BASE);
        mt_put_le32(req_rr + 4, 0);
        err = mt_mcu_send(dev, "REG_READ PSE", MT_MCU_CE(MCU_CE_REG_READ) | MT_MCU_FIELD_QUERY, req_rr, sizeof(req_rr), 1,
                          MT_MCU_TIMEOUT_US, NULL);
        if (err) return err;
    }
    return 0;
}

int mt_tx_power_setup(struct mt792x *dev)
{
    int err = 0;
    if (dev->opt.tx_power_mode == MT792X_TXPWR_NONE) {
        mt_logf(dev, MT_LOG_WARN, "TX power: SET_RATE_TX_POWER NOT sent (tx_power_mode none): firmware defaults apply");
        return 0;
    }
    mt_logf(dev, MT_LOG_INFO, "TX power: mt76 limits for PL: 2.4 GHz %d, 5 GHz 36-48 %d, other 5 GHz %d (0.5 dB units)",
            pwr_limit(dev, MT792X_BAND_2GHZ, 1), pwr_limit(dev, MT792X_BAND_5GHZ, 36), pwr_limit(dev, MT792X_BAND_5GHZ, 149));
    if (dev->nic.has_2ghz) err = power_band(dev, MT792X_BAND_2GHZ);
    if (!err && dev->nic.has_5ghz) err = power_band(dev, MT792X_BAND_5GHZ);
    return err;
}
