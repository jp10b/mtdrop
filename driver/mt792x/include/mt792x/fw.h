/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mt792x/fw.h - parsing and validation of the MT7921 firmware files, usable
 * without a device:
 *
 *   ROM patch  WIFI_MT7961_patch_mcu_1_2_hdr.bin  (big-endian header + section table)
 *   RAM code   WIFI_RAM_CODE_MT7961_1.bin         (little-endian region table + trailer at the end)
 *
 * File formats follow Linux mt76 (struct mt76_connac2_patch_hdr / _patch_sec /
 * _fw_trailer / _fw_region, mt76_connac_mcu.h; validation and download modes from
 * mt76_connac_mcu.c), openwrt/mt76 @ 0936617, BSD-3-Clause-Clear. See
 * docs/MT7921-BRINGUP.md section 5.
 *
 * Files are treated as untrusted input: parsing never reads outside the given
 * buffer and everything the bring-up later relies on is validated here.
 */
#ifndef MT792X_FW_H
#define MT792X_FW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT792X_PATCH_HDR_LEN    96u
#define MT792X_PATCH_SEC_LEN    64u
#define MT792X_RAM_TRAILER_LEN  36u
#define MT792X_RAM_REGION_LEN   40u
/* Sanity limits (the real MT7961 files have 1 patch section and 4 RAM regions). */
#define MT792X_FW_MAX_SECTIONS  16u
#define MT792X_FW_MAX_REGIONS   32u

/* RAM region feature_set bits (mt76_connac_mcu.h) */
#define MT792X_FW_FEATURE_SET_ENCRYPT    0x01u
#define MT792X_FW_FEATURE_SET_KEY_IDX    0x06u
#define MT792X_FW_FEATURE_ENCRY_MODE     0x10u
#define MT792X_FW_FEATURE_OVERRIDE_ADDR  0x20u
#define MT792X_FW_FEATURE_NON_DL         0x40u

struct mt792x_patch_section {
    uint32_t type;          /* raw, (type & 0xffff) == 2 */
    uint32_t offs;          /* file offset of the data */
    uint32_t size;          /* raw 'size' field (informational, not used by mt76) */
    uint32_t addr;          /* load address */
    uint32_t len;           /* bytes to download */
    uint32_t sec_key_idx;   /* raw security info */
    uint32_t align_len;
    uint32_t mode;          /* download mode derived from sec_key_idx */
    uint8_t cmd_id;         /* 0x05 PATCH_START_REQ or 0x01 TARGET_ADDRESS_LEN_REQ */
};

struct mt792x_patch_info {
    char build_date[17];    /* NUL-terminated, non-printable bytes replaced by '?' */
    char platform[5];
    uint32_t hw_sw_ver;
    uint32_t patch_ver;
    uint16_t checksum;
    uint32_t desc_patch_ver;
    uint32_t subsys;
    uint32_t feature;
    uint32_t crc;
    uint32_t n_sections;
    uint32_t total_dl_bytes;
    struct mt792x_patch_section sec[MT792X_FW_MAX_SECTIONS];
};

struct mt792x_ram_region {
    uint32_t file_offset;   /* region data offset in the file (regions are concatenated from 0) */
    uint32_t addr;
    uint32_t len;
    uint32_t decomp_crc;
    uint32_t decomp_len;
    uint32_t decomp_blk_sz;
    uint8_t feature_set;
    uint8_t type;           /* 0 default, 2 CLC */
    uint8_t download;       /* 0 if FW_FEATURE_NON_DL (skipped, bytes still consumed) */
    uint8_t cmd_id;         /* as for patch sections */
    uint32_t mode;          /* download mode derived from feature_set */
};

struct mt792x_ram_info {
    uint8_t chip_id;
    uint8_t eco_code;
    uint8_t format_ver;
    uint8_t format_flag;
    char fw_ver[11];        /* NUL-terminated, non-printable bytes replaced by '?' */
    char build_date[16];
    uint32_t crc;
    uint32_t n_regions;
    uint32_t data_len;      /* bytes before the region table */
    uint32_t total_dl_bytes;
    uint32_t override_addr; /* 0 = none */
    uint32_t fw_start_option;
    struct mt792x_ram_region reg[MT792X_FW_MAX_REGIONS];
};

/* Parse and validate. Return 0 or a negative MT792X_E_FW_* code (see mt792x.h). */
int mt792x_patch_parse(const uint8_t *data, size_t len, struct mt792x_patch_info *out);
int mt792x_ram_parse(const uint8_t *data, size_t len, struct mt792x_ram_info *out);

/* Building blocks, exposed for tests and diagnostics. */
uint32_t mt792x_patch_dl_mode(uint32_t sec_key_idx, int *known_enc_type);
uint32_t mt792x_ram_dl_mode(uint8_t feature_set);
uint8_t mt792x_init_dl_cmd(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* MT792X_FW_H */
