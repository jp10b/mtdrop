/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Firmware file parsing and validation (spec section 5), including a deterministic fuzz loop. */
#include "test.h"
#include "fw_builder.h"
#include "mt792x/mt792x.h"

#include <stdlib.h>
#include <string.h>

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

TEST(fw_modes_and_command_rule)
{
    int known = 0;
    CHECK_EQ_U32(mt792x_patch_dl_mode(0xffffffffu, &known), 0x80000000u);
    CHECK(known == 1);
    CHECK_EQ_U32(mt792x_patch_dl_mode(0x00000000u, &known), 0x80000000u);
    CHECK_EQ_U32(mt792x_patch_dl_mode(0x01000000u, &known), 0x80000009u);
    CHECK_EQ_U32(mt792x_patch_dl_mode(0x01000001u, &known), 0x8000000bu);
    CHECK_EQ_U32(mt792x_patch_dl_mode(0x010000ffu, &known), 0x8000000fu); /* key masked to 2 bits */
    CHECK_EQ_U32(mt792x_patch_dl_mode(0x02001234u, &known), 0x80000049u);
    CHECK(known == 1);
    (void)mt792x_patch_dl_mode(0x03000000u, &known);
    CHECK(known == 0);
    CHECK_EQ_U32(mt792x_ram_dl_mode(0x00), 0x80000000u);
    CHECK_EQ_U32(mt792x_ram_dl_mode(0x01), 0x80000009u);
    CHECK_EQ_U32(mt792x_ram_dl_mode(0x06), 0x80000006u);
    CHECK_EQ_U32(mt792x_ram_dl_mode(0x10), 0x80000040u);
    CHECK_EQ_U32(mt792x_ram_dl_mode(0x35), 0x8000004du);
    CHECK_EQ_U32(mt792x_ram_dl_mode(0x60), 0x80000000u); /* OVERRIDE / NON_DL are not mode bits */
    CHECK_EQ_U32(mt792x_init_dl_cmd(0x00200000u), 0x05);
    CHECK_EQ_U32(mt792x_init_dl_cmd(0x00900000u), 0x05);
    CHECK_EQ_U32(mt792x_init_dl_cmd(0x00900004u), 0x01);
    CHECK_EQ_U32(mt792x_init_dl_cmd(0xe0002800u), 0x01); /* MT7925-only special address */
}

TEST(fw_parse_default_images)
{
    struct mt792x_patch_info *p = (struct mt792x_patch_info *)calloc(1, sizeof(*p));
    struct mt792x_ram_info *r = (struct mt792x_ram_info *)calloc(1, sizeof(*r));
    size_t pl, rl, off = 96u + 3u * 64u;
    uint8_t *pb = fwb_patch(fwb_default_sections, FWB_DEFAULT_SECTIONS, &pl);
    uint8_t *rb = fwb_ram(fwb_default_regions, FWB_DEFAULT_REGIONS, &rl);
    unsigned i;
    uint32_t foff = 0;

    CHECK(mt792x_patch_parse(pb, pl, p) == 0);
    CHECK_EQ_U32(p->n_sections, 3);
    CHECK(strcmp(p->build_date, "20261110123456a?") == 0);
    CHECK(strcmp(p->platform, "ALPS") == 0);
    CHECK_EQ_U32(p->hw_sw_ver, 0x8a108a10u);
    CHECK_EQ_U32(p->checksum, 0x1234u);
    CHECK_EQ_U32(p->total_dl_bytes, 5000u + 4096u + 10u);
    for (i = 0; i < 3; i++) {
        CHECK_EQ_U32(p->sec[i].offs, off);
        CHECK_EQ_U32(p->sec[i].addr, fwb_default_sections[i].addr);
        CHECK_EQ_U32(p->sec[i].len, fwb_default_sections[i].len);
        CHECK_EQ_U32(p->sec[i].mode, fwb_default_section_mode[i]);
        CHECK_EQ_U32(p->sec[i].cmd_id, fwb_default_section_cmd[i]);
        off += fwb_default_sections[i].len;
    }

    CHECK(mt792x_ram_parse(rb, rl, r) == 0);
    CHECK_EQ_U32(r->n_regions, 4);
    CHECK(strcmp(r->fw_ver, "TEST000001") == 0);
    CHECK(strcmp(r->build_date, "20261110123456Z") == 0);
    CHECK_EQ_U32(r->chip_id, 0x0d);
    CHECK_EQ_U32(r->crc, 0xcafef00du);
    CHECK_EQ_U32(r->override_addr, 0x00915000u);
    CHECK_EQ_U32(r->fw_start_option, 1);
    CHECK_EQ_U32(r->total_dl_bytes, 9000u + 4097u + 128u);
    for (i = 0; i < 4; i++) {
        CHECK_EQ_U32(r->reg[i].file_offset, foff);
        CHECK_EQ_U32(r->reg[i].addr, fwb_default_regions[i].addr);
        CHECK_EQ_U32(r->reg[i].mode, fwb_default_region_mode[i]);
        CHECK_EQ_U32(r->reg[i].cmd_id, fwb_default_region_cmd[i]);
        CHECK_EQ_U32(r->reg[i].download, i != 1);
        foff += fwb_default_regions[i].len;
    }
    CHECK_EQ_U32(r->reg[1].type, 2);
    free(pb);
    free(rb);
    free(p);
    free(r);
}

static int patch_err(const struct fwb_section *s, unsigned n, void (*mutate)(uint8_t *, size_t *))
{
    static struct mt792x_patch_info info;
    size_t len;
    int err;
    uint8_t *b = fwb_patch(s, n, &len);
    if (mutate) mutate(b, &len);
    err = mt792x_patch_parse(b, len, &info);
    free(b);
    return err;
}

static void m_n0(uint8_t *b, size_t *l) { (void)l; put_be32(b + 0x2c, 0); }
static void m_nbig(uint8_t *b, size_t *l) { (void)l; put_be32(b + 0x2c, 17); }
static void m_ntable(uint8_t *b, size_t *l) { (void)l; put_be32(b + 0x2c, 4); }        /* table runs into data/past end */
static void m_type(uint8_t *b, size_t *l) { (void)l; put_be32(b + 96, 0x00040003u); }
static void m_offs(uint8_t *b, size_t *l) { put_be32(b + 96 + 4, (uint32_t)*l + 1u); }
static void m_len(uint8_t *b, size_t *l) { put_be32(b + 96 + 16, (uint32_t)*l); }
static void m_offs_hdr(uint8_t *b, size_t *l) { (void)l; put_be32(b + 96 + 4, 100); }
static void m_len0(uint8_t *b, size_t *l) { (void)l; put_be32(b + 96 + 16, 0); }
static void m_wrap(uint8_t *b, size_t *l) { (void)l; put_be32(b + 96 + 12, 0xfffffff0u); }
static void m_file_overlap(uint8_t *b, size_t *l) { (void)l; put_be32(b + 96 + 64 + 4, 96u + 3u * 64u + 100u); }
static void m_enc(uint8_t *b, size_t *l) { (void)l; put_be32(b + 96 + 20, 0x05000000u); }
static void m_trunc(uint8_t *b, size_t *l) { (void)b; *l -= 1; }

TEST(fw_patch_rejects_malformed)
{
    static const struct fwb_section overlap[2] = { { 0x1000u, 0x100u, 0, 0, 1 }, { 0x10ffu, 0x10u, 0, 0, 2 } };
    const struct fwb_section *d = fwb_default_sections;
    static struct mt792x_patch_info info;
    uint8_t small[95];
    memset(small, 0, sizeof(small));
    CHECK(mt792x_patch_parse(small, sizeof(small), &info) == MT792X_E_FW_TRUNCATED);
    CHECK(mt792x_patch_parse(NULL, 100, &info) == MT792X_E_INVAL);
    CHECK(patch_err(d, 3, NULL) == 0);
    CHECK(patch_err(d, 3, m_n0) == MT792X_E_FW_COUNT);
    CHECK(patch_err(d, 3, m_nbig) == MT792X_E_FW_COUNT);
    CHECK(patch_err(d, 3, m_ntable) == MT792X_E_FW_BOUNDS);
    CHECK(patch_err(d, 3, m_type) == MT792X_E_FW_SECTION_TYPE);
    CHECK(patch_err(d, 3, m_offs) == MT792X_E_FW_BOUNDS);
    CHECK(patch_err(d, 3, m_len) == MT792X_E_FW_BOUNDS);
    CHECK(patch_err(d, 3, m_offs_hdr) == MT792X_E_FW_BOUNDS);
    CHECK(patch_err(d, 3, m_len0) == MT792X_E_FW_LENGTH);
    CHECK(patch_err(d, 3, m_wrap) == MT792X_E_FW_LENGTH);
    CHECK(patch_err(d, 3, m_file_overlap) == MT792X_E_FW_OVERLAP);
    CHECK(patch_err(overlap, 2, NULL) == MT792X_E_FW_OVERLAP);   /* load ranges overlap */
    CHECK(patch_err(d, 3, m_enc) == MT792X_E_FW_ENC_TYPE);
    CHECK(patch_err(d, 3, m_trunc) == MT792X_E_FW_BOUNDS);
}

static int ram_err(const struct fwb_region *g, unsigned n, void (*mutate)(uint8_t *, size_t *))
{
    static struct mt792x_ram_info info;
    size_t len;
    int err;
    uint8_t *b = fwb_ram(g, n, &len);
    if (mutate) mutate(b, &len);
    err = mt792x_ram_parse(b, len, &info);
    free(b);
    return err;
}

/* region i of the default image starts at len - 36 - (4 - i) * 40 */
static uint8_t *reg_at(uint8_t *b, size_t l, unsigned i) { return b + l - 36u - (4u - i) * 40u; }
static void r_n0(uint8_t *b, size_t *l) { b[*l - 36 + 2] = 0; }
static void r_nbig(uint8_t *b, size_t *l) { b[*l - 36 + 2] = 33; }
static void r_ntable(uint8_t *b, size_t *l) { b[*l - 36 + 2] = 255; }
static void r_sum(uint8_t *b, size_t *l) { put_le32(reg_at(b, *l, 3) + 0x14, 20000u); }
static void r_len0(uint8_t *b, size_t *l) { put_le32(reg_at(b, *l, 3) + 0x14, 0); }
static void r_wrap(uint8_t *b, size_t *l) { put_le32(reg_at(b, *l, 3) + 0x10, 0xffffffc0u); }
static void r_override0(uint8_t *b, size_t *l) { put_le32(reg_at(b, *l, 2) + 0x10, 0); }
static void r_overlap(uint8_t *b, size_t *l) { put_le32(reg_at(b, *l, 3) + 0x10, 0x00901000u); }
static void r_all_nondl(uint8_t *b, size_t *l)
{
    unsigned i;
    for (i = 0; i < 4; i++) reg_at(b, *l, i)[0x18] |= 0x40u;
}

TEST(fw_ram_rejects_malformed)
{
    const struct fwb_region *d = fwb_default_regions;
    static struct mt792x_ram_info info;
    uint8_t small[35];
    memset(small, 0, sizeof(small));
    CHECK(mt792x_ram_parse(small, sizeof(small), &info) == MT792X_E_FW_TRUNCATED);
    CHECK(mt792x_ram_parse(small, 0, &info) == MT792X_E_FW_TRUNCATED);
    CHECK(ram_err(d, 4, NULL) == 0);
    CHECK(ram_err(d, 4, r_n0) == MT792X_E_FW_COUNT);
    CHECK(ram_err(d, 4, r_nbig) == MT792X_E_FW_COUNT);
    CHECK(ram_err(d, 4, r_ntable) == MT792X_E_FW_COUNT);
    CHECK(ram_err(d, 4, r_sum) == MT792X_E_FW_BOUNDS);
    CHECK(ram_err(d, 4, r_len0) == MT792X_E_FW_LENGTH);
    CHECK(ram_err(d, 4, r_wrap) == MT792X_E_FW_LENGTH);
    CHECK(ram_err(d, 4, r_override0) == MT792X_E_FW_OVERRIDE);
    CHECK(ram_err(d, 4, r_overlap) == MT792X_E_FW_OVERLAP);
    CHECK(ram_err(d, 4, r_all_nondl) == MT792X_E_FW_COUNT);
}

/* After a successful parse every range the bring-up will read must lie inside the buffer. */
static int patch_invariants(const struct mt792x_patch_info *p, size_t len)
{
    uint32_t i;
    if (p->n_sections == 0 || p->n_sections > MT792X_FW_MAX_SECTIONS) return 0;
    for (i = 0; i < p->n_sections; i++)
        if (p->sec[i].len == 0 || (uint64_t)p->sec[i].offs + p->sec[i].len > len) return 0;
    return 1;
}

static int ram_invariants(const struct mt792x_ram_info *r, size_t len)
{
    uint32_t i;
    if (r->n_regions == 0 || r->n_regions > MT792X_FW_MAX_REGIONS || r->data_len > len) return 0;
    for (i = 0; i < r->n_regions; i++)
        if ((uint64_t)r->reg[i].file_offset + r->reg[i].len > r->data_len) return 0;
    return 1;
}

TEST(fw_truncations_never_read_out_of_bounds)
{
    static struct mt792x_patch_info p;
    static struct mt792x_ram_info r;
    size_t pl, rl, n;
    uint8_t *pb = fwb_patch(fwb_default_sections, FWB_DEFAULT_SECTIONS, &pl);
    uint8_t *rb = fwb_ram(fwb_default_regions, FWB_DEFAULT_REGIONS, &rl);
    for (n = 0; n < pl; n++) {
        uint8_t *c = (uint8_t *)malloc(n ? n : 1u); /* exactly sized copy */
        memcpy(c, pb, n);
        CHECK(mt792x_patch_parse(c, n, &p) != 0);    /* every prefix loses data of the last section */
        free(c);
    }
    for (n = 0; n < rl; n++) {
        uint8_t *c = (uint8_t *)malloc(n ? n : 1u);
        memcpy(c, rb, n);
        if (mt792x_ram_parse(c, n, &r) == 0) CHECK(ram_invariants(&r, n));
        free(c);
    }
    free(pb);
    free(rb);
}

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

TEST(fw_fuzz_mutations)
{
    static struct mt792x_patch_info p;
    static struct mt792x_ram_info r;
    static const uint32_t extremes[] = { 0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffff0u, 96u, 160u };
    uint32_t seed = 12345u, it;
    size_t pl, rl;
    unsigned ok_p = 0, ok_r = 0;
    uint8_t *pb = fwb_patch(fwb_default_sections, FWB_DEFAULT_SECTIONS, &pl);
    uint8_t *rb = fwb_ram(fwb_default_regions, FWB_DEFAULT_REGIONS, &rl);

    for (it = 0; it < 4000u; it++) {
        int ram = (int)(it & 1u);
        size_t base_len = ram ? rl : pl, len = base_len;
        uint8_t *c = (uint8_t *)malloc(base_len);
        unsigned k, muts = 1u + lcg(&seed) % 6u;
        memcpy(c, ram ? rb : pb, base_len);
        for (k = 0; k < muts; k++) {
            uint32_t what = lcg(&seed) % 4u;
            if (len == 0) break;
            /* focus mutations on the headers/tables where they matter */
            size_t hot = ram ? (len > 200u ? len - 200u + lcg(&seed) % 200u : lcg(&seed) % len)
                             : (lcg(&seed) % (len < 300u ? len : 300u));
            if (what == 0) c[hot] ^= (uint8_t)(1u << (lcg(&seed) % 8u));
            else if (what == 1) c[hot] = (uint8_t)lcg(&seed);
            else if (what == 2 && hot + 4u <= len) {
                uint32_t v = extremes[lcg(&seed) % (sizeof(extremes) / sizeof(extremes[0]))];
                if (ram) put_le32(c + hot, v);
                else put_be32(c + hot, v);
            } else if (what == 3) {
                len = lcg(&seed) % (len + 1u); /* truncate (buffer stays exactly base_len; parser sees len) */
            }
        }
        if (len < base_len) {
            /* re-copy into an exactly sized buffer so a read past 'len' would leave the allocation */
            uint8_t *t = (uint8_t *)malloc(len ? len : 1u);
            memcpy(t, c, len);
            free(c);
            c = t;
        }
        if (ram) {
            if (mt792x_ram_parse(c, len, &r) == 0) {
                ok_r++;
                CHECK(ram_invariants(&r, len));
            }
        } else {
            if (mt792x_patch_parse(c, len, &p) == 0) {
                ok_p++;
                CHECK(patch_invariants(&p, len));
            }
        }
        free(c);
    }
    CHECK(ok_p > 0 && ok_r > 0); /* some mutations hit only data bytes and must still parse */
    free(pb);
    free(rb);
}
