/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Station records for peers (ADR-0007 S1): MCU_UNI_CMD(STA_REC_UPDATE) as mt76 sends it for a station of an
 * AP-type BSS, reduced to what unicast data with a fixed rate needs - a BASIC TLV and a WTBL TLV with GENERIC, RX
 * and HDR_TRANS. No HT/VHT, rate adaptation or block-ack TLVs yet (S4/S5).
 *
 * Sources (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear): mt7921/main.c:806-855 (mt7921_mac_sta_add: wcid from
 * mt76_wcid_alloc(mask, MT792x_WTBL_STA - 1), wtbl_update(ADM_COUNT_CLEAR), STA_REC_UPDATE), mt7921/mcu.c:1099-1121,
 * mt76_connac_mcu.c:318-342 (sta_req_hdr), 344-367 (wtbl_req_hdr), 414-479 (BASIC), 514-541 (HDR_TRANS),
 * 595-643 (GENERIC, RX), 1094-1150 (mt76_connac_mcu_sta_cmd), mt76_connac_mcu.h:306-327, 667-734, 836-924, 1028-1033.
 * Layout and values: docs/MT7921-BRINGUP.md "Station records". */
#include "internal.h"
#include "regs.h"

static int same_addr(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < 6u; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

#define STA_REC_BASIC 0u
#define STA_REC_WTBL 13u
#define WTBL_GENERIC 0u
#define WTBL_RX 1u
#define WTBL_HDR_TRANS 6u
#define WTBL_RESET_AND_SET 1u
#define CONNECTION_INFRA_STA 0x00010001u /* STA_TYPE_STA | NETWORK_INFRA: a station of our AP-type BSS */
#define CONN_STATE_DISCONNECT 0u
#define CONN_STATE_PORT_SECURE 2u
#define EXTRA_INFO_VER 0x1u
#define EXTRA_INFO_NEW 0x2u

#define STA_HDR_LEN 8u      /* struct sta_req_hdr */
#define BASIC_LEN 20u       /* struct sta_rec_basic */
#define WTBL_HDR_LEN 8u     /* struct wtbl_req_hdr */
#define GENERIC_LEN 20u     /* struct wtbl_generic */
#define RX_LEN 12u          /* struct wtbl_rx */
#define HDR_TRANS_LEN 8u    /* struct wtbl_hdr_trans */

uint32_t mt_sta_rec_build(uint8_t *req, uint32_t cap, uint8_t wcid, const uint8_t addr[6], int enable)
{
    uint32_t wtbl_len = 4u + WTBL_HDR_LEN + (enable ? GENERIC_LEN + RX_LEN + HDR_TRANS_LEN : 0u);
    uint32_t total = STA_HDR_LEN + BASIC_LEN + wtbl_len;
    uint8_t *p;
    if (cap < total) return 0;
    mt_memset(req, 0, total);

    /* struct sta_req_hdr: bss_idx 0 (our vif), wlan_idx_lo, tlv_num 2, is_tlv_append 1, muar_idx = omac 0
     * (a real station, wcid->sta = 1), wlan_idx_hi 0 */
    req[0] = 0;
    req[1] = wcid;
    mt_put_le16(req + 2, 2);
    req[4] = 1;
    req[5] = 0;
    p = req + STA_HDR_LEN;

    /* STA_REC_BASIC (mt76_connac_mcu_sta_basic_tlv, AP-type vif) */
    mt_put_le16(p, STA_REC_BASIC);
    mt_put_le16(p + 2, BASIC_LEN);
    mt_put_le32(p + 4, CONNECTION_INFRA_STA);
    p[8] = enable ? CONN_STATE_PORT_SECURE : CONN_STATE_DISCONNECT;
    p[9] = 0;                                   /* qos: AWDL data frames are plain data, no QoS header */
    mt_put_le16(p + 10, 1);                     /* aid: no association, any non-zero id */
    mt_memcpy(p + 12, addr, 6);
    mt_put_le16(p + 18, (uint16_t)(EXTRA_INFO_VER | (enable ? EXTRA_INFO_NEW : 0u)));
    p += BASIC_LEN;

    /* STA_REC_WTBL with a wtbl_req_hdr and the nested WTBL TLVs (mt76_connac_mcu_add_nested_tlv adds each nested
     * length to this TLV's len and counts it in the wtbl_req_hdr) */
    mt_put_le16(p, STA_REC_WTBL);
    mt_put_le16(p + 2, (uint16_t)wtbl_len);
    p[4] = wcid;                                /* wtbl_req_hdr.wlan_idx_lo */
    p[5] = WTBL_RESET_AND_SET;                  /* operation */
    mt_put_le16(p + 6, (uint16_t)(enable ? 3u : 0u));
    p[8] = 0;                                   /* wlan_idx_hi */
    p += 4u + WTBL_HDR_LEN;
    if (!enable) return total;

    /* WTBL_GENERIC: peer address, muar_idx = omac 0, partial_aid = aid, qos 0 */
    mt_put_le16(p, WTBL_GENERIC);
    mt_put_le16(p + 2, GENERIC_LEN);
    mt_memcpy(p + 4, addr, 6);
    p[10] = 0;                                  /* muar_idx */
    mt_put_le16(p + 16, 1);                     /* partial_aid */
    p += GENERIC_LEN;

    /* WTBL_RX: rca1 = (vif type != AP) = 0 for an AP-type vif, rca2 1, rv 1 */
    mt_put_le16(p, WTBL_RX);
    mt_put_le16(p + 2, RX_LEN);
    p[4] = 0;                                   /* rcid */
    p[5] = 0;                                   /* rca1 */
    p[6] = 1;                                   /* rca2 */
    p[7] = 1;                                   /* rv */
    p += RX_LEN;

    /* WTBL_HDR_TRANS: not a station vif -> from_ds; no header translation for this peer */
    mt_put_le16(p, WTBL_HDR_TRANS);
    mt_put_le16(p + 2, HDR_TRANS_LEN);
    p[4] = 0;                                   /* to_ds */
    p[5] = 1;                                   /* from_ds */
    p[6] = 1;                                   /* no_rx_trans */
    return total;
}

int mt792x_sta_add(struct mt792x *dev, const uint8_t addr[6], uint8_t *wcid)
{
    uint8_t req[128];
    uint32_t len, i, k = 0;
    int err, status = -1;

    if (wcid) *wcid = 0;
    if (!dev->sniff_started) return MT792X_E_TX_DENIED;
    if (addr[0] & 1u) return MT792X_E_TX_FRAME;                 /* a group address cannot be a station */
    for (i = MT792X_STA_FIRST; i <= MT792X_STA_LAST; i++) {
        if (dev->sta_used[i] && same_addr(dev->sta_addr[i], addr)) {
            if (wcid) *wcid = (uint8_t)i;                       /* already known */
            return 0;
        }
        if (!dev->sta_used[i] && !k) k = i;
    }
    if (!k) return MT792X_E_STA;

    err = mt_wtbl_update(dev, k);                               /* mt7921_mac_wtbl_update(idx, ADM_COUNT_CLEAR) */
    if (err) return err;
    len = mt_sta_rec_build(req, sizeof(req), (uint8_t)k, addr, 1);
    err = mt_mcu_send(dev, "STA_REC_UPDATE add", MT_MCU_UNI(MCU_UNI_STA_REC_UPDATE), req, len, 1, MT_MCU_TIMEOUT_US, &status);
    if (err) return err;
    if (status != 0) {
        mt_logf(dev, MT_LOG_ERROR, "STA_REC_UPDATE add wcid %u: status 0x%08x", k, (uint32_t)status);
        return MT792X_E_MCU_STATUS;
    }
    dev->sta_used[k] = 1;
    mt_memcpy(dev->sta_addr[k], addr, 6);
    mt_logf(dev, MT_LOG_INFO, "station %02x:%02x:%02x:xx:xx:xx added as wcid %u", addr[0], addr[1], addr[2], k);
    if (wcid) *wcid = (uint8_t)k;
    return 0;
}

int mt792x_sta_remove(struct mt792x *dev, uint8_t wcid)
{
    uint8_t req[64];
    uint32_t len;
    int err, status = -1;

    if (wcid < MT792X_STA_FIRST || wcid > MT792X_STA_LAST || !dev->sta_used[wcid]) return MT792X_E_STA;
    for (len = 0; len < MT792X_TX0_SLOTS; len++)
        if (dev->slot[len].state == SLOT_BUSY && dev->slot[len].wcid == wcid) return MT792X_E_TX_BUSY;
    len = mt_sta_rec_build(req, sizeof(req), wcid, dev->sta_addr[wcid], 0);
    err = mt_mcu_send(dev, "STA_REC_UPDATE remove", MT_MCU_UNI(MCU_UNI_STA_REC_UPDATE), req, len, 1, MT_MCU_TIMEOUT_US,
                      &status);
    dev->sta_used[wcid] = 0;                                    /* forget it whatever the firmware says */
    if (err) return err;
    if (status != 0) {
        mt_logf(dev, MT_LOG_WARN, "STA_REC_UPDATE remove wcid %u: status 0x%08x", wcid, (uint32_t)status);
        return MT792X_E_MCU_STATUS;
    }
    return 0;
}

int mt_sta_lookup(const struct mt792x *dev, uint8_t wcid, const uint8_t *addr1)
{
    if (wcid < MT792X_STA_FIRST || wcid > MT792X_STA_LAST || !dev->sta_used[wcid]) return 0;
    return same_addr(dev->sta_addr[wcid], addr1);
}
