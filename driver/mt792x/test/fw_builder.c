/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fw_builder.h"

#include <stdlib.h>
#include <string.h>

/*
 * Default patch sections:
 *   0: addr 0x00200000 (MCU_PATCH_ADDRESS -> cmd 0x05), 5000 B (2 chunks), sec 0xffffffff (NOT_SUPPORT) -> 0x80000000
 *   1: addr 0x00900000 (-> cmd 0x05), 4096 B (exactly 1 chunk), AES key 2 -> 0x80000000|ENC|KEY(2)<<1|IV = 0x8000000d
 *   2: addr 0x00e00000 (-> cmd 0x01), 10 B, SCRAMBLE -> ENC|ENCRY_MODE_SEL|IV = 0x80000049
 * Default RAM regions:
 *   0: addr 0x00900000 (-> cmd 0x05, the rule applies to RAM too), 9000 B (3 chunks), feature 0 -> 0x80000000
 *   1: addr 0x0e000000, 300 B, NON_DL, type CLC -> skipped, bytes still consumed
 *   2: addr 0x00915000 (-> cmd 0x01), 4097 B, OVERRIDE|ENCRY_MODE|KEY 2|ENCRYPT (0x35) -> 0x8000004d
 *   3: addr 0x02000000, 128 B, ENCRYPT key 0 -> 0x80000009
 * FW_START: option 1 (override), addr 0x00915000.
 */
const struct fwb_section fwb_default_sections[FWB_DEFAULT_SECTIONS] = {
    { 0x00200000u, 5000u, 0xffffffffu, 0, 0x11 },
    { 0x00900000u, 4096u, 0x01000002u, 0, 0x22 },
    { 0x00e00000u, 10u, 0x02000000u, 0, 0x33 },
};
const uint32_t fwb_default_section_mode[FWB_DEFAULT_SECTIONS] = { 0x80000000u, 0x8000000du, 0x80000049u };
const uint8_t fwb_default_section_cmd[FWB_DEFAULT_SECTIONS] = { 0x05, 0x05, 0x01 };

const struct fwb_region fwb_default_regions[FWB_DEFAULT_REGIONS] = {
    { 0x00900000u, 9000u, 0x00, 0, 0x44 },
    { 0x0e000000u, 300u, 0x40, 2, 0x55 },
    { 0x00915000u, 4097u, 0x35, 0, 0x66 },
    { 0x02000000u, 128u, 0x01, 0, 0x77 },
};
const uint32_t fwb_default_region_mode[FWB_DEFAULT_REGIONS] = { 0x80000000u, 0x80000000u, 0x8000004du, 0x80000009u };
const uint8_t fwb_default_region_cmd[FWB_DEFAULT_REGIONS] = { 0x05, 0x01, 0x01, 0x01 };

void fwb_pattern(uint8_t *dst, uint32_t len, uint8_t seed)
{
    uint32_t x = 0x9e3779b9u ^ ((uint32_t)seed * 0x01000193u), i;
    for (i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        dst[i] = (uint8_t)(x >> 24);
    }
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint8_t *fwb_patch(const struct fwb_section *s, unsigned n, size_t *out_len)
{
    size_t total = 96u + 64u * (size_t)n, off;
    unsigned i;
    uint8_t *b;
    for (i = 0; i < n; i++) total += s[i].len;
    b = (uint8_t *)calloc(1, total);
    if (!b) return NULL;
    memcpy(b, "20261110123456a\n", 16);
    memcpy(b + 0x10, "ALPS", 4);
    be32(b + 0x14, 0x8a108a10u);
    be32(b + 0x18, 0xffffffffu);
    b[0x1c] = 0x12;
    b[0x1d] = 0x34;
    be32(b + 0x20, 0x11223344u);
    be32(b + 0x24, 0x00000004u);
    be32(b + 0x28, 0);
    be32(b + 0x2c, n);
    be32(b + 0x30, 0xffff0000u);
    off = 96u + 64u * (size_t)n;
    for (i = 0; i < n; i++) {
        uint8_t *e = b + 96u + 64u * i;
        be32(e + 0x00, s[i].type ? s[i].type : 0x00040002u);
        be32(e + 0x04, (uint32_t)off);
        be32(e + 0x08, s[i].len);
        be32(e + 0x0c, s[i].addr);
        be32(e + 0x10, s[i].len);
        be32(e + 0x14, s[i].sec_key_idx);
        be32(e + 0x18, 0x100u);
        fwb_pattern(b + off, s[i].len, s[i].seed);
        off += s[i].len;
    }
    *out_len = total;
    return b;
}

uint8_t *fwb_ram(const struct fwb_region *r, unsigned n, size_t *out_len)
{
    size_t data = 0, total, off = 0;
    unsigned i;
    uint8_t *b, *t;
    for (i = 0; i < n; i++) data += r[i].len;
    total = data + 40u * (size_t)n + 36u;
    b = (uint8_t *)calloc(1, total);
    if (!b) return NULL;
    for (i = 0; i < n; i++) {
        uint8_t *e = b + data + 40u * i;
        fwb_pattern(b + off, r[i].len, r[i].seed);
        off += r[i].len;
        le32(e + 0x00, 0xdeadbeefu);
        le32(e + 0x04, r[i].len);
        le32(e + 0x08, 0x1000u);
        le32(e + 0x10, r[i].addr);
        le32(e + 0x14, r[i].len);
        e[0x18] = r[i].feature_set;
        e[0x19] = r[i].type;
    }
    t = b + total - 36u;
    t[0] = 0x0d;
    t[1] = 0x01;
    t[2] = (uint8_t)n;
    t[3] = 0x02;
    t[4] = 0x01;
    memcpy(t + 0x07, "TEST000001", 10);
    memcpy(t + 0x11, "20261110123456Z", 15);
    le32(t + 0x20, 0xcafef00du);
    *out_len = total;
    return b;
}
