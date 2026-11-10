/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fw.c - validation and parsing of the MT7921 ROM patch and RAM firmware files.
 *
 * File formats, download-mode derivation and the command-id rule are derived from
 * Linux mt76 mt76_connac_mcu.c / mt76_connac_mcu.h (mt76_connac2_patch_check,
 * mt76_connac2_load_patch, mt76_connac2_get_data_mode, mt76_connac2_fw_check,
 * mt76_connac_mcu_send_ram_firmware, mt76_connac_mcu_gen_dl_mode,
 * mt76_connac_mcu_init_download), openwrt/mt76 @ 0936617, BSD-3-Clause-Clear,
 * Copyright (C) 2020 MediaTek Inc. Spec: docs/MT7921-BRINGUP.md section 5.
 *
 * Checks beyond mt76 (the files are untrusted input here): section/region counts are
 * limited; zero-length downloads, 32-bit address wrap, overlapping file ranges or load
 * ranges, patch data inside the header/section table, unknown encryption types and an
 * override flag with address 0 are rejected; mt76 checks the section type only after it
 * has taken the semaphore, we check it up front.
 */
#include "internal.h"
#include "regs.h"

#define MCU_PATCH_ADDRESS 0x200000u

uint8_t mt792x_init_dl_cmd(uint32_t addr)
{
    /* is_connac2: PATCH_START_REQ for MCU_PATCH_ADDRESS and 0x900000 */
    return (addr == MCU_PATCH_ADDRESS || addr == 0x900000u) ? MCU_CMD_PATCH_START_REQ
                                                            : MCU_CMD_TARGET_ADDRESS_LEN_REQ;
}

uint32_t mt792x_patch_dl_mode(uint32_t info, int *known)
{
    uint32_t mode = 0x80000000u; /* DL_MODE_NEED_RSP */
    if (known) *known = 1;
    if (info == 0xffffffffu) return mode; /* PATCH_SEC_NOT_SUPPORT */
    switch (info >> 24) {
    case 0x00: /* PLAIN */
        break;
    case 0x01: /* AES: ENCRYPT | KEY_IDX(info & 0xff) | RESET_SEC_IV */
        mode |= 0x1u | (((info & 0xffu) << 1) & 0x6u) | 0x8u;
        break;
    case 0x02: /* SCRAMBLE: ENCRYPT | ENCRY_MODE_SEL | RESET_SEC_IV */
        mode |= 0x1u | 0x40u | 0x8u;
        break;
    default:
        if (known) *known = 0;
        break;
    }
    return mode;
}

/* mt792x_get_offload_capability (mt792x_core.c:861-929): after the concatenated region data
 * ('regions_len' = sum of all region lengths, as mt76_connac2_fw_check returns it) come 16
 * bytes, then a release-info header {le16 len; u8 pad_len; u8 tag} whose 'len' bytes hold a
 * sequence of release-info entries (same header, then len data bytes and pad_len padding).
 * The entry with tag MT792X_FW_TAG_FEATURE holds struct mt792x_fw_features {u8 segment;
 * u8 data; u8 rsv[14]} (mt792x.h:72-82); 'data' is the feature byte.
 * Returns 1 = found, 0 = no feature entry, -1 = malformed/missing (mt76: "Invalid firmware"). */
static int ram_features(const uint8_t *data, uint64_t fw_end, uint64_t regions_len, uint8_t *features)
{
    uint64_t p, end;
    uint32_t len;

    *features = 0;
    if (regions_len > fw_end || fw_end - regions_len < 16u + 4u) return -1;
    p = regions_len + 16u;
    len = mt_get_le16(data + p);
    p += 4u;
    if (len > fw_end - p) return -1;
    end = p + len;
    while (p < end) {
        uint32_t elen, pad;
        uint8_t tag;
        if (end - p < 4u) return -1;
        elen = mt_get_le16(data + p);
        pad = data[p + 2u];
        tag = data[p + 3u];
        p += 4u;
        if (elen > end - p || pad > end - p - elen) return -1;
        if (tag == MT792X_FW_TAG_FEATURE) {
            if (elen < 16u) return -1;
            *features = data[p + 1u];
            return 1;
        }
        p += (uint64_t)elen + pad;
    }
    return 0;
}

uint32_t mt792x_ram_dl_mode(uint8_t fs)
{
    uint32_t mode = 0;
    if (fs & MT792X_FW_FEATURE_SET_ENCRYPT) mode |= 0x1u | 0x8u;      /* ENCRYPT | RESET_SEC_IV */
    if (fs & MT792X_FW_FEATURE_ENCRY_MODE) mode |= 0x40u;             /* DL_CONFIG_ENCRY_MODE_SEL */
    mode |= (((uint32_t)fs >> 1) & 0x3u) << 1;                        /* DL_MODE_KEY_IDX */
    mode |= 0x80000000u;                                              /* DL_MODE_NEED_RSP */
    return mode;
}

static void copy_text(char *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
    dst[i] = '\0';
}

static int ranges_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

int mt792x_patch_parse(const uint8_t *data, size_t len, struct mt792x_patch_info *out)
{
    uint32_t n, i, j;
    uint64_t table_end, total = 0;

    if (!data || !out) return MT792X_E_INVAL;
    mt_memset(out, 0, sizeof(*out));
    if (len < MT792X_PATCH_HDR_LEN) return MT792X_E_FW_TRUNCATED;

    copy_text(out->build_date, data + 0x00, 16);
    copy_text(out->platform, data + 0x10, 4);
    out->hw_sw_ver = mt_get_be32(data + 0x14);
    out->patch_ver = mt_get_be32(data + 0x18);
    out->checksum = mt_get_be16(data + 0x1c);
    out->desc_patch_ver = mt_get_be32(data + 0x20);
    out->subsys = mt_get_be32(data + 0x24);
    out->feature = mt_get_be32(data + 0x28);
    n = mt_get_be32(data + 0x2c);
    out->crc = mt_get_be32(data + 0x30);

    if (n == 0 || n > MT792X_FW_MAX_SECTIONS) return MT792X_E_FW_COUNT;
    table_end = MT792X_PATCH_HDR_LEN + (uint64_t)n * MT792X_PATCH_SEC_LEN;
    if (table_end > len) return MT792X_E_FW_COUNT;

    for (i = 0; i < n; i++) {
        const uint8_t *s = data + MT792X_PATCH_HDR_LEN + (size_t)i * MT792X_PATCH_SEC_LEN;
        struct mt792x_patch_section *sec = &out->sec[i];
        int known;

        sec->type = mt_get_be32(s + 0x00);
        sec->offs = mt_get_be32(s + 0x04);
        sec->size = mt_get_be32(s + 0x08);
        sec->addr = mt_get_be32(s + 0x0c);
        sec->len = mt_get_be32(s + 0x10);
        sec->sec_key_idx = mt_get_be32(s + 0x14);
        sec->align_len = mt_get_be32(s + 0x18);

        if ((sec->type & 0xffffu) != 0x2u) return MT792X_E_FW_SECTION_TYPE; /* PATCH_SEC_TYPE_INFO */
        if (sec->len == 0) return MT792X_E_FW_LENGTH;
        if ((uint64_t)sec->offs > len || (uint64_t)sec->len > len - sec->offs) return MT792X_E_FW_BOUNDS;
        if ((uint64_t)sec->offs < table_end) return MT792X_E_FW_BOUNDS;
        if ((uint64_t)sec->addr + sec->len > 0x100000000ull) return MT792X_E_FW_LENGTH;
        sec->mode = mt792x_patch_dl_mode(sec->sec_key_idx, &known);
        if (!known) return MT792X_E_FW_ENC_TYPE;
        sec->cmd_id = mt792x_init_dl_cmd(sec->addr);
        for (j = 0; j < i; j++) {
            if (ranges_overlap(sec->offs, sec->len, out->sec[j].offs, out->sec[j].len) ||
                ranges_overlap(sec->addr, sec->len, out->sec[j].addr, out->sec[j].len))
                return MT792X_E_FW_OVERLAP;
        }
        total += sec->len;
    }
    out->n_sections = n;
    out->total_dl_bytes = (uint32_t)total; /* <= file length */
    return 0;
}

int mt792x_ram_parse(const uint8_t *data, size_t len, struct mt792x_ram_info *out)
{
    const uint8_t *t;
    uint32_t n, i, j, n_dl = 0;
    uint64_t data_len, offset = 0, total = 0;

    if (!data || !out) return MT792X_E_INVAL;
    mt_memset(out, 0, sizeof(*out));
    if (len < MT792X_RAM_TRAILER_LEN) return MT792X_E_FW_TRUNCATED;

    t = data + len - MT792X_RAM_TRAILER_LEN;
    out->chip_id = t[0x00];
    out->eco_code = t[0x01];
    n = t[0x02];
    out->format_ver = t[0x03];
    out->format_flag = t[0x04];
    copy_text(out->fw_ver, t + 0x07, 10);
    copy_text(out->build_date, t + 0x11, 15);
    out->crc = mt_get_le32(t + 0x20);

    if (n == 0 || n > MT792X_FW_MAX_REGIONS) return MT792X_E_FW_COUNT;
    if ((uint64_t)n * MT792X_RAM_REGION_LEN > len - MT792X_RAM_TRAILER_LEN) return MT792X_E_FW_COUNT;
    data_len = len - MT792X_RAM_TRAILER_LEN - (uint64_t)n * MT792X_RAM_REGION_LEN;

    for (i = 0; i < n; i++) {
        const uint8_t *r = data + (size_t)data_len + (size_t)i * MT792X_RAM_REGION_LEN;
        struct mt792x_ram_region *reg = &out->reg[i];

        reg->decomp_crc = mt_get_le32(r + 0x00);
        reg->decomp_len = mt_get_le32(r + 0x04);
        reg->decomp_blk_sz = mt_get_le32(r + 0x08);
        reg->addr = mt_get_le32(r + 0x10);
        reg->len = mt_get_le32(r + 0x14);
        reg->feature_set = r[0x18];
        reg->type = r[0x19];
        reg->download = (reg->feature_set & MT792X_FW_FEATURE_NON_DL) ? 0 : 1;
        reg->mode = mt792x_ram_dl_mode(reg->feature_set);
        reg->cmd_id = mt792x_init_dl_cmd(reg->addr);

        if (offset + reg->len > data_len) return MT792X_E_FW_BOUNDS; /* mt76_connac2_fw_check */
        reg->file_offset = (uint32_t)offset;
        offset += reg->len;
        if (!reg->download) continue;

        if (reg->len == 0) return MT792X_E_FW_LENGTH;
        if ((uint64_t)reg->addr + reg->len > 0x100000000ull) return MT792X_E_FW_LENGTH;
        if (reg->feature_set & MT792X_FW_FEATURE_OVERRIDE_ADDR) {
            if (reg->addr == 0) return MT792X_E_FW_OVERRIDE;
            out->override_addr = reg->addr; /* the last one wins, as in mt76 */
        }
        for (j = 0; j < i; j++) {
            if (out->reg[j].download && ranges_overlap(reg->addr, reg->len, out->reg[j].addr, out->reg[j].len))
                return MT792X_E_FW_OVERLAP;
        }
        total += reg->len;
        n_dl++;
    }
    if (n_dl == 0) return MT792X_E_FW_COUNT;
    out->n_regions = n;
    out->data_len = (uint32_t)data_len;
    out->total_dl_bytes = (uint32_t)total;
    out->fw_start_option = out->override_addr ? FW_START_OVERRIDE : 0u;
    out->fw_features_status = (int8_t)ram_features(data, data_len, offset, &out->fw_features);
    return 0;
}
