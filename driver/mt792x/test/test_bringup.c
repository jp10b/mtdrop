/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Staged bring-up against the simulated chip: each stage stops where it should. */
#include "test.h"
#include "rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned writes_outside(const struct rig *r, const uint32_t *allowed, unsigned n_allowed)
{
    uint32_t i;
    unsigned k, bad = 0;
    for (i = 0; i < r->m.trace_len; i++) {
        int ok = 0;
        if (!r->m.trace[i].is_write) continue;
        for (k = 0; k < n_allowed; k++) ok |= r->m.trace[i].offset == allowed[k];
        if (!ok) {
            if (!bad) printf("    unexpected write 0x%05x = 0x%08x\n", r->m.trace[i].offset, r->m.trace[i].value);
            bad++;
        }
    }
    return bad;
}

static unsigned count_writes_in(const struct rig *r, uint32_t lo, uint32_t hi)
{
    uint32_t i;
    unsigned n = 0;
    for (i = 0; i < r->m.trace_len; i++)
        if (r->m.trace[i].is_write && r->m.trace[i].offset >= lo && r->m.trace[i].offset < hi) n++;
    return n;
}

TEST(stage_ownership)
{
    static const uint32_t allowed[] = { 0xe0010u, 0xfe24cu };
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.m.log_watch = "S1 fw-own";
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_OWNERSHIP) == 0);
    CHECK(mt792x_last_step(r.dev) == 3);
    CHECK(mt792x_failed_step(r.dev) == -1);
    CHECK_EQ_U32(mt792x_chip_id(r.dev), 0x7961u);
    CHECK_EQ_U32(mt792x_hw_rev(r.dev), 0x79610010u);
    CHECK(r.f->own_sync == 0);                   /* driver owns the chip */
    CHECK_EQ_U32(r.f->set_own_writes, 1);
    CHECK_EQ_U32(r.f->wfsys_resets, 0);          /* no resets */
    CHECK(writes_outside(&r, allowed, 2) == 0);  /* only ownership + L1 window selection */
    CHECK(count_writes_in(&r, 0x40000u, 0x50000u) == 0);
    CHECK_EQ_U32(r.f->n_enabling, 0);
    CHECK_EQ_U32(r.m.log_watch_hits, 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(stage_reset)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.m.log_watch = "S4 WFSYS reset: INIT_DONE after 20 ms";
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_RESET) == 0);
    CHECK(mt792x_last_step(r.dev) == 7);
    CHECK_EQ_U32(r.m.log_watch_hits, 1);
    CHECK_EQ_U32(r.f->wfsys_resets, 1);
    CHECK_EQ_U32(fake_glo_cfg(r.f), 0x00103870u);      /* spec 3.7: measured 0x1010b870 after dma_disable */
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd4204u), 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0);          /* F-011: interrupts off (mt76 writes 0xff) */
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd4100u) & 0x30u, 0x30u);
    CHECK(mock_get_reg(&r.m, 0xd6004u) & 0x10000000u);
    CHECK(!(mock_get_reg(&r.m, 0xd42b0u) & 0x40u));
    CHECK(count_writes_in(&r, 0xd4300u, 0xd4600u) == 0); /* no ring register touched */
    CHECK_EQ_U32(r.f->n_enabling, 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(stage_rings)
{
    static const uint32_t prefetch[][2] = {
        { 0xd4680u, 0x00000004u }, { 0xd4688u, 0x00400004u }, { 0xd468cu, 0x00800004u }, { 0xd4690u, 0x00c00004u },
        { 0xd4694u, 0x01000004u }, { 0xd4600u, 0x01400004u }, { 0xd4604u, 0x01800004u }, { 0xd4608u, 0x01c00004u },
        { 0xd460cu, 0x02000004u }, { 0xd4610u, 0x02400004u }, { 0xd4614u, 0x02800004u }, { 0xd4618u, 0x02c00004u },
        { 0xd4640u, 0x03400004u }, { 0xd4644u, 0x03800004u },
    };
    static const uint32_t rings[][3] = { /* register block, size, is_rx */
        { 0xd4300u, MT792X_TX0_NDESC, 0 }, { 0xd4400u, MT792X_TX16_NDESC, 0 }, { 0xd4410u, MT792X_TX17_NDESC, 0 },
        { 0xd4500u, MT792X_RX0_NDESC, 1 }, { 0xd4520u, MT792X_RX2_NDESC, 1 }, { 0xd4540u, MT792X_RX4_NDESC, 1 },
    };
    struct rig r;
    unsigned i;
    int first_enable = -1, last_ring_write = -1;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_RINGS) == 0);
    CHECK(mt792x_last_step(r.dev) == 13);
    CHECK_EQ_U32(fake_glo_cfg(r.f), 0x5030b875u);      /* spec 3.7 expected value */
    for (i = 0; i < 14; i++) CHECK_EQ_U32(mock_get_reg(&r.m, prefetch[i][0]), prefetch[i][1]);
    CHECK_EQ_U32(mock_count_writes(&r.m, 0xd4600u), 2); /* 0x4 in S8, then the prefetch value */
    CHECK(mock_find_write(&r.m, 0xd420cu, 0) >= 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd420cu), 0xffffffffu);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd42f0u), 0);
    for (i = 0; i < 6; i++) {
        uint32_t base = mock_get_reg(&r.m, rings[i][0]);
        CHECK_EQ_U32(mock_get_reg(&r.m, rings[i][0] + 4u), rings[i][1]);
        CHECK(base >= r.m.hal.dma_bus && base + rings[i][1] * 16u <= r.m.hal.dma_bus + r.m.hal.dma_len);
        CHECK_EQ_U32(mock_get_reg(&r.m, rings[i][0] + 8u), rings[i][2] ? rings[i][1] - 1u : 0u);
    }
    CHECK_EQ_U32(r.f->rx_cpu[0], MT792X_RX0_NDESC - 1u);
    CHECK_EQ_U32(r.f->rx_cpu[4], MT792X_RX4_NDESC - 1u);
    CHECK_EQ_U32(r.f->rx_cpu[2], MT792X_RX2_NDESC - 1u);
    CHECK_EQ_U32(r.f->n_cmds, 0);                        /* no command sent */
    CHECK_EQ_U32(r.f->tx_kicks[17] + r.f->tx_kicks[16] + r.f->tx_kicks[0], 0);
    CHECK(mock_find_write(&r.m, 0x9f23cu, 0) >= 0 && mock_get_reg(&r.m, 0x9f23cu) == 0);
    CHECK(r.f->top_lpcr_fw_own == 0);
    CHECK(mock_get_reg(&r.m, 0x10194u) & 0x100u);
    CHECK(mock_get_reg(&r.m, 0x02120u) & 0x2u);
    CHECK(mock_get_reg(&r.m, 0xd41f4u) & 0x1u);
    /* every ring register write precedes the first GLO_CFG enable */
    for (i = 0; i < r.m.trace_len; i++) {
        const struct mock_access *a = &r.m.trace[i];
        if (!a->is_write) continue;
        if (a->offset == 0xd4208u && (a->value & 5u) && first_enable < 0) first_enable = (int)i;
        if (a->offset >= 0xd4300u && a->offset < 0xd4600u) last_ring_write = (int)i;
    }
    CHECK(first_enable > last_ring_write);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(stage_mcu_releases_semaphore)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_MCU) == 0);
    CHECK(mt792x_last_step(r.dev) == 16);
    CHECK_EQ_U32(r.f->n_cmds, 3);
    CHECK_EQ_U32(r.f->cmds[0].cid, 0x04);
    CHECK_EQ_U32(r.f->cmds[0].seq, 1);
    CHECK_EQ_U32(r.f->cmds[1].cid, 0x10);
    CHECK_EQ_U32(r.f->cmds[1].seq, 2);
    CHECK_EQ_U32(r.f->cmds[2].cid, 0x10);
    CHECK(r.f->sem_held == 0);                           /* nothing left held */
    CHECK(r.dev->sem_held == 0);
    CHECK_EQ_U32(r.f->n_dl, 0);
    CHECK_EQ_U32(fake_misc(r.f), 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(stage_mcu_patch_already_loaded)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->patch_loaded = 1;
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_MCU) == 0);
    CHECK_EQ_U32(r.f->n_cmds, 2);                        /* PATCH_IS_DL: no release */
    CHECK(mt792x_patch_was_loaded(r.dev) == 1);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

/* Expected command stream for the default firmware: ids and the ring they came on. */
static unsigned check_command_stream(const struct rig *r, int patch_loaded)
{
    uint8_t exp[128];
    unsigned n = 0, i, k, bad = 0;
    exp[n++] = 0x04;
    exp[n++] = 0x10;
    if (!patch_loaded) {
        for (i = 0; i < FWB_DEFAULT_SECTIONS; i++) {
            exp[n++] = fwb_default_section_cmd[i];
            for (k = 0; k < (fwb_default_sections[i].len + 4095u) / 4096u; k++) exp[n++] = 0xee;
        }
        exp[n++] = 0x07;
        exp[n++] = 0x10;
    }
    for (i = 0; i < FWB_DEFAULT_REGIONS; i++) {
        if (fwb_default_regions[i].feature_set & 0x40u) continue;
        exp[n++] = fwb_default_region_cmd[i];
        for (k = 0; k < (fwb_default_regions[i].len + 4095u) / 4096u; k++) exp[n++] = 0xee;
    }
    exp[n++] = 0x02;
    if (r->f->n_cmds != n) {
        printf("    %u commands, expected %u\n", r->f->n_cmds, n);
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (r->f->cmds[i].cid != exp[i] || r->f->cmds[i].ring != (exp[i] == 0xee ? 16u : 17u)) {
            printf("    command %u: cid 0x%02x ring %u, expected 0x%02x\n", i, r->f->cmds[i].cid, r->f->cmds[i].ring, exp[i]);
            bad++;
        }
    }
    return bad;
}

static unsigned check_downloads(const struct rig *r, int patch_loaded)
{
    unsigned i, j = 0, bad = 0;
    uint8_t *want;
    if (!patch_loaded) {
        for (i = 0; i < FWB_DEFAULT_SECTIONS; i++, j++) {
            const struct fake_dl *d = &r->f->dl[j];
            const struct fwb_section *s = &fwb_default_sections[i];
            want = (uint8_t *)malloc(s->len);
            fwb_pattern(want, s->len, s->seed);
            if (j >= r->f->n_dl || d->phase != 0 || d->addr != s->addr || d->len != s->len ||
                d->mode != fwb_default_section_mode[i] || d->cmd != fwb_default_section_cmd[i] || d->got != s->len ||
                memcmp(d->data, want, s->len) != 0) {
                printf("    patch section %u mismatch (addr 0x%08x len %u mode 0x%08x cmd 0x%02x got %u)\n", i, d->addr,
                       d->len, d->mode, d->cmd, d->got);
                bad++;
            }
            free(want);
        }
    }
    for (i = 0; i < FWB_DEFAULT_REGIONS; i++) {
        const struct fwb_region *g = &fwb_default_regions[i];
        const struct fake_dl *d = &r->f->dl[j];
        if (g->feature_set & 0x40u) continue;
        want = (uint8_t *)malloc(g->len);
        fwb_pattern(want, g->len, g->seed);
        if (j >= r->f->n_dl || d->phase != 1 || d->addr != g->addr || d->len != g->len ||
            d->mode != fwb_default_region_mode[i] || d->cmd != fwb_default_region_cmd[i] || d->got != g->len ||
            memcmp(d->data, want, g->len) != 0) {
            printf("    RAM region %u mismatch (addr 0x%08x len %u mode 0x%08x cmd 0x%02x got %u)\n", i, d->addr, d->len,
                   d->mode, d->cmd, d->got);
            bad++;
        }
        free(want);
        j++;
    }
    if (j != r->f->n_dl) {
        printf("    %u downloads, expected %u\n", r->f->n_dl, j);
        bad++;
    }
    return bad;
}

static void full_run(uint32_t bus, unsigned event_ring, unsigned fw_start_ring, int noise)
{
    struct rig r;
    uint32_t sd;
    unsigned resets;
    CHECK(rig_init(&r, bus) == 0);
    r.f->event_ring = event_ring;
    r.f->fw_start_ring = fw_start_ring;
    r.f->inject_unsolicited = noise;
    r.f->inject_stale = noise;
    r.f->nic_power_responds = noise;
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    CHECK(mt792x_last_step(r.dev) == 23);
    CHECK_EQ_U32(fake_misc(r.f), 3);
    CHECK_EQ_U32(r.f->fw_start_option, 1);
    CHECK_EQ_U32(r.f->fw_start_addr, 0x00915000u);
    CHECK_EQ_U32(r.dev->evt_ring, fw_start_ring);
    CHECK(r.f->sem_held == 0 && r.f->patch_done == 1);
    CHECK(check_command_stream(&r, 0) == 0);
    CHECK(check_downloads(&r, 0) == 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK(rig_check_set_own_only_in_s1(&r) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);
    CHECK(mt792x_patch_info(r.dev) != NULL && mt792x_ram_info(r.dev) != NULL);

    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    mt792x_shutdown(r.dev);
    CHECK(rig_check_shutdown(&r, sd, resets, 0x18451800u) == 0);
    CHECK(r.dev->shutdown_error == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    sd = r.m.trace_len;
    mt792x_shutdown(r.dev);                              /* idempotent */
    CHECK_EQ_U32(r.m.trace_len, sd);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_OWNERSHIP) == MT792X_E_STATE);
    rig_free(&r);
}

TEST(full_firmware_bringup)
{
    full_run(0, 0, 4, 0);
}

TEST(full_firmware_region_ending_at_4gib)
{
    full_run(0xffe00000u, 0, 4, 0);
}

TEST(full_firmware_responses_on_other_rings)
{
    full_run(0, 4, 0, 0);  /* every event on ring 4, FW_START on ring 0 */
    full_run(0, 0, 0, 0);
}

TEST(full_firmware_with_unsolicited_and_stale_events)
{
    full_run(0, 0, 4, 1);
}

TEST(full_firmware_patch_already_loaded)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->patch_loaded = 1;
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    CHECK(mt792x_patch_was_loaded(r.dev) == 1);
    CHECK(check_command_stream(&r, 1) == 0);
    CHECK(check_downloads(&r, 1) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    mt792x_shutdown(r.dev);
    rig_free(&r);
}

TEST(full_firmware_large_images_wrap_the_rings)
{
    /* 50 + 75 chunks: more than the 32-entry FWDL ring, and more than 15 sequence numbers */
    static const struct fwb_section s[1] = { { 0x00900000u, 200000u, 0, 0, 9 } };
    static const struct fwb_region g[2] = { { 0x00800000u, 300000u, 0x20, 0, 7 }, { 0x00100000u, 1u, 0, 0, 8 } };
    struct rig r;
    size_t pl, rl;
    uint8_t *p, *q, *want;
    const struct fake_dl *d;
    CHECK(rig_init(&r, 0) == 0);
    p = fwb_patch(s, 1, &pl);
    q = fwb_ram(g, 2, &rl);
    rig_set_fw(&r, p, pl, q, rl);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    d = fake_find_dl(r.f, 0x00900000u);
    want = (uint8_t *)malloc(300000u);
    CHECK(d && d->got == 200000u && d->chunks == 49u);
    fwb_pattern(want, 200000u, 9);
    CHECK(d && memcmp(d->data, want, 200000u) == 0);
    d = fake_find_dl(r.f, 0x00800000u);
    fwb_pattern(want, 300000u, 7);
    CHECK(d && d->got == 300000u && d->chunks == 74u && memcmp(d->data, want, 300000u) == 0);
    CHECK_EQ_U32(r.f->fw_start_addr, 0x00800000u);
    free(want);
    mt792x_shutdown(r.dev);
    rig_free(&r);
}

TEST(drv_own_retries)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->drv_own_fail_count = 3;
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_OWNERSHIP) == 0);
    CHECK_EQ_U32(r.f->clr_own_requests, 4);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(api_misuse_touches_nothing)
{
    struct rig r;
    struct mt792x_fw bad;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_FIRMWARE) == MT792X_E_FW_MISSING);
    CHECK(mt792x_bringup(r.dev, &r.fw, (enum mt792x_stage)0) == MT792X_E_INVAL);
    CHECK(mt792x_bringup(r.dev, &r.fw, (enum mt792x_stage)7) == MT792X_E_INVAL);
    CHECK(mt792x_bringup(NULL, &r.fw, MT792X_STAGE_OWNERSHIP) == MT792X_E_INVAL);
    bad = r.fw;
    bad.patch_len = 95;                                  /* invalid file, even for stage 1 */
    CHECK(mt792x_bringup(r.dev, &bad, MT792X_STAGE_OWNERSHIP) == MT792X_E_FW_TRUNCATED);
    bad = r.fw;
    bad.ram_len = 35;
    CHECK(mt792x_bringup(r.dev, &bad, MT792X_STAGE_FIRMWARE) == MT792X_E_FW_TRUNCATED);
    CHECK_EQ_U32(r.m.trace_len, 0);                      /* no register access at all */
    mt792x_shutdown(r.dev);                              /* nothing touched: no access either */
    CHECK_EQ_U32(r.m.trace_len, 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_OWNERSHIP) == MT792X_E_STATE);
    mt792x_shutdown(NULL);
    rig_free(&r);
}
