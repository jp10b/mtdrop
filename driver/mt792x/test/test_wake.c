/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_wake.c - a chip that stops answering right after SET_OWN (docs/FINDINGS.md F-012). */
#include <stdio.h>

#include "rig.h"
#include "test.h"

TEST(drv_own_survives_a_sleeping_chip)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->sleep_after_set_own = 1;
    r.f->wake_us = 120000u;                       /* > 100 ms of all-ones, as measured */
    r.m.log_watch = "drv-own: chip answered again after";
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    CHECK_EQ_U32(r.m.log_watch_hits, 1);
    CHECK(r.f->reads_while_asleep >= 1u);
    CHECK_EQ_U32(r.f->writes_while_asleep, 0);    /* only CLR_OWN while it slept */
    CHECK(r.f->clr_own_requests >= 2u);
    CHECK_EQ_U32(mt792x_chip_id(r.dev), 0x7961u);
    CHECK(rig_check_set_own_only_in_s1(&r) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(drv_own_gives_up_on_a_chip_that_never_wakes)
{
    struct rig r;
    uint64_t t0;
    CHECK(rig_init(&r, 0) == 0);
    r.f->sleep_after_set_own = 1;
    r.f->wake_us = 0;
    t0 = r.m.clock_us;
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == MT792X_E_BUS_DEAD);
    CHECK(mt792x_failed_step(r.dev) == 2);
    CHECK_EQ_U32(r.f->writes_while_asleep, 0);
    CHECK_EQ_U32(r.f->clr_own_requests, 10u);     /* mt76's MT792x_DRV_OWN_RETRY_COUNT */
    CHECK(r.m.clock_us - t0 >= 450000u);          /* about mt76's 10 x 50 ms budget ... */
    CHECK(r.m.clock_us - t0 < 1000000u);          /* ... not more */
    mt792x_shutdown(r.dev);                       /* must terminate */
    CHECK(r.dev->shutdown_error != 0);
    rig_free(&r);
}

TEST(pcie_int_saved_at_s6_not_s0)
{
    struct rig r;
    uint32_t i, first_read = 0;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_OWNERSHIP) == 0);
    for (i = 0; i < r.m.trace_len; i++)
        if (r.m.trace[i].offset == 0x10188u) first_read = i + 1u;
    CHECK_EQ_U32(first_read, 0);                  /* S0-S3 do not touch 0x10188 */
    mt792x_shutdown(r.dev);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu);
    rig_free(&r);
}
