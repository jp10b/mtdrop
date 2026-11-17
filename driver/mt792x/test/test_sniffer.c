/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Post-boot (S24-S31), sniffer start/stop, channel switching and shutdown against the simulated
 * chip. Expected command bytes are written out by hand from mt76 (docs/MT7921-BRINGUP.md 11). */
#include "test.h"
#include "rig.h"
#include "rx_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define K_CE(id) FAKE_KEY(FAKE_FMT_CE, (id), 0)
#define K_EXT(ext) FAKE_KEY(FAKE_FMT_EXT, 0xed, (ext))
#define K_UNI(id) FAKE_KEY(FAKE_FMT_UNI, (id), 0)

static const uint8_t chip_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static uint32_t cmd_key(const struct fake_cmd *c)
{
    return FAKE_KEY(c->fmt, c->cid, c->ext);
}

/* Index of the first command after the FW_START_REQ. */
static unsigned after_fw_start(const struct rig *r)
{
    unsigned i;
    for (i = 0; i < r->f->n_cmds; i++)
        if (r->f->cmds[i].fmt == FAKE_FMT_CMD && r->f->cmds[i].cid == 0x02 && r->f->cmds[i].ring == 17) return i + 1u;
    return r->f->n_cmds;
}

static unsigned check_keys(const struct rig *r, unsigned from, const uint32_t *keys, unsigned n)
{
    unsigned i, bad = 0;
    if (r->f->n_cmds - from != n) {
        printf("    %u commands from index %u, expected %u\n", r->f->n_cmds - from, from, n);
        bad++;
    }
    for (i = 0; i < n && from + i < r->f->n_cmds; i++) {
        if (cmd_key(&r->f->cmds[from + i]) != keys[i]) {
            printf("    command %u: key 0x%06x, expected 0x%06x\n", from + i, cmd_key(&r->f->cmds[from + i]), keys[i]);
            bad++;
        }
    }
    return bad;
}

/* Compares the raw bytes of command 'idx' with 'want' (byte 0x27 = the command's sequence number). */
static unsigned check_bytes(const struct rig *r, unsigned idx, const uint8_t *want, uint32_t len)
{
    const struct fake_cmd *c = &r->f->cmds[idx];
    uint8_t *w = (uint8_t *)malloc(len);
    uint32_t i;
    unsigned bad = 0;
    memcpy(w, want, len);
    w[0x27] = c->seq;
    if (c->total_len != len || !c->raw) {
        printf("    command %u: %u bytes, expected %u\n", idx, c->total_len, len);
        bad = 1;
    } else {
        for (i = 0; i < len; i++) {
            if (c->raw[i] != w[i]) {
                printf("    command %u byte 0x%02x: 0x%02x, expected 0x%02x\n", idx, i, c->raw[i], w[i]);
                bad = 1;
                break;
            }
        }
    }
    free(w);
    return bad;
}

/* 64-byte header skeleton for CE / EXT (spec 4.1 + 4.3) */
static void hdr64(uint8_t *b, uint32_t total, uint8_t cid, uint8_t set_query, uint8_t ext)
{
    memset(b, 0, 64);
    b[0] = (uint8_t)total;
    b[1] = (uint8_t)(total >> 8);
    b[3] = 0x41;
    b[6] = 0x01;
    b[7] = 0x80;
    b[0x20] = (uint8_t)(total - 32u);
    b[0x23] = 0x80;
    b[0x24] = cid;
    b[0x25] = 0xa0;
    b[0x26] = set_query;
    b[0x29] = ext;
    b[0x2b] = ext ? 1 : 0;
}

/* 48-byte UNI header skeleton (spec 4.2) */
static void hdr48(uint8_t *b, uint32_t total, uint8_t cid)
{
    memset(b, 0, 48);
    b[0] = (uint8_t)total;
    b[3] = 0x41;
    b[6] = 0x01;
    b[7] = 0x80;
    b[0x20] = (uint8_t)(total - 32u);
    b[0x22] = cid;
    b[0x25] = 0xa0;
    b[0x2b] = 0x07;
}

static const uint32_t postboot_keys[] = { K_CE(0x8a), K_CE(0xca), K_CE(0xc5), K_EXT(0x21), K_EXT(0x3e), K_CE(0xca), K_EXT(0x46) };

TEST(postboot_sequence_registers_and_bytes)
{
    struct rig r;
    uint8_t want[128];
    unsigned from;
    const struct mt792x_nic_caps *nc;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    CHECK(mt792x_last_step(r.dev) == 31);
    from = after_fw_start(&r);
    CHECK(check_keys(&r, from, postboot_keys, 7) == 0);

    /* GET_NIC_CAPAB: CE command without payload, set_query 1 (MCU_Q_SET, CE flag without QUERY) */
    hdr64(want, 64, 0x8a, 1, 0);
    CHECK(check_bytes(&r, from, want, 64) == 0);
    /* EFUSE_BUFFER_MODE: EXT 0x21, {buffer_mode 0 = EFUSE, format 1 = WHOLE, le16 len 0} */
    hdr64(want, 68, 0xed, 1, 0x21);
    want[64] = 0x00;
    want[65] = 0x01;
    want[66] = 0x00;
    want[67] = 0x00;
    CHECK(check_bytes(&r, from + 3, want, 68) == 0);
    {   /* written out completely once: */
        static const uint8_t efuse_full[68] = {
            0x44, 0x00, 0x00, 0x41, 0x00, 0x00, 0x01, 0x80, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x24, 0x00, 0x00, 0x80, 0xed, 0xa0, 0x01, 0x00, 0x00, 0x21, 0x00, 0x01, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x00, 0x01, 0x00, 0x00,
        };
        CHECK(check_bytes(&r, from + 3, efuse_full, 68) == 0);
    }
    /* PROTECT_CTRL: {prot_idx 1, band 0, rsv[2], le32 0x92b, le32 2} */
    hdr64(want, 76, 0xed, 1, 0x3e);
    memcpy(want + 64, "\x01\x00\x00\x00\x2b\x09\x00\x00\x02\x00\x00\x00", 12);
    CHECK(check_bytes(&r, from + 4, want, 76) == 0);
    /* MAC_INIT_CTRL enable */
    hdr64(want, 68, 0xed, 1, 0x46);
    want[64] = 1;
    CHECK(check_bytes(&r, from + 6, want, 68) == 0);
    /* FWLOG_2_HOST */
    hdr64(want, 68, 0xc5, 1, 0);
    want[64] = 1;
    CHECK(check_bytes(&r, from + 2, want, 68) == 0);

    CHECK(r.f->thermal_set && r.f->fwlog_on && r.f->efuse_mode_set && r.f->rts_set && r.f->mac_enabled);
    CHECK_EQ_U32(r.f->keep_full_pwr, 1);
    CHECK_EQ_U32(r.f->wtbl_updates, 20);
    /* mt7921_mac_init / mt792x_mac_init_band register values */
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x0f004u) & 0xfff8u, 1536u << 3);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x0f000u) & 0x88000u, 0x88000u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x210f4u), 0x3fu | 0x20000u | 0x40000u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xa10f4u), 0x3fu | 0x20000u | 0x40000u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x217c4u), 0x40000000u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x21780u), 0x40000000u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x24804u), 0x300u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x21e00u), 1536u << 3);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xa1e00u), 1536u << 3);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x23408u), 0x03000000u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xa3408u), 0x03000000u);
    nc = mt792x_nic_caps(r.dev);
    CHECK(nc != NULL);
    if (nc) {
        CHECK(nc->has_mac && memcmp(nc->mac, chip_mac, 6) == 0);
        CHECK_EQ_U32(nc->antenna_mask, 3);
        CHECK(nc->has_2ghz && nc->has_5ghz && !nc->has_6ghz && nc->has_phy);
        CHECK(nc->has_chip_cap && nc->chip_cap == 9u);
        CHECK_EQ_U32(nc->hw_ver_len, 8);
        CHECK_EQ_U32(nc->sw_ver_len, 8);
        CHECK_EQ_U32(nc->n_parsed, 6);
        CHECK(!nc->truncated);
    }
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK(rig_check_set_own_only_in_s1(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);
}

TEST(postboot_options)
{
    struct rig r;
    unsigned from;
    static const uint32_t keys[] = { K_CE(0x8a), K_EXT(0x21), K_EXT(0x3e), K_CE(0xca), K_EXT(0x46) };
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.skip_fw_log = 1;
    r.dev->opt.skip_thermal_protect = 1;
    r.dev->opt.no_rx_hdr_trans = 1;
    CHECK(rig_postboot(&r) == 0);
    from = after_fw_start(&r);
    CHECK(check_keys(&r, from, keys, 5) == 0);
    CHECK(!r.f->fwlog_on && !r.f->thermal_set);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x0f000u) & 0x80000u, 0);  /* header translation left off */
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x0f000u) & 0x8000u, 0x8000u);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

static const uint32_t sniff_keys_common[] = { K_CE(0x0f), K_EXT(0x4e), K_EXT(0x05), K_EXT(0x05), K_EXT(0xbd), K_UNI(0x01),
                                              K_UNI(0x02), K_CE(0x0a), K_UNI(0x24), K_CE(0xca), K_CE(0x17), K_CE(0x0a) };

TEST(sniffer_start_non_cnm_firmware_uses_channel_switch)
{
    struct rig r;
    uint32_t keys[16];
    uint8_t want[160];
    unsigned from, n;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    CHECK(r.dev->ram.fw_features == 0 && r.dev->ram.fw_features_status == -1);
    from = r.f->n_cmds;
    {
        struct mt792x_sniffer_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.channel = 6;
        cfg.band = MT792X_BAND_2GHZ;
        cfg.flags = MT792X_SNIFF_CONTROL | MT792X_SNIFF_OTHER_BSS;
        CHECK(mt792x_sniffer_start(r.dev, &cfg) == 0);
    }
    memcpy(keys, sniff_keys_common, sizeof(sniff_keys_common));
    n = sizeof(sniff_keys_common) / sizeof(sniff_keys_common[0]);
    keys[n++] = K_EXT(0x08);
    CHECK(check_keys(&r, from, keys, n) == 0);
    CHECK_EQ_U32(mt792x_tune_mode_used(r.dev), MT792X_TUNE_CHANNEL_SWITCH);

    /* SET_RX_PATH ch 6: {control 6, center 6, bw 0, tx_streams 2, rx_streams = mask 3, reason 0, ...} */
    hdr64(want, 140, 0xed, 1, 0x4e);
    memset(want + 64, 0, 76);
    want[64] = 6;
    want[65] = 6;
    want[67] = 2;
    want[68] = 3;
    CHECK(check_bytes(&r, from + 1, want, 140) == 0);
    /* CHANNEL_SWITCH: rx_streams = popcount */
    hdr64(want, 140, 0xed, 1, 0x08);
    want[64] = 6;
    want[65] = 6;
    want[67] = 2;
    want[68] = 2;
    CHECK(check_bytes(&r, from + 12, want, 140) == 0);
    /* DEV_INFO_UPDATE active (UNI 0x01): {omac 0, band 0, pad} + {tag 0, len 12, active 1, link 0, mac} */
    {
        static const uint8_t dev_full[64] = {
            0x40, 0x00, 0x00, 0x41, 0x00, 0x00, 0x01, 0x80, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x20, 0x00, 0x01, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0, 0, 0, 0,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x01, 0x00, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
        };
        CHECK(check_bytes(&r, from + 5, dev_full, 64) == 0);
    }
    /* BSS_INFO_UPDATE active: basic TLV, conn_type CONNECTION_INFRA_AP 0x00010002, conn_state 1 */
    hdr48(want, 84, 0x02);
    memset(want + 48, 0, 36);
    want[48 + 6] = 32;
    want[48 + 8] = 1;
    want[48 + 12] = 0x02;
    want[48 + 14] = 0x01;
    want[48 + 16] = 1;
    CHECK(check_bytes(&r, from + 6, want, 84) == 0);
    /* SNIFFER enable, UNI 0x24: written out completely */
    {
        static const uint8_t sniff_on[60] = {
            0x3c, 0x00, 0x00, 0x41, 0x00, 0x00, 0x01, 0x80, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x1c, 0x00, 0x24, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0, 0, 0, 0,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00,
        };
        CHECK(check_bytes(&r, from + 8, sniff_on, 60) == 0);
    }
    /* SET_RX_FILTER: mode 1, fif = ENABLE | CONTROL | OTHER_BSS */
    hdr64(want, 132, 0x0a, 1, 0);
    memset(want + 64, 0, 68);
    want[64 + 4] = 1;
    want[64 + 8] = 0x60;
    want[64 + 11] = 0x80;
    CHECK(check_bytes(&r, from + 7, want, 132) == 0);
    /* beacon filter off: mode 2, bit_op CLR (2), bit_map DROP_OTHER_BEACON BIT(11) */
    memset(want + 64, 0, 68);
    want[64 + 4] = 2;
    want[64 + 13] = 0x08;
    want[64 + 16] = 2;
    CHECK(check_bytes(&r, from + 11, want, 132) == 0);

    CHECK_EQ_U32(r.f->channel, 6);
    CHECK_EQ_U32(r.f->band, 0);
    CHECK(r.f->sniffer_on && r.f->dev_active && r.f->bss_active && r.f->other_beacon_drop_cleared);
    CHECK(memcmp(r.f->dev_mac, chip_mac, 6) == 0);
    CHECK_EQ_U32(r.f->rx_filter_fif, 0x80000060u);
    CHECK_EQ_U32(r.f->bss_aborts, 1);
    CHECK_EQ_U32(r.f->radio_led, 2);
    CHECK_EQ_U32(r.f->rf_pin_inits, 1);
    CHECK_EQ_U32(r.f->chan_domain_n2, 14);
    CHECK_EQ_U32(r.f->chan_domain_n5, 28);
    CHECK_EQ_U32(r.f->wtbl_updates, 21);
    /* mt792x_mac_set_timeing after CHANNEL_SWITCH (2.4 GHz: SIFS 10, slot 9) */
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x21090u), 0x003000e7u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x21094u), 0x001c003cu);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x210a4u), 0x090a0968u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x20884u) & 0x3fffu, 0x49u);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x20c80u) & 0x300u, 0);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    rig_free(&r);

    /* RX_DISABLE left set in the arbiter is cleared by the timing update; 5 GHz uses SIFS 16 */
    CHECK(rig_init(&r, 0) == 0);
    mock_set_reg(&r.m, 0x20c80u, 0x00000300u);
    r.m.log_watch = "had RX_DISABLE set";
    CHECK(rig_sniff(&r, 36, 0) == 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x20c80u), 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x210a4u), 0x09100968u);
    CHECK_EQ_U32(r.m.log_watch_hits, 1);
    rig_free(&r);
}

static void use_cnm_firmware(struct rig *r, int features)
{
    size_t rl;
    uint8_t *ram = fwb_ram_features(fwb_default_regions, FWB_DEFAULT_REGIONS, features, &rl);
    uint8_t *patch;
    size_t pl;
    patch = fwb_patch(fwb_default_sections, FWB_DEFAULT_SECTIONS, &pl);
    rig_set_fw(r, patch, pl, ram, rl);
}

TEST(sniffer_start_cnm_firmware_uses_sniffer_config)
{
    struct rig r;
    uint32_t keys[16];
    unsigned from, n;
    CHECK(rig_init(&r, 0) == 0);
    use_cnm_firmware(&r, 0x80);
    r.f->tune_by = 2;                       /* in this simulation only the sniffer config retunes */
    CHECK(rig_postboot(&r) == 0);
    CHECK_EQ_U32(r.dev->ram.fw_features, 0x80);
    CHECK_EQ_U32((uint32_t)(int)r.dev->ram.fw_features_status, 1);
    from = r.f->n_cmds;
    {
        struct mt792x_sniffer_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.channel = 6;
        cfg.flags = MT792X_SNIFF_CONTROL | MT792X_SNIFF_OTHER_BSS;
        CHECK(mt792x_sniffer_start(r.dev, &cfg) == 0);
    }
    memcpy(keys, sniff_keys_common, sizeof(sniff_keys_common));
    n = sizeof(sniff_keys_common) / sizeof(sniff_keys_common[0]);
    keys[n++] = K_UNI(0x24);
    keys[n++] = K_UNI(0x24);
    CHECK(check_keys(&r, from, keys, n) == 0);
    CHECK_EQ_U32(mt792x_tune_mode_used(r.dev), MT792X_TUNE_SNIFFER_CONFIG);
    {   /* SNIFFER config, UNI 0x24 tag 1: ch_band 1, bw 0, control 6, sco 0, center 6, drop_err 1 */
        static const uint8_t cfg_full[68] = {
            0x44, 0x00, 0x00, 0x41, 0x00, 0x00, 0x01, 0x80, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x24, 0x00, 0x24, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0, 0, 0, 0,
            0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x06, 0x00,
            0x01, 0x00, 0x00, 0x00,
        };
        CHECK(check_bytes(&r, from + 13, cfg_full, 68) == 0);
    }
    CHECK_EQ_U32(r.f->channel, 6);
    CHECK_EQ_U32(r.f->drop_err, 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);

    /* FCS-error frames wanted: drop_err 0 and FILTER_FCSFAIL in fif; 5 GHz channel 149 */
    CHECK(rig_init(&r, 0) == 0);
    use_cnm_firmware(&r, 0x80);
    CHECK(rig_sniff(&r, 149, MT792X_SNIFF_FCSFAIL) == 0);
    CHECK_EQ_U32(r.f->drop_err, 0);
    CHECK_EQ_U32(r.f->rx_filter_fif, 0x80000004u);
    CHECK_EQ_U32(r.f->channel, 149);
    CHECK_EQ_U32(r.f->band, 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(sniffer_forced_tune_modes)
{
    struct rig r;
    unsigned n0;
    /* CNM firmware but CHANNEL_SWITCH forced */
    CHECK(rig_init(&r, 0) == 0);
    use_cnm_firmware(&r, 0xff);
    r.dev->opt.tune_mode = MT792X_TUNE_CHANNEL_SWITCH;
    CHECK(rig_sniff(&r, 11, 0) == 0);
    CHECK(cmd_key(&r.f->cmds[r.f->n_cmds - 1u]) == K_EXT(0x08));
    rig_free(&r);
    /* both */
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.tune_mode = MT792X_TUNE_BOTH;
    CHECK(rig_sniff(&r, 1, 0) == 0);
    n0 = r.f->n_cmds;
    CHECK(cmd_key(&r.f->cmds[n0 - 3u]) == K_EXT(0x08));
    CHECK(cmd_key(&r.f->cmds[n0 - 2u]) == K_UNI(0x24));
    CHECK(cmd_key(&r.f->cmds[n0 - 1u]) == K_UNI(0x24));
    CHECK(mt792x_sniffer_set_channel(r.dev, 36, MT792X_BAND_5GHZ, NULL) == 0);
    CHECK_EQ_U32(r.f->n_cmds - n0, 3);
    CHECK_EQ_U32(r.f->channel, 36);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(firmware_feature_byte_parsing)
{
    struct mt792x_ram_info *ri = (struct mt792x_ram_info *)calloc(1, sizeof(*ri));
    size_t len;
    uint8_t *ram;
    ram = fwb_ram_features(fwb_default_regions, FWB_DEFAULT_REGIONS, 0x80, &len);
    CHECK(mt792x_ram_parse(ram, len, ri) == 0);
    CHECK_EQ_U32(ri->fw_features, 0x80);
    CHECK_EQ_U32((uint32_t)(int)ri->fw_features_status, 1);
    free(ram);
    ram = fwb_ram_features(fwb_default_regions, FWB_DEFAULT_REGIONS, 0x20, &len);
    CHECK(mt792x_ram_parse(ram, len, ri) == 0);
    CHECK_EQ_U32(ri->fw_features, 0x20);
    free(ram);
    ram = fwb_ram_features(fwb_default_regions, FWB_DEFAULT_REGIONS, -1, &len); /* no feature entry */
    CHECK(mt792x_ram_parse(ram, len, ri) == 0);
    CHECK_EQ_U32(ri->fw_features, 0);
    CHECK_EQ_U32((uint32_t)(int)ri->fw_features_status, 0);
    /* corrupt the outer length: block invalid, file still accepted (mt76 only warns) */
    {
        size_t data = 0;
        unsigned i;
        for (i = 0; i < FWB_DEFAULT_REGIONS; i++) data += fwb_default_regions[i].len;
        ram[data + 16] = 0xff;
        ram[data + 17] = 0x7f;
        CHECK(mt792x_ram_parse(ram, len, ri) == 0);
        CHECK_EQ_U32((uint32_t)(int)ri->fw_features_status, (uint32_t)-1);
        CHECK_EQ_U32(ri->fw_features, 0);
    }
    free(ram);
    ram = fwb_ram(fwb_default_regions, FWB_DEFAULT_REGIONS, &len);  /* no block at all */
    CHECK(mt792x_ram_parse(ram, len, ri) == 0);
    CHECK_EQ_U32((uint32_t)(int)ri->fw_features_status, (uint32_t)-1);
    free(ram);
    free(ri);
}

TEST(nic_caps_parser_bounds)
{
    struct mt792x_nic_caps c;
    uint8_t b[64];
    /* header only */
    memset(b, 0, sizeof(b));
    CHECK(mt792x_nic_caps_parse(b, 3, &c) == MT792X_E_NIC_CAPAB);
    CHECK(mt792x_nic_caps_parse(b, 4, &c) == 0 && c.n_parsed == 0 && !c.truncated);
    /* n_element 5 but one TLV whose length runs past the end */
    b[0] = 5;
    b[4] = 7;
    b[8] = 40;
    CHECK(mt792x_nic_caps_parse(b, 20, &c) == 0);
    CHECK(c.truncated && c.n_parsed == 0 && !c.has_mac);
    /* MAC TLV of 5 bytes is ignored (len >= ETH_ALEN required); then a short TLV header */
    memset(b, 0, sizeof(b));
    b[0] = 3;
    b[4] = 7;
    b[8] = 5;
    b[17] = 8; /* PHY type, but only 6 bytes left for its 8-byte header */
    CHECK(mt792x_nic_caps_parse(b, 23, &c) == 0);
    CHECK(!c.has_mac && c.n_parsed == 1 && c.truncated);
    /* PHY shorter than 11 bytes is ignored */
    memset(b, 0, sizeof(b));
    b[0] = 1;
    b[4] = 8;
    b[8] = 10;
    CHECK(mt792x_nic_caps_parse(b, 22, &c) == 0);
    CHECK(!c.has_phy && c.antenna_mask == 0 && c.n_parsed == 1);
    CHECK(mt792x_nic_caps_parse(NULL, 4, &c) == MT792X_E_INVAL);
}

TEST(sniffer_needs_a_mac_address)
{
    static const uint8_t no_mac[] = { 1, 0, 0, 0, 8, 0, 0, 0, 12, 0, 0, 0, 1, 1, 1, 2, 2, 0, 1, 1, 1, 1, 3, 1 };
    static const uint8_t mine[6] = { 0x06, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    struct mt792x_sniffer_cfg cfg;
    struct rig r;
    unsigned n0;
    CHECK(rig_init(&r, 0) == 0);
    r.f->nic_reply = no_mac;
    r.f->nic_reply_len = sizeof(no_mac);
    CHECK(rig_postboot(&r) == 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = 6;
    n0 = r.f->n_cmds;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_NO_MAC);
    CHECK_EQ_U32(r.f->n_cmds, n0);              /* nothing sent */
    cfg.mac = mine;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == 0);
    CHECK(memcmp(r.f->dev_mac, mine, 6) == 0);
    CHECK_EQ_U32(r.f->chan_domain_n2 + r.f->chan_domain_n5, 14 + 28);
    CHECK_EQ_U32(r.f->rf_pin_inits, 0);         /* no CHIP_CAP TLV: WF_RF_PIN_CTRL not sent */
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(sniffer_argument_and_state_checks)
{
    struct mt792x_sniffer_cfg cfg;
    struct mt792x_rx_frame fr;
    struct rig r;
    unsigned n0;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = 6;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_STATE);   /* no post-boot */
    CHECK(mt792x_rx_poll(r.dev, &fr) == MT792X_E_STATE);
    rig_free(&r);

    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    n0 = r.f->n_cmds;
    CHECK(mt792x_sniffer_set_channel(r.dev, 6, 0, NULL) == MT792X_E_STATE);
    cfg.channel = 15;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_INVAL);
    cfg.channel = 37;
    cfg.band = MT792X_BAND_5GHZ;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_INVAL);
    cfg.channel = 36;
    cfg.bw = 1;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_INVAL);
    CHECK(mt792x_sniffer_start(r.dev, NULL) == MT792X_E_INVAL);
    CHECK_EQ_U32(r.f->n_cmds, n0);
    cfg.bw = 0;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == 0);
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_STATE);   /* already running */
    CHECK(mt792x_sniffer_set_channel(r.dev, 14, MT792X_BAND_5GHZ, NULL) == MT792X_E_INVAL);
    CHECK(mt792x_sniffer_stop(r.dev) == 0);
    CHECK(mt792x_sniffer_set_channel(r.dev, 6, 0, NULL) == MT792X_E_STATE);
    cfg.channel = 1;
    cfg.band = 0;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == 0);                /* restart after stop */
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(channel_switch_latency_uses_the_clock)
{
    static const uint8_t hop[] = { 1, 6, 11, 6 };
    struct rig r;
    uint32_t el = 0;
    unsigned i;
    CHECK(rig_init(&r, 0) == 0);
    r.f->tune_delay_us = 1700;
    CHECK(rig_sniff(&r, 6, 0) == 0);
    for (i = 0; i < sizeof(hop); i++) {
        CHECK(mt792x_sniffer_set_channel(r.dev, hop[i], MT792X_BAND_2GHZ, &el) == 0);
        CHECK_EQ_U32(el, 1700);
        CHECK_EQ_U32(r.f->channel, hop[i]);
    }
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);

    CHECK(rig_init(&r, 0) == 0);
    use_cnm_firmware(&r, 0x80);
    r.f->tune_delay_us = 2500;
    CHECK(rig_sniff(&r, 44, 0) == 0);
    CHECK(mt792x_sniffer_set_channel(r.dev, 149, MT792X_BAND_5GHZ, &el) == 0);
    CHECK_EQ_U32(el, 2500);  /* sniffer enable is answered at once, the config after 2.5 ms */
    CHECK_EQ_U32(r.f->channel, 149);
    rig_free(&r);
}

/* Commands sent by a shutdown that started at command index 'from'. */
static unsigned check_shutdown_cmds(const struct rig *r, unsigned from, int with_sniffer)
{
    uint32_t keys[8];
    unsigned n = 0, bad;
    if (with_sniffer) {
        keys[n++] = K_UNI(0x24);
        keys[n++] = K_UNI(0x02);
        keys[n++] = K_UNI(0x01);
    }
    keys[n++] = K_EXT(0x05);
    keys[n++] = K_EXT(0x46);
    bad = check_keys(r, from, keys, n);
    if (!bad && with_sniffer) {
        if (r->f->cmds[from].raw[48 + 8] != 0) bad++;          /* sniffer enable = 0 */
        if (r->f->cmds[from + 1].raw[48 + 8] != 0) bad++;      /* BSS active = 0 */
        if (r->f->cmds[from + 2].raw[48 + 8] != 0) bad++;      /* DEV active = 0 */
    }
    if (!bad && (r->f->cmds[from + n - 2].raw[64] != 3 || r->f->cmds[from + n - 1].raw[64] != 0)) bad++;
    return bad;
}

TEST(shutdown_after_sniffing_stops_sniffer_then_mac_then_dma)
{
    struct rig r;
    uint32_t sd, regs_from, i;
    unsigned resets, n0;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    sd = r.m.trace_len;
    n0 = r.f->n_cmds;
    resets = r.f->wfsys_resets;
    mt792x_shutdown(r.dev);
    CHECK(check_shutdown_cmds(&r, n0, 1) == 0);
    CHECK(!r.f->sniffer_on && !r.f->bss_active && !r.f->dev_active && !r.f->mac_enabled);
    CHECK_EQ_U32(r.f->radio_led, 3);
    regs_from = rig_shutdown_regs_from(&r, sd);
    for (i = regs_from; i < r.m.trace_len; i++)   /* no MCU command after the register part began */
        CHECK(!(r.m.trace[i].is_write && r.m.trace[i].offset == 0xd4418u));
    CHECK(rig_check_shutdown(&r, regs_from, resets, 0x18451800u) == 0);
    CHECK(r.dev->shutdown_error == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK(rig_check_set_own_only_in_s1(&r) == 0);
    rig_free(&r);

    /* after sniffer_stop only the radio and MAC remain */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    CHECK(mt792x_sniffer_stop(r.dev) == 0);
    CHECK(!r.f->sniffer_on && !r.f->bss_active && !r.f->dev_active && r.f->mac_enabled);
    n0 = r.f->n_cmds;
    mt792x_shutdown(r.dev);
    CHECK(check_shutdown_cmds(&r, n0, 0) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);

    /* after stage 6 without a sniffer: only the MAC disable (no radio command was sent) */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    n0 = r.f->n_cmds;
    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    mt792x_shutdown(r.dev);
    CHECK_EQ_U32(r.f->n_cmds - n0, 1);
    CHECK(cmd_key(&r.f->cmds[n0]) == K_EXT(0x46));
    CHECK(rig_check_shutdown(&r, rig_shutdown_regs_from(&r, sd), resets, 0x18451800u) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(sniffer_fault_paths_leave_the_device_safe)
{
    struct rig r;
    struct mt792x_sniffer_cfg cfg;
    uint32_t sd;
    uint64_t t0;
    unsigned resets;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = 6;

    /* 1. sniffer enable never answered */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    r.f->drop_key = K_UNI(0x24);
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_MCU_TIMEOUT);
    CHECK_EQ_U32((uint32_t)r.dev->sniff_failed_step, 9);
    CHECK(mt792x_sniffer_set_channel(r.dev, 1, 0, NULL) == MT792X_E_STATE);
    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    t0 = r.m.clock_us;
    mt792x_shutdown(r.dev);                 /* the sniffer disable times out (500 ms), the rest is skipped */
    CHECK(r.m.clock_us - t0 >= 500000u && r.m.clock_us - t0 < 1000000u);
    CHECK(rig_check_shutdown(&r, rig_shutdown_regs_from(&r, sd), resets, 0x18451800u) == 0);
    CHECK(r.f->violations == 0);
    rig_free(&r);

    /* 2. DEV_INFO answered with an error status; shutdown still deactivates and disables */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    r.f->uni_status_dev = 0x5;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == MT792X_E_MCU_STATUS);
    CHECK_EQ_U32((uint32_t)r.dev->sniff_failed_step, 5);
    r.f->uni_status_dev = 0;
    mt792x_shutdown(r.dev);
    CHECK(!r.f->mac_enabled && !r.f->dev_active);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);

    /* 3. a reply with the right seq but a wrong cid is skipped (mt76 -EAGAIN) */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_postboot(&r) == 0);
    r.f->uni_wrong_cid = 1;
    CHECK(mt792x_sniffer_start(r.dev, &cfg) == 0);
    CHECK_EQ_U32(r.f->uni_wrong_cid_sent, 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);

    /* 4. no response at all from the moment the shutdown starts */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    r.f->drop_all_from_cmd = (int)r.f->cmd_count + 1;
    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    t0 = r.m.clock_us;
    mt792x_shutdown(r.dev);
    CHECK(r.m.clock_us - t0 < 1000000u);
    CHECK(rig_check_shutdown(&r, rig_shutdown_regs_from(&r, sd), resets, 0x18451800u) == 0);
    rig_free(&r);

    /* 5. post-boot: EFUSE_BUFFER_MODE never answered -> S27, nothing to undo on the MCU side */
    CHECK(rig_init(&r, 0) == 0);
    r.f->drop_key = K_EXT(0x21);
    CHECK(rig_postboot(&r) == MT792X_E_MCU_TIMEOUT);
    CHECK_EQ_U32((uint32_t)mt792x_failed_step(r.dev), 27);
    CHECK_EQ_U32((uint32_t)mt792x_last_step(r.dev), 26);
    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    {
        unsigned n0 = r.f->n_cmds;
        mt792x_shutdown(r.dev);
        CHECK_EQ_U32(r.f->n_cmds, n0);
    }
    CHECK(rig_check_shutdown(&r, sd, resets, 0x18451800u) == 0);
    CHECK(r.f->violations == 0);
    rig_free(&r);

    /* 6. MAC enable never answered: shutdown still tries to disable it */
    CHECK(rig_init(&r, 0) == 0);
    r.f->drop_key = K_EXT(0x46);
    CHECK(rig_postboot(&r) == MT792X_E_MCU_TIMEOUT);
    CHECK_EQ_U32((uint32_t)mt792x_failed_step(r.dev), 31);
    {
        unsigned n0 = r.f->n_cmds;
        r.f->drop_key = 0;
        mt792x_shutdown(r.dev);
        CHECK_EQ_U32(r.f->n_cmds - n0, 1);
        CHECK(!r.f->mac_enabled);
    }
    CHECK(r.f->violations == 0);
    rig_free(&r);
}

/* The bus dies after N register accesses, for N spread over post-boot and sniffer start. */
TEST(fault_bus_dead_sweep_postboot_and_sniffer)
{
    struct rig r;
    uint32_t begin, total, n, stride;
    unsigned runs = 0;

    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    begin = r.f->accesses;
    rig_free(&r);
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    total = r.f->accesses;
    rig_free(&r);
    CHECK(total > begin + 50u);
    stride = (total - begin) / 41u ? (total - begin) / 41u : 1u;

    for (n = begin; n < total; n += stride) {
        int err;
        uint32_t sd;
        CHECK(rig_init(&r, 0) == 0);
        r.f->bus_dead_after = n;
        err = rig_sniff(&r, 6, 0);
        if (err != MT792X_E_BUS_DEAD) {
            printf("  bus dead after %u accesses: err %d (%s) at S%d / SN%d\n", n, err, mt792x_strerror(err),
                   mt792x_failed_step(r.dev), r.dev->sniff_failed_step);
            CHECK(0);
        }
        CHECK_EQ_U32(r.f->writes_after_dead_read, 0);
        CHECK(r.f->violations == 0);
        sd = r.m.trace_len;
        mt792x_shutdown(r.dev);
        CHECK(r.dev->shutdown_error != 0);
        CHECK(r.m.trace_len - sd < 200u);
        rig_free(&r);
        runs++;
    }
    CHECK(runs >= 40u);
}

TEST(shutdown_after_stage_6_and_stop_list)
{
    struct rig r;
    struct mt792x_stop_op ops[MT792X_STOP_OPS_COUNT];
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    /* the proxy replay alone (client died) still stops DMA */
    rig_replay_stop_ops(&r);
    CHECK(!(fake_glo_cfg(r.f) & 0x5u));
    CHECK(mt792x_stop_ops(ops, MT792X_STOP_OPS_COUNT) == 13u); /* unchanged by the sniffer work (F-011 added three) */
    rig_free(&r);
}
