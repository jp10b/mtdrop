/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * postboot.c - what mt76 does after FW_START (steps S24-S31), the monitor interface / sniffer
 * (SN1-SN13), channel switching, and the best-effort MCU part of the shutdown.
 *
 * Derived from Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear, Copyright (C) 2020-2023
 * MediaTek Inc.): mt7921/mcu.c (mt7921_run_firmware 682-704, get_nic_capability 597-667,
 * fw_log_2_host 669-680, radio_led_ctrl 707-718, set_chan_info 912-970, set_eeprom 972-985,
 * set_bss_pm 1061-1097, set_beacon_filter 1123-1156, set_sniffer 1185-1213, config_sniffer
 * 1215-1281, wf_rf_pin_ctrl 1506-1519, set_thermal_protect 1524-1532, set_rxfilter 1534-1554),
 * mt7921/init.c (mac_init 64-81, __mt7921_init_hardware 84-107, init_work 131-168),
 * mt7921/main.c (__mt7921_start 227-270, add_interface 300-360, set_channel 477-499,
 * sniffer_interface_iter 602-618, configure_filter 666-691, change_chanctx 1411-1436),
 * mt7921/mac.c:19-26 (wtbl_update), mt792x_mac.c:289-316 (mac_init_band), mt792x_core.c:232-252
 * (mt792x_stop), mt76_connac_mcu.c (set_channel_domain 123-213, set_mac_enable 216-230,
 * set_rts_thresh 255-272, uni_add_dev 1194-1302, chip_config 2047-2057, set_deep_sleep
 * 2060-2067). Spec: docs/MT7921-BRINGUP.md section 11.
 */
#include "internal.h"
#include "regs.h"

#define WTBL_POLL_US 5000u   /* mt76_poll(..., 5000) */
#define WTBL_TICK_US 10u     /* mt76_poll: udelay(10) */

/* mt76 2.4 / 5 GHz channel tables (mac80211.c:30-82) */
static const uint8_t chan_2g[14] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static const uint8_t chan_5g[28] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120,
                                     124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177 };

static int chan_valid(uint8_t ch, uint8_t band)
{
    unsigned i;
    if (band == MT792X_BAND_2GHZ) return ch >= 1 && ch <= 14;
    if (band != MT792X_BAND_5GHZ) return 0;
    for (i = 0; i < sizeof(chan_5g); i++)
        if (chan_5g[i] == ch) return 1;
    return 0;
}

static uint8_t popcount8(uint8_t v)
{
    uint8_t n = 0;
    while (v) {
        n = (uint8_t)(n + (v & 1u));
        v >>= 1;
    }
    return n;
}

static uint8_t ant_mask(const struct mt792x *dev)
{
    return dev->opt.antenna_mask ? dev->opt.antenna_mask : dev->nic.antenna_mask;
}

/* ---- GET_NIC_CAPAB ---- */

int mt792x_nic_caps_parse(const uint8_t *p, uint32_t len, struct mt792x_nic_caps *out)
{
    uint32_t off, i;

    if (!p || !out) return MT792X_E_INVAL;
    mt_memset(out, 0, sizeof(*out));
    /* struct mt76_connac_cap_hdr {le16 n_element; u8 rsv[2];} */
    if (len < 4u) return MT792X_E_NIC_CAPAB;
    out->valid = 1;
    out->n_element = mt_get_le16(p);
    off = 4u;
    for (i = 0; i < out->n_element; i++) {
        uint32_t type, tlen;
        const uint8_t *d;
        if (len - off < 8u) {          /* skb->len < sizeof(*tlv): break */
            out->truncated = 1;
            break;
        }
        type = mt_get_le32(p + off);
        tlen = mt_get_le32(p + off + 4u);
        off += 8u;
        if (tlen > len - off) {        /* skb->len < len: break */
            out->truncated = 1;
            break;
        }
        d = p + off;
        if (type < 32u) out->types_seen |= 1u << type;
        switch (type) {
        case MT_NIC_CAP_6G:
            if (tlen) out->has_6ghz = d[0] ? 1u : 0u;
            break;
        case MT_NIC_CAP_MAC_ADDR:
            if (tlen >= 6u) {
                mt_memcpy(out->mac, d, 6);
                out->has_mac = 1;
            }
            break;
        case MT_NIC_CAP_PHY:
            /* struct mt7921_phy_cap {ht, vht, _5g, max_bw, nss, dbdc, tx_ldpc, rx_ldpc, tx_stbc,
             * rx_stbc, hw_path, he}; used only if len >= offsetofend(hw_path) = 11 */
            if (tlen >= 11u) {
                out->has_phy = 1;
                out->phy_max_bw = d[3];
                out->phy_nss = d[4];
                out->phy_hw_path = d[10];
                out->phy_he = tlen >= 12u ? d[11] : 0u;
                out->antenna_mask = (uint8_t)(d[4] < 8u ? (1u << d[4]) - 1u : 0xffu); /* BIT(nss) - 1 */
                out->has_2ghz = (d[10] & 0x1u) ? 1u : 0u;                           /* BIT(WF0_24G) */
                out->has_5ghz = (d[10] & 0x2u) ? 1u : 0u;                           /* BIT(WF0_5G) */
            }
            break;
        case MT_NIC_CAP_CHIP_CAP:
            if (tlen >= 8u) {
                out->chip_cap = (uint64_t)mt_get_le32(d) | ((uint64_t)mt_get_le32(d + 4) << 32);
                out->has_chip_cap = 1;
            }
            break;
        case MT_NIC_CAP_HW_VER:
            out->hw_ver_len = (uint8_t)(tlen < sizeof(out->hw_ver) ? tlen : sizeof(out->hw_ver));
            mt_memcpy(out->hw_ver, d, out->hw_ver_len);
            break;
        case MT_NIC_CAP_SW_VER:
            out->sw_ver_len = (uint8_t)(tlen < sizeof(out->sw_ver) ? tlen : sizeof(out->sw_ver));
            mt_memcpy(out->sw_ver, d, out->sw_ver_len);
            break;
        default:
            break;
        }
        off += tlen;
        out->n_parsed++;
    }
    return 0;
}

static void hex16(char *dst, const uint8_t *b, uint32_t n)
{
    static const char hx[] = "0123456789abcdef";
    uint32_t i;
    for (i = 0; i < n && i < 16u; i++) {
        dst[2u * i] = hx[b[i] >> 4];
        dst[2u * i + 1u] = hx[b[i] & 0xfu];
    }
    dst[2u * i] = '\0';
}

/* ---- helpers for the individual commands ---- */

static int chip_config(struct mt792x *dev, const char *name, const char *text)
{
    uint8_t req[MT_CONNAC_CONFIG_LEN];
    uint32_t i;
    mt_memset(req, 0, sizeof(req));
    /* id 0, type 0, resp_type 0, data_size 0, resv 0; strscpy(data, text) */
    for (i = 0; text[i] && i + 1u < MT_CONNAC_CONFIG_LEN - MT_CONNAC_CONFIG_DATA_OFF; i++)
        req[MT_CONNAC_CONFIG_DATA_OFF + i] = (uint8_t)text[i];
    return mt_mcu_send(dev, name, MT_MCU_CE(MCU_CE_CHIP_CONFIG), req, sizeof(req), 0, MT_MCU_TIMEOUT_US, NULL);
}

static int mac_enable(struct mt792x *dev, int enable, uint32_t timeout_us)
{
    uint8_t req[4] = { 0, 0, 0, 0 };   /* {u8 enable; u8 band; u8 rsv[2]} */
    req[0] = enable ? 1u : 0u;
    return mt_mcu_send(dev, enable ? "MAC_INIT_CTRL enable" : "MAC_INIT_CTRL disable", MT_MCU_EXT(MCU_EXT_MAC_INIT_CTRL),
                       req, sizeof(req), 1, timeout_us, NULL);
}

static int radio_ctrl(struct mt792x *dev, uint8_t ctrlid, const char *name)
{
    uint8_t req[4] = { 0, 0, 0, 0 };   /* {u8 ctrlid; u8 rsv[3]} */
    req[0] = ctrlid;
    return mt_mcu_send(dev, name, MT_MCU_EXT(MCU_EXT_ID_RADIO_ON_OFF_CTRL), req, sizeof(req), 0, MT_MCU_TIMEOUT_US, NULL);
}

/* mt7921_mcu_set_chan_info, 20 MHz only: center_ch = control_ch, bw = CMD_CBW_20MHZ (0). */
static int chan_info(struct mt792x *dev, uint32_t cmd, const char *name, uint8_t ch, uint8_t band)
{
    uint8_t req[76];
    uint8_t mask = ant_mask(dev);
    mt_memset(req, 0, sizeof(req));
    req[0] = ch;                                   /* control_ch */
    req[1] = ch;                                   /* center_ch */
    req[2] = 0;                                    /* bw: CMD_CBW_20MHZ = IEEE80211_STA_RX_BW_20 = 0 */
    req[3] = popcount8(mask);                      /* tx_streams_num */
    req[4] = cmd == MT_MCU_EXT(MCU_EXT_CHANNEL_SWITCH) ? popcount8(mask) : mask; /* rx_streams */
    req[5] = CH_SWITCH_NORMAL;                     /* SET_RX_PATH, or monitor and not off-channel */
    req[6] = 0;                                    /* band_idx */
    req[10] = band == MT792X_BAND_5GHZ ? 1u : 0u;  /* channel_band = nl80211 band */
    return mt_mcu_send(dev, name, cmd, req, sizeof(req), 1, MT_MCU_TIMEOUT_US, NULL);
}

static int sniffer_enable(struct mt792x *dev, int enable, uint32_t timeout_us)
{
    uint8_t req[12];
    mt_memset(req, 0, sizeof(req));
    req[0] = 0;                                    /* band_idx */
    mt_put_le16(req + 4, 0);                       /* tag 0 */
    mt_put_le16(req + 6, 8);                       /* len = sizeof(struct sniffer_enable_tlv) */
    req[8] = enable ? 1u : 0u;
    return mt_mcu_send(dev, enable ? "SNIFFER enable" : "SNIFFER disable", MT_MCU_UNI(MCU_UNI_SNIFFER), req, sizeof(req), 1,
                       timeout_us, NULL);
}

static int sniffer_config(struct mt792x *dev, uint8_t ch, uint8_t band)
{
    uint8_t req[20];
    mt_memset(req, 0, sizeof(req));
    req[0] = 0;                                    /* band_idx */
    mt_put_le16(req + 4, 1);                       /* tag 1 */
    mt_put_le16(req + 6, 16);                      /* len = sizeof(req.tlv) */
    mt_put_le16(req + 8, 0);                       /* aid */
    req[10] = band == MT792X_BAND_5GHZ ? 2u : 1u;  /* ch_band: 1 = 2.4 GHz, 2 = 5 GHz */
    req[11] = 0;                                   /* bw: NL80211_CHAN_WIDTH_20 -> 0 */
    req[12] = ch;                                  /* control_ch */
    req[13] = 0;                                   /* sco: control == center */
    req[14] = ch;                                  /* center_ch */
    req[15] = 0;                                   /* center_ch2 */
    req[16] = (dev->sniff_flags & MT792X_SNIFF_FCSFAIL) ? 0u : 1u; /* drop_err = !(rxfilter & FCSFAIL) */
    return mt_mcu_send(dev, "SNIFFER config", MT_MCU_UNI(MCU_UNI_SNIFFER), req, sizeof(req), 1, MT_MCU_TIMEOUT_US, NULL);
}

static int rx_filter(struct mt792x *dev, const char *name, uint32_t fif, uint8_t bit_op, uint32_t bit_map)
{
    uint8_t req[68];
    mt_memset(req, 0, sizeof(req));
    req[4] = fif ? 1u : 2u;                        /* mode */
    mt_put_le32(req + 8, fif);
    mt_put_le32(req + 12, bit_map);
    req[16] = bit_op;
    return mt_mcu_send(dev, name, MT_MCU_CE(MCU_CE_SET_RX_FILTER), req, sizeof(req), 0, MT_MCU_TIMEOUT_US, NULL);
}

/* mt76_connac_mcu_uni_add_dev for the monitor vif: idx = omac_idx = 0, band 0, wmm 0, link 0.
 * The wcid passed by mt7921_add_interface still has idx 0 at this point (it is set to
 * MT792x_WTBL_RESERVED - idx only afterwards, mt7921/main.c:323-335). */
static int dev_info(struct mt792x *dev, int active, uint32_t timeout_us, int *status)
{
    uint8_t req[16];
    mt_memset(req, 0, sizeof(req));
    req[0] = 0;                                    /* omac_idx */
    req[1] = 0;                                    /* band_idx */
    mt_put_le16(req + 4, DEV_INFO_ACTIVE);
    mt_put_le16(req + 6, 12);                      /* sizeof(struct req_tlv) */
    req[8] = active ? 1u : 0u;
    req[9] = 0;                                    /* link_idx */
    mt_memcpy(req + 10, dev->own_mac, 6);
    return mt_mcu_send(dev, active ? "DEV_INFO_UPDATE active" : "DEV_INFO_UPDATE inactive",
                       MT_MCU_UNI(MCU_UNI_DEV_INFO_UPDATE), req, sizeof(req), 1, timeout_us, status);
}

static int bss_info(struct mt792x *dev, int active, uint32_t timeout_us, int *status)
{
    uint8_t req[36];
    mt_memset(req, 0, sizeof(req));
    req[0] = 0;                                    /* bss_idx */
    mt_put_le16(req + 4, UNI_BSS_INFO_BASIC);      /* struct mt76_connac_bss_basic_tlv (32 bytes) */
    mt_put_le16(req + 6, 32);
    req[8] = active ? 1u : 0u;                     /* active */
    req[9] = 0;                                    /* omac_idx */
    req[10] = HW_BSSID_0;                          /* hw_bss_idx: omac_idx (0) <= EXT_BSSID_START */
    req[11] = 0;                                   /* band_idx */
    mt_put_le32(req + 12, CONNECTION_INFRA_AP);    /* conn_type for NL80211_IFTYPE_MONITOR */
    req[16] = 1;                                   /* conn_state */
    req[17] = 0;                                   /* wmm_idx */
    mt_put_le16(req + 24, 0);                      /* bmc_tx_wlan_idx = wcid->idx (0, see dev_info) */
    mt_put_le16(req + 30, 0);                      /* sta_idx */
    return mt_mcu_send(dev, active ? "BSS_INFO_UPDATE active" : "BSS_INFO_UPDATE inactive",
                       MT_MCU_UNI(MCU_UNI_BSS_INFO_UPDATE), req, sizeof(req), 1, timeout_us, status);
}

/* mt7921_mac_wtbl_update: rmw(WTBL_UPDATE, WLAN_IDX, idx | mask); poll BUSY == 0, 5 ms. The
 * poll result is ignored by mt7921_mac_init; logged here. */
static int wtbl_update(struct mt792x *dev, uint32_t idx)
{
    uint32_t v = 0;
    int err = mt_rmw(dev, MT_WTBL_UPDATE, MT_WTBL_UPDATE_WLAN_IDX, idx | MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
    if (err) return err;
    err = mt_poll(dev, MT_WTBL_UPDATE, MT_WTBL_UPDATE_BUSY, 0, WTBL_POLL_US, WTBL_TICK_US, &v, NULL);
    if (err == MT_POLL_TIMEOUT) {
        mt_logf(dev, MT_LOG_WARN, "WTBL update %u: BUSY still set (0x%08x) after 5 ms (ignored, as mt76)", idx, v);
        return 0;
    }
    return err;
}

static int mac_init_band(struct mt792x *dev, uint32_t b)
{
    int err = mt_rmw(dev, MT_TMAC_CTCR0(b), MT_TMAC_CTCR0_INS_DDLMT_REFTIME, 0x3fu);
    if (!err) err = mt_rmw(dev, MT_TMAC_CTCR0(b), 0, MT_TMAC_CTCR0_INS_DDLMT_VHT_SMPDU_EN | MT_TMAC_CTCR0_INS_DDLMT_EN);
    if (!err) err = mt_rmw(dev, MT_WF_RMAC_MIB_TIME0(b), 0, MT_WF_RMAC_MIB_RXTIME_EN);
    if (!err) err = mt_rmw(dev, MT_WF_RMAC_MIB_AIRTIME0(b), 0, MT_WF_RMAC_MIB_RXTIME_EN);
    if (!err) err = mt_rmw(dev, MT_MIB_SCR1(b), 0, MT_MIB_TXDUR_EN);
    if (!err) err = mt_rmw(dev, MT_MIB_SCR1(b), 0, MT_MIB_RXDUR_EN);
    if (!err) err = mt_rmw(dev, MT_DMA_DCR0(b), MT_MAX_RX_LEN_MASK, MT_MAX_RX_LEN_1536);
    if (!err) err = mt_rmw(dev, MT_DMA_DCR0(b), MT_DMA_DCR0_RXD_G5_EN, 0);
    if (!err) err = mt_rmw(dev, MT_WTBLOFF_TOP_RSCR(b), MT_WTBLOFF_TOP_RSCR_RCPI_MASK, MT_WTBLOFF_TOP_RSCR_RCPI_SET);
    return err;
}

static int mac_init_regs(struct mt792x *dev)
{
    uint32_t i, dcr0;
    int err = mt_rmw(dev, MT_MDP_DCR1, MT_MAX_RX_LEN_MASK, MT_MAX_RX_LEN_1536);
    if (!err) err = mt_rmw(dev, MT_MDP_DCR0, 0, MT_MDP_DCR0_DAMSDU_EN);
    if (!err && !dev->opt.no_rx_hdr_trans) err = mt_rmw(dev, MT_MDP_DCR0, 0, MT_MDP_DCR0_RX_HDR_TRANS_EN);
    for (i = 0; !err && i < MT792X_WTBL_SIZE; i++) err = wtbl_update(dev, i);
    for (i = 0; !err && i < 2u; i++) err = mac_init_band(dev, i);
    if (err) return err;
    dcr0 = mt_rr(dev, MT_MDP_DCR0);
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    mt_logf(dev, MT_LOG_INFO, "S28 MAC init: MDP_DCR0 0x%08x (RX header translation %s), MDP_DCR1 0x%08x, DMA_DCR0 0x%08x",
            dcr0, (dcr0 & MT_MDP_DCR0_RX_HDR_TRANS_EN) ? "ON" : "off", mt_rr(dev, MT_MDP_DCR1), mt_rr(dev, MT_DMA_DCR0(0)));
    return dev->bus_dead ? MT792X_E_BUS_DEAD : 0;
}

/* ---- S24-S31 ---- */

int mt_postboot(struct mt792x *dev)
{
    static const uint8_t fwlog_on[4] = { 1, 0, 0, 0 };         /* {u8 ctrl_val; u8 pad[3]} */
    static const uint8_t efuse_mode[4] = { EE_MODE_EFUSE, EE_FORMAT_WHOLE, 0, 0 }; /* {buffer_mode; format; le16 len} */
    uint8_t rts[12];
    char text[48];
    uint64_t t0 = mt_now(dev);
    int err;

    /* S24 */
    err = mt_mcu_send(dev, "GET_NIC_CAPAB", MT_MCU_CE(MCU_CE_GET_NIC_CAPAB), NULL, 0, 1, MT_MCU_TIMEOUT_US, NULL);
    if (err) return mt_step_fail(dev, 24, err, t0);
    err = mt792x_nic_caps_parse(dev->evt + MT_MCU_RXD_LEN, dev->evt_len - MT_MCU_RXD_LEN, &dev->nic);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "S24 GET_NIC_CAPAB reply of %u bytes has no capability header", dev->evt_len);
        return mt_step_fail(dev, 24, err, t0);
    }
    {
        char hw[33], sw[33];
        hex16(hw, dev->nic.hw_ver, dev->nic.hw_ver_len);
        hex16(sw, dev->nic.sw_ver, dev->nic.sw_ver_len);
        mt_logf(dev, MT_LOG_INFO,
                "S24 NIC capability: %u/%u TLVs%s, types 0x%08x; MAC %s %02x:%02x:%02x:xx:xx:xx; chip_cap %s 0x%08x%08x",
                dev->nic.n_parsed, dev->nic.n_element, dev->nic.truncated ? " (truncated)" : "", dev->nic.types_seen,
                dev->nic.has_mac ? "yes" : "NO", dev->nic.mac[0], dev->nic.mac[1], dev->nic.mac[2],
                dev->nic.has_chip_cap ? "yes" : "NO", (uint32_t)(dev->nic.chip_cap >> 32), (uint32_t)dev->nic.chip_cap);
        mt_logf(dev, MT_LOG_INFO,
                "S24 PHY %s: nss %u hw_path 0x%x (2.4 GHz %u, 5 GHz %u, 6 GHz %u) max_bw %u he %u -> antenna mask 0x%x",
                dev->nic.has_phy ? "yes" : "NO", dev->nic.phy_nss, dev->nic.phy_hw_path, dev->nic.has_2ghz,
                dev->nic.has_5ghz, dev->nic.has_6ghz, dev->nic.phy_max_bw, dev->nic.phy_he, dev->nic.antenna_mask);
        mt_logf(dev, MT_LOG_INFO, "S24 HW_VER %s; SW_VER %s (raw, format not parsed by mt76)", hw, sw);
    }
    mt_step_done(dev, 24);
    /* CLC (mt7921_load_clc: EFUSE_ACCESS query + CLC tables from the RAM file, later SET_CLC) is
     * regulatory TX power / channel policy and is not sent (spec 11.3). */

    /* S25 */
    t0 = mt_now(dev);
    if (!dev->opt.skip_thermal_protect) {
        mt_format(text, sizeof(text), "DowngradeTxStreamTemp %d %d", MT7921_THERMAL_PROT_LOW_TEMP,
                  MT7921_THERMAL_PROT_HIGH_TEMP);
        err = chip_config(dev, "CHIP_CONFIG thermal protect", text);
        if (err) return mt_step_fail(dev, 25, err, t0);
    }
    mt_step_done(dev, 25);

    /* S26 */
    t0 = mt_now(dev);
    if (!dev->opt.skip_fw_log) {
        err = mt_mcu_send(dev, "FWLOG_2_HOST 1", MT_MCU_CE(MCU_CE_FWLOG_2_HOST), fwlog_on, sizeof(fwlog_on), 0,
                          MT_MCU_TIMEOUT_US, NULL);
        if (err) return mt_step_fail(dev, 26, err, t0);
    }
    mt_step_done(dev, 26);

    /* S27 */
    t0 = mt_now(dev);
    err = mt_mcu_send(dev, "EFUSE_BUFFER_MODE", MT_MCU_EXT(MCU_EXT_EFUSE_BUFFER_MODE), efuse_mode, sizeof(efuse_mode), 1,
                      MT_MCU_TIMEOUT_US, NULL);
    if (err) return mt_step_fail(dev, 27, err, t0);
    mt_step_done(dev, 27);

    /* S28 */
    t0 = mt_now(dev);
    err = mac_init_regs(dev);
    if (err) return mt_step_fail(dev, 28, err, t0);
    mt_step_done(dev, 28);

    /* S29: mt76_connac_mcu_set_rts_thresh(0x92b, band 0) {prot_idx 1; band; rsv[2]; le32 len; le32 pkt} */
    t0 = mt_now(dev);
    mt_memset(rts, 0, sizeof(rts));
    rts[0] = 1;
    mt_put_le32(rts + 4, MT_RTS_THRESH_DEFAULT);
    mt_put_le32(rts + 8, MT_RTS_PKT_THRESH);
    err = mt_mcu_send(dev, "PROTECT_CTRL rts", MT_MCU_EXT(MCU_EXT_PROTECT_CTRL), rts, sizeof(rts), 1, MT_MCU_TIMEOUT_US, NULL);
    if (err) return mt_step_fail(dev, 29, err, t0);
    mt_step_done(dev, 29);

    /* S30: [HOST] mt76 sends "KeepFullPwr 0" here (deep sleep enabled by default); we never want
     * the chip to sleep (project finding), which is what mt76 sends for a monitor interface. */
    t0 = mt_now(dev);
    err = chip_config(dev, "CHIP_CONFIG KeepFullPwr 1", "KeepFullPwr 1");
    if (err) return mt_step_fail(dev, 30, err, t0);
    mt_step_done(dev, 30);

    /* S31: first command of __mt7921_start */
    t0 = mt_now(dev);
    dev->mac_enabled = 1;        /* set before sending: shutdown undoes whatever may have taken effect */
    err = mac_enable(dev, 1, MT_MCU_TIMEOUT_US);
    if (err) return mt_step_fail(dev, 31, err, t0);
    dev->postboot_done = 1;
    dev->rx_active = 1;          /* from now on data ring 2 is drained during command waits */
    dev->next_liveness_us = 0;
    mt_step_done(dev, 31);
    return 0;
}

/* ---- sniffer ---- */

static int sn_fail(struct mt792x *dev, int step, int err)
{
    dev->sniff_failed_step = step;
    mt_logf(dev, MT_LOG_ERROR, "sniffer SN%d FAILED: %s (%d)", step, mt792x_strerror(err), err);
    return err;
}

static const char *tune_name(int t)
{
    switch (t) {
    case MT792X_TUNE_CHANNEL_SWITCH: return "CHANNEL_SWITCH";
    case MT792X_TUNE_SNIFFER_CONFIG: return "sniffer config";
    case MT792X_TUNE_BOTH: return "CHANNEL_SWITCH + sniffer config";
    default: return "?";
    }
}

/* mt792x_mac_set_timeing for band 0, coverage class 0, slot time 9 (mt76 defaults): pause TX/RX
 * in the arbiter, program CCK/OFDM timeouts, IFS and the CF-End rate, release TX/RX. The value of
 * MT_ARB_SCR before the update is logged: an RX_DISABLE bit left set by the firmware would block
 * reception until this runs (TODO-VERIFY on hardware). */
static int set_timing(struct mt792x *dev, uint8_t band)
{
    uint32_t sifs = band == MT792X_BAND_2GHZ ? 10u : 16u, scr;
    uint32_t cfend = (MT792X_SLOTTIME_DEFAULT < 20u || band != MT792X_BAND_2GHZ) ? MT792X_CFEND_RATE_DEFAULT
                                                                                    : MT792X_CFEND_RATE_11B;
    int err;
    scr = mt_rr(dev, MT_ARB_SCR0);
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    err = mt_rmw(dev, MT_ARB_SCR0, 0, MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
    if (err) return err;
    mt_sleep(dev, 1);
    mt_wr(dev, MT_TMAC_CDTR0, MT_TIMING_CCK);
    mt_wr(dev, MT_TMAC_ODTR0, MT_TIMING_OFDM);
    mt_wr(dev, MT_TMAC_ICR0_0, MT_IFS(360u, 2u, sifs, MT792X_SLOTTIME_DEFAULT));
    err = mt_rmw(dev, MT_AGG_ACR0_0, MT_AGG_ACR_CFEND_RATE, cfend);
    if (!err) err = mt_rmw(dev, MT_ARB_SCR0, MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE, 0);
    if (err) return err;
    mt_logf(dev, MT_LOG_DEBUG, "MAC timing set (ARB_SCR was 0x%08x%s)", scr,
            (scr & MT_ARB_SCR_RX_DISABLE) ? ", RX_DISABLE was set" : "");
    if (scr & MT_ARB_SCR_RX_DISABLE)
        mt_logf(dev, MT_LOG_INFO, "MAC timing: ARB_SCR 0x%08x had RX_DISABLE set before the update", scr);
    return dev->bus_dead ? MT792X_E_BUS_DEAD : 0;
}

/* *t_resp = time at which the last awaited response arrived. */
static int tune(struct mt792x *dev, uint8_t ch, uint8_t band, uint64_t *t_resp)
{
    int err = 0;
    if (dev->tune_used == MT792X_TUNE_CHANNEL_SWITCH || dev->tune_used == MT792X_TUNE_BOTH) {
        err = chan_info(dev, MT_MCU_EXT(MCU_EXT_CHANNEL_SWITCH), "CHANNEL_SWITCH", ch, band);
        *t_resp = mt_now(dev);
        if (!err && !dev->opt.skip_set_timing) err = set_timing(dev, band); /* mt7921_set_channel */
    }
    if (!err && (dev->tune_used == MT792X_TUNE_SNIFFER_CONFIG || dev->tune_used == MT792X_TUNE_BOTH)) {
        err = sniffer_enable(dev, 1, MT_MCU_TIMEOUT_US); /* mt7921_change_chanctx sends both */
        if (!err) err = sniffer_config(dev, ch, band);
        *t_resp = mt_now(dev);
    }
    if (!err) {
        dev->cur_channel = ch;
        dev->cur_band = band;
    }
    return err;
}

static int mac_valid(const uint8_t *m)
{
    return !(m[0] & 1u) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

int mt792x_sniffer_start(struct mt792x *dev, const struct mt792x_sniffer_cfg *cfg)
{
    uint8_t hdr[12], dom[12 + 8 * 42], abort_req[4] = { 0, 0, 0, 0 };
    uint32_t n = 0, i, fif = MT7921_FILTER_ENABLE;
    int err, status = 0;

    if (!dev || !cfg) return MT792X_E_INVAL;
    if (dev->state != MT_ST_TOUCHED || !dev->postboot_done || dev->sniff_started || dev->sniffer_on || dev->dev_active ||
        dev->bss_active)
        return MT792X_E_STATE;
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    if (dev->opt.tune_mode > MT792X_TUNE_BOTH) return MT792X_E_INVAL;
    if (cfg->bw != 0 || !chan_valid(cfg->channel, cfg->band)) {
        mt_logf(dev, MT_LOG_ERROR, "sniffer: channel %u band %u bw %u not supported", cfg->channel, cfg->band, cfg->bw);
        return MT792X_E_INVAL;
    }
    if (cfg->mac) {
        mt_memcpy(dev->own_mac, cfg->mac, 6);
    } else if (dev->nic.has_mac && mac_valid(dev->nic.mac)) {
        mt_memcpy(dev->own_mac, dev->nic.mac, 6);
    } else {
        mt_logf(dev, MT_LOG_ERROR, "sniffer: no MAC address given and none valid from GET_NIC_CAPAB");
        return MT792X_E_NO_MAC;
    }
    if (!ant_mask(dev)) mt_logf(dev, MT_LOG_WARN, "sniffer: antenna mask 0 (no PHY capability): streams fields will be 0");
    dev->sniff_flags = cfg->flags;
    dev->sniff_failed_step = -1;
    if (dev->opt.tune_mode != MT792X_TUNE_AUTO) {
        dev->tune_used = dev->opt.tune_mode;
    } else {
        dev->tune_used = (dev->ram.fw_features & MT792X_FW_CAP_CNM) ? MT792X_TUNE_SNIFFER_CONFIG : MT792X_TUNE_CHANNEL_SWITCH;
    }
    mt_logf(dev, MT_LOG_INFO, "sniffer start: channel %u (%s), flags 0x%x, MAC %02x:%02x:%02x:xx:xx:xx, firmware features 0x%02x (status %d) -> tuning by %s%s",
            cfg->channel, cfg->band == MT792X_BAND_5GHZ ? "5 GHz" : "2.4 GHz", cfg->flags, dev->own_mac[0], dev->own_mac[1],
            dev->own_mac[2], dev->ram.fw_features, dev->ram.fw_features_status, tune_name(dev->tune_used),
            dev->opt.tune_mode ? " (forced)" : "");

    /* SN1: mt76_connac_mcu_set_channel_domain. [HOST] flags 0 and alpha2 "00" (see spec 11.5). */
    if (!dev->opt.skip_chan_domain) {
        mt_memset(dom, 0, sizeof(dom));
        if (dev->nic.has_2ghz)
            for (i = 0; i < sizeof(chan_2g); i++, n++) mt_put_le16(dom + 12 + 8 * n, chan_2g[i]);
        if (dev->nic.has_5ghz)
            for (i = 0; i < sizeof(chan_5g); i++, n++) mt_put_le16(dom + 12 + 8 * n, chan_5g[i]);
        mt_memset(hdr, 0, sizeof(hdr));
        hdr[0] = '0';                                /* alpha2 "00" (memcpy of char[3]) */
        hdr[1] = '0';
        hdr[4] = 0;                                  /* bw_2g BW_20_40M */
        hdr[5] = 3;                                  /* bw_5g BW_20_40_80_160M */
        hdr[6] = 3;                                  /* bw_6g */
        hdr[8] = dev->nic.has_2ghz ? (uint8_t)sizeof(chan_2g) : 0u;
        hdr[9] = dev->nic.has_5ghz ? (uint8_t)sizeof(chan_5g) : 0u;
        hdr[10] = 0;                                 /* n_6ch */
        mt_memcpy(dom, hdr, sizeof(hdr));
        err = mt_mcu_send(dev, "SET_CHAN_DOMAIN", MT_MCU_CE(MCU_CE_SET_CHAN_DOMAIN), dom, 12u + 8u * n, 0,
                          MT_MCU_TIMEOUT_US, NULL);
        if (err) return sn_fail(dev, 1, err);
    }
    /* SN2 */
    err = chan_info(dev, MT_MCU_EXT(MCU_EXT_SET_RX_PATH), "SET_RX_PATH", cfg->channel, cfg->band);
    if (err) return sn_fail(dev, 2, err);
    /* SN3 (TX power, mt76_connac_mcu_set_rate_txpower, is not sent: spec 11.5) */
    if (!dev->opt.skip_radio_ctrl) {
        dev->radio_on = 1;
        err = radio_ctrl(dev, EXT_CMD_RADIO_LED_CTRL_ENABLE, "ID_RADIO_ON_OFF_CTRL led enable");
        if (!err) err = radio_ctrl(dev, EXT_CMD_RADIO_ON_LED, "ID_RADIO_ON_OFF_CTRL radio on");
        if (err) return sn_fail(dev, 3, err);
    }
    /* SN4 */
    if (dev->nic.chip_cap & MT792X_CHIP_CAP_WF_RF_PIN_CTRL_EVT_EN) {
        uint8_t pin[2] = { WF_RF_PIN_INIT, 0 };
        err = mt_mcu_send(dev, "WF_RF_PIN_CTRL init", MT_MCU_EXT(MCU_EXT_WF_RF_PIN_CTRL), pin, sizeof(pin), 0,
                          MT_MCU_TIMEOUT_US, NULL);
        if (err) return sn_fail(dev, 4, err);
    }
    /* SN5-SN7: monitor interface */
    if (!dev->opt.skip_add_dev) {
        dev->dev_active = 1;     /* flags are set before sending, see mt_postboot S31 */
        err = dev_info(dev, 1, MT_MCU_TIMEOUT_US, &status);
        if (!err && status) {
            mt_logf(dev, MT_LOG_ERROR, "DEV_INFO_UPDATE active: status 0x%08x", (uint32_t)status);
            err = MT792X_E_MCU_STATUS;
        }
        if (err) return sn_fail(dev, 5, err);
        dev->bss_active = 1;
        err = bss_info(dev, 1, MT_MCU_TIMEOUT_US, &status);
        if (!err && status) {
            mt_logf(dev, MT_LOG_ERROR, "BSS_INFO_UPDATE active: status 0x%08x%s", (uint32_t)status,
                    (uint32_t)status == MT76_CONNAC_MCU_STATUS_WLAN_FAILURE ? " (wlan_failure)" : "");
            err = MT792X_E_MCU_STATUS;
        }
        if (err) return sn_fail(dev, 6, err);
        err = wtbl_update(dev, MT792X_WTBL_RESERVED);
        if (err) return sn_fail(dev, 7, err);
    }
    /* SN8: mt7921_configure_filter */
    if (cfg->flags & MT792X_SNIFF_FCSFAIL) fif |= MT7921_FILTER_FCSFAIL;
    if (cfg->flags & MT792X_SNIFF_CONTROL) fif |= MT7921_FILTER_CONTROL;
    if (cfg->flags & MT792X_SNIFF_OTHER_BSS) fif |= MT7921_FILTER_OTHER_BSS;
    err = rx_filter(dev, "SET_RX_FILTER", fif, 0, 0);
    if (err) return sn_fail(dev, 8, err);
    /* SN9-SN12: mt7921_sniffer_interface_iter with IEEE80211_CONF_MONITOR */
    dev->sniffer_on = 1;
    err = sniffer_enable(dev, 1, MT_MCU_TIMEOUT_US);
    if (err) return sn_fail(dev, 9, err);
    err = chip_config(dev, "CHIP_CONFIG KeepFullPwr 1", "KeepFullPwr 1");
    if (err) return sn_fail(dev, 10, err);
    err = mt_mcu_send(dev, "SET_BSS_ABORT", MT_MCU_CE(MCU_CE_SET_BSS_ABORT), abort_req, sizeof(abort_req), 0,
                      MT_MCU_TIMEOUT_US, NULL);
    if (err) return sn_fail(dev, 11, err);
    err = rx_filter(dev, "SET_RX_FILTER clear DROP_OTHER_BEACON", 0, MT7921_FIF_BIT_CLR, MT_WF_RFCR_DROP_OTHER_BEACON);
    if (err) return sn_fail(dev, 12, err);
    /* SN13: tune */
    {
        uint64_t t_resp = 0;
        err = tune(dev, cfg->channel, cfg->band, &t_resp);
    }
    if (err) return sn_fail(dev, 13, err);
    {
        uint32_t misc = mt_rr(dev, MT_CONN_ON_MISC);
        if (dev->bus_dead) return sn_fail(dev, 13, MT792X_E_BUS_DEAD);
        mt_logf(dev, MT_LOG_INFO, "sniffer running on channel %u via %s; CONN_ON_MISC 0x%08x", cfg->channel,
                tune_name(dev->tune_used), misc);
    }
    dev->sniff_started = 1;
    return 0;
}

int mt792x_sniffer_set_channel(struct mt792x *dev, uint8_t channel, uint8_t band, uint32_t *elapsed_us)
{
    uint64_t t0, t_resp;
    uint32_t el;
    int err;

    if (elapsed_us) *elapsed_us = 0;
    if (!dev) return MT792X_E_INVAL;
    if (dev->state != MT_ST_TOUCHED || !dev->sniff_started) return MT792X_E_STATE;
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    if (!chan_valid(channel, band)) return MT792X_E_INVAL;
    t0 = t_resp = mt_now(dev);
    err = tune(dev, channel, band, &t_resp);
    el = (uint32_t)(t_resp - t0);
    if (elapsed_us) *elapsed_us = el;
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "channel switch to %u failed: %s", channel, mt792x_strerror(err));
        return err;
    }
    mt_logf(dev, MT_LOG_INFO, "channel -> %u via %s: %u us until the last response", channel, tune_name(dev->tune_used), el);
    return 0;
}

/* Deactivation in the mt76 order: sniffer off (config CHANGE_MONITOR), then
 * mt76_connac_mcu_uni_add_dev(enable = false): BSS first, then DEV; WLAN_FAILURE on a BSS
 * deactivation is ignored (mt76_connac_mcu_bss_deact_err). Stops at the first transport error. */
static int sniffer_down(struct mt792x *dev, uint32_t timeout_us)
{
    int err = 0, status = 0;
    dev->sniff_started = 0;
    if (dev->sniffer_on) {
        err = sniffer_enable(dev, 0, timeout_us);
        if (err) return err;
        dev->sniffer_on = 0;
    }
    if (dev->bss_active) {
        err = bss_info(dev, 0, timeout_us, &status);
        if (err) return err;
        if (status && (uint32_t)status != MT76_CONNAC_MCU_STATUS_WLAN_FAILURE)
            mt_logf(dev, MT_LOG_WARN, "BSS_INFO_UPDATE inactive: status 0x%08x", (uint32_t)status);
        dev->bss_active = 0;
    }
    if (dev->dev_active) {
        err = dev_info(dev, 0, timeout_us, &status);
        if (err) return err;
        if (status) mt_logf(dev, MT_LOG_WARN, "DEV_INFO_UPDATE inactive: status 0x%08x", (uint32_t)status);
        dev->dev_active = 0;
    }
    return 0;
}

int mt792x_sniffer_stop(struct mt792x *dev)
{
    if (!dev) return MT792X_E_INVAL;
    if (dev->state != MT_ST_TOUCHED || !dev->postboot_done) return MT792X_E_STATE;
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    return sniffer_down(dev, MT_MCU_TIMEOUT_US);
}

/* Spec 7.3 step 1: best effort, short timeout, stops at the first failure. mt7921_stop order:
 * radio LED off (no wait), then mt792x_stop: MAC_INIT_CTRL disable. */
void mt_sniffer_shutdown(struct mt792x *dev)
{
    int err = 0;
    if (dev->bus_dead || !(dev->sniffer_on || dev->bss_active || dev->dev_active || dev->radio_on || dev->mac_enabled))
        return;
    mt_logf(dev, MT_LOG_INFO, "shutdown: stopping sniffer and MAC (best effort, %u ms per command)",
            MT_MCU_SHUTDOWN_TIMEOUT_US / 1000u);
    err = sniffer_down(dev, MT_MCU_SHUTDOWN_TIMEOUT_US);
    if (!err && dev->radio_on) {
        err = radio_ctrl(dev, EXT_CMD_RADIO_OFF_LED, "ID_RADIO_ON_OFF_CTRL radio off");
        if (!err) dev->radio_on = 0;
    }
    if (!err && dev->mac_enabled) {
        err = mac_enable(dev, 0, MT_MCU_SHUTDOWN_TIMEOUT_US);
        if (!err) dev->mac_enabled = 0;
    }
    if (err) mt_logf(dev, MT_LOG_WARN, "shutdown: MCU part stopped early (%s); continuing with the register part",
                     mt792x_strerror(err));
    dev->rx_active = 0;
}

const struct mt792x_nic_caps *mt792x_nic_caps(const struct mt792x *dev)
{
    return (dev && dev->nic.valid) ? &dev->nic : NULL;
}

int mt792x_tune_mode_used(const struct mt792x *dev)
{
    return dev ? dev->tune_used : 0;
}
