/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Orderly shutdown (spec 7.3) and the proxy stop list (spec 7.2). */
#include "test.h"
#include "rig.h"

#include <stdio.h>
#include <string.h>

TEST(shutdown_after_each_stage)
{
    int stage;
    for (stage = MT792X_STAGE_OWNERSHIP; stage <= MT792X_STAGE_FIRMWARE; stage++) {
        struct rig r;
        uint32_t sd;
        unsigned resets;
        CHECK(rig_init(&r, 0) == 0);
        r.m.log_watch = "shutdown: done";
        CHECK(mt792x_bringup(r.dev, &r.fw, (enum mt792x_stage)stage) == 0);
        sd = r.m.trace_len;
        resets = r.f->wfsys_resets;
        mt792x_shutdown(r.dev);
        if (rig_check_shutdown(&r, sd, resets, 0x18451800u)) {
            printf("  ... after stage %d\n", stage);
            CHECK(0);
        }
        CHECK(r.dev->shutdown_error == 0);
        CHECK_EQ_U32(r.m.log_watch_hits, 1);
        CHECK(rig_check_no_violations(&r) == 0);
        CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
        CHECK(rig_check_set_own_only_in_s1(&r) == 0);
        rig_free(&r);
    }
}

TEST(shutdown_restores_any_saved_remap)
{
    struct rig r;
    uint32_t sd;
    CHECK(rig_init(&r, 0) == 0);
    mock_set_reg(&r.m, 0xfe24cu, 0x18457001u); /* vendor left the window on 0x7001xxxx */
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_RESET) == 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xfe24cu), 0x18451800u); /* last L1 access was the WFSYS register */
    sd = r.m.trace_len;
    mt792x_shutdown(r.dev);
    CHECK(rig_check_shutdown(&r, sd, 1, 0x18457001u) == 0);
    rig_free(&r);
}

TEST(shutdown_waits_50ms_in_wfsys_reset)
{
    struct rig r;
    uint64_t t0;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_RINGS) == 0);
    t0 = r.m.clock_us;
    mt792x_shutdown(r.dev);
    CHECK(r.m.clock_us - t0 >= 50000u + 12000u); /* 50 ms assert + INIT_DONE delay */
    CHECK(r.f->violations == 0);                 /* the fake flags a release earlier than 50 ms */
    rig_free(&r);
}

TEST(stop_ops_match_spec_7_2)
{
    static const struct mt792x_stop_op spec[10] = {
        { 1, 0xe0010u, 0x00000002u }, { 1, 0xd4204u, 0x00000000u }, { 3, 0xd4208u, 0x18208005u },
        { 3, 0xd4208u, 0x18208005u }, { 3, 0xd4100u, 0x00000030u }, { 2, 0xd4100u, 0x00000030u },
        { 3, 0xfe24cu, 0x0000ffffu }, { 2, 0xfe24cu, 0x00001800u }, { 3, 0x40140u, 0x00000001u },
        { 2, 0x40140u, 0x00000001u },
    };
    struct mt792x_stop_op ops[12];
    size_t i;
    memset(ops, 0xee, sizeof(ops));
    CHECK(mt792x_stop_ops(ops, 12) == 10u);
    for (i = 0; i < 10; i++) {
        CHECK_EQ_U32(ops[i].kind, spec[i].kind);
        CHECK_EQ_U32(ops[i].bar_offset, spec[i].bar_offset);
        CHECK_EQ_U32(ops[i].value, spec[i].value);
    }
    CHECK_EQ_U32(ops[10].kind, 0xee);            /* nothing written past the count */
    memset(ops, 0xee, sizeof(ops));
    CHECK(mt792x_stop_ops(ops, 3) == 10u);
    CHECK_EQ_U32(ops[2].bar_offset, 0xd4208u);
    CHECK_EQ_U32(ops[3].kind, 0xee);
    CHECK(mt792x_stop_ops(NULL, 0) == 10u);
    /* the Windows proxy takes at most 32 and uses the same kind numbering (1 write, 2 set, 3 clear) */
    CHECK(MT792X_STOP_OPS_COUNT <= 32u);
}

static void replay_and_check(struct rig *r, const char *when)
{
    unsigned resets = r->f->wfsys_resets;
    uint32_t glo;
    r->f->violations = 0;
    r->f->first_violation[0] = '\0';
    rig_replay_stop_ops(r);
    glo = fake_glo_cfg(r->f);
    if (glo & 0x5u) {
        printf("  stop list replay %s: GLO_CFG 0x%08x\n", when, glo);
        CHECK(0);
    }
    CHECK((mock_get_reg(&r->m, 0xd4100u) & 0x30u) == 0x30u);
    CHECK_EQ_U32(mock_get_reg(&r->m, 0xfe24cu), 0x18451800u);
    CHECK_EQ_U32(r->f->wfsys_resets, resets + 1u);
    CHECK(r->f->own_sync == 0);
    /* The proxy cannot wait 50 ms between rows 9 and 10 (spec 7.2, TODO-VERIFY): the fake reports
     * exactly that and nothing else. */
    CHECK_EQ_U32(r->f->violations, 1);
    CHECK(strstr(r->f->first_violation, "WFSYS reset released") != NULL);
}

TEST(stop_ops_replay_leaves_dma_disabled)
{
    int stage;
    for (stage = MT792X_STAGE_OWNERSHIP; stage <= MT792X_STAGE_FIRMWARE; stage++) {
        struct rig r;
        char when[32];
        CHECK(rig_init(&r, 0) == 0);
        CHECK(mt792x_bringup(r.dev, &r.fw, (enum mt792x_stage)stage) == 0);
        snprintf(when, sizeof(when), "after stage %d", stage);
        replay_and_check(&r, when);
        rig_free(&r);
    }
}

TEST(stop_ops_replay_after_failures)
{
    struct rig r;
    /* client "dies" waiting for a response, in the middle of the patch download */
    CHECK(rig_init(&r, 0) == 0);
    r.f->drop_response_nth = 4;
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == MT792X_E_MCU_TIMEOUT);
    replay_and_check(&r, "after a patch timeout");
    rig_free(&r);
    /* ... with the TX ring stalled ... */
    CHECK(rig_init(&r, 0) == 0);
    r.f->tx_stall_ring = 16;
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == MT792X_E_TX_TIMEOUT);
    replay_and_check(&r, "after a TX stall");
    rig_free(&r);
    /* ... and with the firmware running */
    CHECK(rig_init(&r, 0) == 0);
    r.f->n9_never = 1;
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == MT792X_E_N9_RDY);
    replay_and_check(&r, "after N9 timeout");
    rig_free(&r);
}
