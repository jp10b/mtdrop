/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rx.c - RX data path for a polling host: packet classification on all RX rings, the RXD parser
 * for 802.11 frames, counters and the liveness check.
 *
 * Derived from Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear, Copyright (C) 2020
 * MediaTek Inc.): mt7921/mac.c (mt7921_mac_fill_rx lines 168-436, mt7921_mac_tx_free 476-551,
 * mt7921_queue_rx_skb 578-621), mt76_connac2_mac.h (RXD masks, lines 184-300), mt792x.h
 * (mt792x_get_status_freq_info 444-456), mt7921/mt7921.h:114 (to_rssi), mac80211.c:1229-1256
 * (mt76_rx_signal), mt76_connac_mac.c:1045-1075 (rate fields). Spec: docs/MT7921-BRINGUP.md 11.7-11.9.
 *
 * [HOST] differences: every read is checked against the received length before it happens (mt76
 * reads a group's words before its length check and relies on skb tailroom); a frame shorter than
 * 10 bytes is rejected; the FCS treatment is selectable (mt792x_fcs_mode).
 */
#include "internal.h"
#include "regs.h"

#define LIVENESS_PERIOD_US 250000u
#define REARM_BATCH        32u

uint32_t mt792x_crc32(const uint8_t *p, uint32_t len)
{
    /* IEEE 802.3 CRC-32, reflected polynomial 0xEDB88320, init and final XOR 0xFFFFFFFF;
     * the 802.11 FCS is this value, transmitted least significant byte first. */
    uint32_t crc = 0xffffffffu, i;
    int k;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* (RCPI - 220) / 2 with the rounding mt76 gets from unsigned arithmetic: floor. */
static int8_t rcpi_to_dbm(uint32_t rcpi)
{
    int d = (int)rcpi - MT_RCPI_OFFSET;
    return (int8_t)(d >= 0 ? d / 2 : -((-d + 1) / 2));
}

/* mt76_rx_signal (mac80211.c:1229-1256) */
static int8_t combine_signal(uint8_t chain_mask, const int8_t *chain_signal)
{
    int signal = -128;
    uint8_t chains;
    for (chains = chain_mask; chains; chains >>= 1, chain_signal++) {
        int cur = *chain_signal, diff;
        if (!(chains & 1u) || cur > 0) continue;
        if (cur > signal) {
            int t = cur;
            cur = signal;
            signal = t;
        }
        diff = signal - cur;
        if (diff == 0) signal += 3;
        else if (diff <= 2) signal += 2;
        else if (diff <= 6) signal += 1;
    }
    return (int8_t)signal;
}

/* 802.11 MAC header length for the A-MSDU header move (ieee80211_get_hdrlen_from_skb). Only data
 * frames are expected there; returns 0 for anything else. */
static uint32_t data_hdrlen(uint16_t fc)
{
    uint32_t hl = 24;
    if (((fc >> 2) & 3u) != 2u) return 0;
    if ((fc & 0x0300u) == 0x0300u) hl += 6;          /* ToDS + FromDS: address 4 */
    if (fc & 0x0080u) {                               /* QoS subtype bit */
        hl += 2;
        if (fc & 0x8000u) hl += 4;                    /* +HTC */
    }
    return hl;
}

int mt792x_rx_parse(uint8_t *buf, uint32_t len, uint8_t fcs_mode, uint8_t antenna_mask, struct mt792x_rx_frame *out)
{
    uint32_t rxd0, rxd1, rxd2, rxd3, rxd4, off, remove_pad, chfreq, flen, i, v1 = 0;
    uint8_t *frame;
    int hdr_trans;

    if (!buf || !out) return -MT792X_RXE_SHORT;
    mt_memset(out, 0, sizeof(*out));
    out->rssi_dbm = -128;
    out->raw_len = len;
    if (len < 24u) return -MT792X_RXE_SHORT;
    for (i = 0; i < 6; i++) out->rxd[i] = mt_get_le32(buf + 4u * i);
    rxd0 = out->rxd[0];
    rxd1 = out->rxd[1];
    rxd2 = out->rxd[2];
    rxd3 = out->rxd[3];
    rxd4 = out->rxd[4];

    if ((rxd0 & MT_RXD0_LENGTH) != len) out->flags |= MT792X_RXF_LEN_MISMATCH;
    if (rxd1 & MT_RXD1_BAND_IDX) return -MT792X_RXE_BAND_IDX;
    if (rxd2 & MT_RXD2_AMSDU_ERR) return -MT792X_RXE_AMSDU_ERR;
    hdr_trans = (rxd2 & MT_RXD2_HDR_TRANS) != 0;
    if (hdr_trans && (rxd1 & MT_RXD1_CM)) return -MT792X_RXE_HDR_TRANS_CM;
    if (rxd1 & MT_RXD1_ICV_ERR) out->flags |= MT792X_RXF_ICV_ERR;

    chfreq = (rxd3 >> MT_RXD3_CH_FREQ_SHIFT) & 0xffu;
    if (chfreq > 180u) {
        out->band = MT792X_BAND_6GHZ;
        out->channel = (uint8_t)((chfreq - 181u) * 4u + 1u);
    } else {
        out->band = chfreq > 14u ? MT792X_BAND_5GHZ : MT792X_BAND_2GHZ;
        out->channel = (uint8_t)chfreq;
    }
    if (((rxd3 >> MT_RXD3_ADDR_TYPE_SHIFT) & 3u) == MT_RXD3_U2M) out->flags |= MT792X_RXF_UNICAST_TO_ME;
    if (rxd1 & MT_RXD1_FCS_ERR) out->flags |= MT792X_RXF_FCS_ERR;
    out->fcs_ok = (rxd1 & MT_RXD1_FCS_ERR) ? 0u : 1u;
    if (rxd1 & MT_RXD1_TKIP_MIC_ERR) out->flags |= MT792X_RXF_TKIP_MIC_ERR;
    if (((rxd1 >> MT_RXD1_SEC_MODE_SHIFT) & MT_RXD1_SEC_MODE_MASK) != 0 && !(rxd1 & (MT_RXD1_CLM | MT_RXD1_CM)))
        out->flags |= MT792X_RXF_DECRYPTED;
    if (rxd2 & MT_RXD2_FRAG) out->flags |= MT792X_RXF_FRAG;
    remove_pad = (rxd2 >> MT_RXD2_HDR_OFFSET_SHIFT) & 3u;
    if (rxd2 & MT_RXD2_MAX_LEN_ERROR) return -MT792X_RXE_MAX_LEN_ERR;

    /* Optional groups in the order mt7921_mac_fill_rx walks them: 4, 1, 2, 3 (+5). After every
     * group mt76 requires at least one more byte ("(u8 *)rxd - skb->data >= skb->len"). */
    off = 24u;
    if (rxd1 & MT_RXD1_GROUP_4) {
        if (off + 16u >= len) return -MT792X_RXE_GROUP_OVERRUN;
        for (i = 0; i < 4; i++) out->group4[i] = mt_get_le32(buf + off + 4u * i);
        off += 16u;
    }
    if (rxd1 & MT_RXD1_GROUP_1) {
        if (off + 16u >= len) return -MT792X_RXE_GROUP_OVERRUN;
        off += 16u;
    }
    if (rxd1 & MT_RXD1_GROUP_2) {
        if (off + 8u >= len) return -MT792X_RXE_GROUP_OVERRUN;
        out->timestamp = mt_get_le32(buf + off);
        out->flags |= MT792X_RXF_TIMESTAMP;
        off += 8u;
    }
    if (rxd1 & MT_RXD1_GROUP_3) {
        uint32_t v0;
        if (off + 8u >= len) return -MT792X_RXE_GROUP_OVERRUN;
        v0 = mt_get_le32(buf + off);
        v1 = mt_get_le32(buf + off + 4u);
        out->prxv[0] = v0;
        out->prxv[1] = v1;
        off += 8u;
        out->flags |= MT792X_RXF_RATE;
        out->rate_idx = (uint8_t)(v0 & MT_PRXV_TX_RATE);
        out->rate_nss = (uint8_t)(((v0 >> MT_PRXV_NSTS_SHIFT) & 7u) + 1u);
        out->rate_bw = (uint8_t)((v0 >> MT_PRXV_FRAME_MODE_SHIFT) & 7u);
        out->rate_mode = (uint8_t)((v0 >> MT_PRXV_TX_MODE_SHIFT) & 0xfu);
        if (rxd1 & MT_RXD1_GROUP_5) {
            /* 18 words; "Monitor mode would use RCPI described in GROUP 5 instead": word 6 */
            if (off + 24u >= len || off + 72u >= len) return -MT792X_RXE_GROUP_OVERRUN;
            v1 = mt_get_le32(buf + off + 24u);
            off += 72u;
            out->flags |= MT792X_RXF_GROUP5;
        }
        for (i = 0; i < 4; i++) out->chain_rssi[i] = rcpi_to_dbm((v1 >> (8u * i)) & 0xffu);
        if (antenna_mask) {
            out->rssi_dbm = combine_signal(antenna_mask, out->chain_rssi);
        } else {
            out->rssi_dbm = out->chain_rssi[0];
            out->flags |= MT792X_RXF_RSSI_CHAIN0;
        }
    } else if (rxd1 & MT_RXD1_GROUP_5) {
        out->flags |= MT792X_RXF_G5_WITHOUT_G3; /* mt76 does not skip it either */
    }
    if (rxd4 & MT_RXD4_PAYLOAD_FORMAT) out->flags |= MT792X_RXF_AMSDU;

    out->hdr_gap = off + 2u * remove_pad;
    if (out->hdr_gap >= len) return -MT792X_RXE_PAD_OVERRUN;
    frame = buf + out->hdr_gap;
    flen = len - out->hdr_gap;

    if (hdr_trans) {
        /* mt76 reverses the translation of a first fragment only with a station entry */
        if ((rxd1 & MT_RXD1_GROUP_4) && (out->group4[0] & 0x0400u)) return -MT792X_RXE_HDR_TRANS_FRAG;
        out->flags |= MT792X_RXF_HDR_TRANS;
        if (flen < 14u) return -MT792X_RXE_FRAME_SHORT;
    } else {
        if (out->flags & MT792X_RXF_AMSDU) {
            uint32_t hl, k;
            if (flen < 2u) return -MT792X_RXE_AMSDU_HDR;
            hl = data_hdrlen(mt_get_le16(frame));
            if (hl == 0 || hl + 2u > flen) return -MT792X_RXE_AMSDU_HDR;
            for (k = hl; k > 0; k--) frame[k + 1u] = frame[k - 1u]; /* memmove(data + 2, data, hl) */
            frame += 2;
            flen -= 2u;
        }
        if (flen < 10u) return -MT792X_RXE_FRAME_SHORT;
        if (flen >= 14u && mt792x_crc32(frame, flen - 4u) == mt_get_le32(frame + flen - 4u))
            out->flags |= MT792X_RXF_FCS_TRAILER;
        if (flen >= 14u &&
            (fcs_mode == MT792X_FCS_STRIP || (fcs_mode == MT792X_FCS_AUTO && (out->flags & MT792X_RXF_FCS_TRAILER)))) {
            flen -= 4u;
            out->flags |= MT792X_RXF_FCS_STRIPPED;
        }
    }
    out->data = frame;
    out->len = flen;
    return 0;
}

void mt_rx_account_packet(struct mt792x *dev, const uint8_t *p, uint32_t len, uint32_t ring, int in_cmd)
{
    uint32_t rxd0, type, flag;

    if (len < 4u) {
        dev->rxc.other_pkts++;
        return;
    }
    rxd0 = mt_get_le32(p);
    type = rxd0 >> MT_RXD0_PKT_TYPE_SHIFT;
    flag = (rxd0 >> MT_RXD0_PKT_FLAG_SHIFT) & MT_RXD0_PKT_FLAG_MASK;
    if (type == MT_PKT_TYPE_RX_EVENT && flag == 1u) type = MT_PKT_TYPE_NORMAL_MCU;

    switch (type) {
    case MT_PKT_TYPE_RX_EVENT:
        mt_mcu_note_event(dev, p, len, ring, 0);
        break;
    case MT_PKT_TYPE_TXS:
        /* for (rxd += 2; rxd + 8 <= end; rxd += 8) (mt7921/mac.c:605-609) */
        if (len / 4u >= MT_TXS_HDR_WORDS) {
            uint32_t n = (len / 4u - MT_TXS_HDR_WORDS) / MT_TXS_RECORD_WORDS, k;
            dev->rxc.txs_records += n;
            for (k = 0; k < n; k++) mt_tx_on_txs(dev, p + 4u * (MT_TXS_HDR_WORDS + k * MT_TXS_RECORD_WORDS), ring);
        }
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: TX status packet, %u bytes", ring, len);
        break;
    case MT_PKT_TYPE_TXRX_NOTIFY: {
        /* struct mt76_connac_tx_free {le16 rx_byte_cnt; le16 ctrl; le32 txd;} + le32 entries; a
         * wcid-pair entry (BIT(31)) does not count against MSDU_CNT (mt7921/mac.c:494-519). */
        uint32_t count, i, avail;
        if (len < 8u) {
            dev->rxc.other_pkts++;
            break;
        }
        count = mt_get_le16(p + 2) & MT_TX_FREE_MSDU_CNT;
        avail = (len - 8u) / 4u;
        for (i = 0; i < count && i < avail; i++) {
            if (mt_get_le32(p + 8u + 4u * i) & MT_TX_FREE_PAIR) count++;
            else dev->rxc.tx_free_msdus++;
        }
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: TX-free notification, %u bytes", ring, len);
        mt_tx_on_free(dev, p, len, ring);
        break;
    }
    case MT_PKT_TYPE_NORMAL:
    case MT_PKT_TYPE_NORMAL_MCU:
        if (in_cmd) dev->rxc.discarded_in_cmd++;
        else dev->rxc.other_pkts++;
        break;
    default:
        dev->rxc.other_pkts++;
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: packet type %u flag %u (%u bytes) ignored", ring, type, flag, len);
        break;
    }
}

/* Register-level health check every 250 ms: the data path itself reads no registers. */
static int liveness(struct mt792x *dev)
{
    uint64_t now = mt_now(dev);
    uint32_t misc, dummy;

    if (now < dev->next_liveness_us) return 0;
    dev->next_liveness_us = now + LIVENESS_PERIOD_US;
    dev->rxc.liveness_checks++;
    misc = mt_rr(dev, MT_CONN_ON_MISC);
    dummy = mt_rr(dev, MT_WFDMA_DUMMY_CR);
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    if (mt_irq_guard(dev, "liveness")) return MT792X_E_BUS_DEAD;
    if ((misc & MT_TOP_MISC2_FW_N9_RDY) != MT_TOP_MISC2_FW_N9_RDY) {
        mt_logf(dev, MT_LOG_ERROR, "liveness: CONN_ON_MISC 0x%08x, firmware no longer ready", misc);
        return MT792X_E_FW_DEAD;
    }
    if (!(dummy & MT_WFDMA_NEED_REINIT)) {
        mt_logf(dev, MT_LOG_ERROR, "liveness: WFDMA_DUMMY_CR 0x%08x, NEED_REINIT cleared (spec R6)", dummy);
        return MT792X_E_DMA_REINIT;
    }
    return 0;
}

int mt792x_rx_poll(struct mt792x *dev, struct mt792x_rx_frame *out)
{
    static const int ev_rings[2] = { MT792X_RING_RX0, MT792X_RING_RX4 };
    struct mt792x_ring *r2;
    uint32_t budget;
    int i, rc;

    if (!dev || !out) return MT792X_E_INVAL;
    if (dev->state != MT_ST_TOUCHED || !dev->postboot_done) return MT792X_E_STATE;
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    rc = liveness(dev);
    if (rc) return rc;

    for (i = 0; i < 2; i++) {
        struct mt792x_ring *r = &dev->ring[ev_rings[i]];
        budget = r->ndesc;
        while (budget--) {
            uint32_t len = 0;
            rc = mt_ring_rx_take_buf(dev, r, dev->evt, sizeof(dev->evt), &len, 1);
            if (rc < 0) return rc;
            if (rc == 0) break;
            if (rc == 1) mt_rx_account_packet(dev, dev->evt, len, r->hw_idx, 0);
        }
    }

    r2 = &dev->ring[MT792X_RING_RX2];
    if (r2->queued && mt_ring_rx_completed(dev, r2) == r2->queued) dev->rxc.rx2_full++;
    budget = r2->ndesc;
    while (budget--) {
        uint32_t len = 0, rxd0, type;
        uint64_t t = mt_now(dev);
        rc = mt_ring_rx_take_buf(dev, r2, dev->rx_raw, sizeof(dev->rx_raw), &len, 0);
        if (rc < 0) return rc;
        if (rc == 0) break;
        if (rc == 2) continue;
        rxd0 = len >= 4u ? mt_get_le32(dev->rx_raw) : 0u;
        type = rxd0 >> MT_RXD0_PKT_TYPE_SHIFT;
        if (type == MT_PKT_TYPE_RX_EVENT && ((rxd0 >> MT_RXD0_PKT_FLAG_SHIFT) & MT_RXD0_PKT_FLAG_MASK) == 1u)
            type = MT_PKT_TYPE_NORMAL_MCU;
        if (len >= 4u && (type == MT_PKT_TYPE_NORMAL || type == MT_PKT_TYPE_NORMAL_MCU)) {
            int perr = mt792x_rx_parse(dev->rx_raw, len, dev->opt.fcs_mode,
                                       dev->opt.antenna_mask ? dev->opt.antenna_mask : dev->nic.antenna_mask, out);
            if (perr) {
                dev->rxc.malformed++;
                if (-perr > 0 && -perr < MT792X_RXE_COUNT) dev->rxc.malformed_by[-perr]++;
                mt_logf(dev, MT_LOG_DEBUG, "RX ring 2: malformed packet (reason %d, %u bytes, RXD1 0x%08x) dropped", -perr,
                        len, len >= 8u ? mt_get_le32(dev->rx_raw + 4) : 0u);
                continue;
            }
            out->ring = r2->hw_idx;
            out->host_time_us = t;
            dev->rxc.frames++;
            if (!out->fcs_ok) dev->rxc.fcs_err++;
            if (out->flags & MT792X_RXF_ICV_ERR) dev->rxc.icv_err++;
            if (out->flags & MT792X_RXF_HDR_TRANS) {
                if (!dev->rxc.hdr_trans)
                    mt_logf(dev, MT_LOG_INFO, "RX: first frame with hardware header translation (802.3 payload)");
                dev->rxc.hdr_trans++;
            }
            if (out->flags & MT792X_RXF_FCS_TRAILER) dev->rxc.fcs_trailer++;
            if (r2->rearm_pending >= REARM_BATCH) {
                rc = mt_ring_rx_flush(dev, r2);
                if (rc) return rc;
            }
            return 1;
        }
        mt_rx_account_packet(dev, dev->rx_raw, len, r2->hw_idx, 0);
    }
    rc = mt_ring_rx_flush(dev, r2);
    return rc ? rc : 0;
}

const struct mt792x_rx_counters *mt792x_rx_counters(const struct mt792x *dev)
{
    return dev ? &dev->rxc : NULL;
}
