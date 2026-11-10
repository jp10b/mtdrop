/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fw_builder.h - synthetic MT7921 firmware images in the exact file formats of
 * docs/MT7921-BRINGUP.md section 5 (no real firmware in the repository).
 * Buffers are malloc'd with exactly the file length, so out-of-bounds reads are
 * not hidden by slack.
 */
#ifndef MT792X_TEST_FW_BUILDER_H
#define MT792X_TEST_FW_BUILDER_H

#include <stddef.h>
#include <stdint.h>

struct fwb_section {
    uint32_t addr, len, sec_key_idx;
    uint32_t type;      /* 0 = default 0x00000002 */
    uint8_t seed;
};

struct fwb_region {
    uint32_t addr, len;
    uint8_t feature_set, type, seed;
};

void fwb_pattern(uint8_t *dst, uint32_t len, uint8_t seed);

/* Patch: 96-byte header, n x 64-byte section table, then the section data in order. */
uint8_t *fwb_patch(const struct fwb_section *s, unsigned n, size_t *out_len);
/* RAM: region data concatenated from offset 0, then n x 40-byte regions, then the 36-byte trailer. */
uint8_t *fwb_ram(const struct fwb_region *r, unsigned n, size_t *out_len);

/* RAM image with a release-info block after the region data: features < 0 = block without a
 * feature tag, else a feature tag carrying that byte (MT792X_FW_CAP_CNM = 0x80). */
uint8_t *fwb_ram_features(const struct fwb_region *r, unsigned n, int features, size_t *out_len);

/* The default images used by most bring-up tests (see fw_builder.c for the exact content). */
#define FWB_DEFAULT_SECTIONS 3u
#define FWB_DEFAULT_REGIONS 4u
extern const struct fwb_section fwb_default_sections[FWB_DEFAULT_SECTIONS];
extern const struct fwb_region fwb_default_regions[FWB_DEFAULT_REGIONS];
/* Expected download mode / command id per default section / region, computed by hand from the spec. */
extern const uint32_t fwb_default_section_mode[FWB_DEFAULT_SECTIONS];
extern const uint8_t fwb_default_section_cmd[FWB_DEFAULT_SECTIONS];
extern const uint32_t fwb_default_region_mode[FWB_DEFAULT_REGIONS];
extern const uint8_t fwb_default_region_cmd[FWB_DEFAULT_REGIONS];

#endif /* MT792X_TEST_FW_BUILDER_H */
