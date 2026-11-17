/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_irq.c - interrupts stay off at the source on a host without an interrupt handler
 * (risk R1, docs/FINDINGS.md F-011). */
#include <stdio.h>

#include "rig.h"
#include "test.h"

static unsigned nonzero_writes(const struct rig *r, uint32_t offset)
{
    uint32_t i;
    unsigned n = 0;
    for (i = 0; i < r->m.trace_len; i++)
        if (r->m.trace[i].is_write && r->m.trace[i].offset == offset && r->m.trace[i].value != 0) n++;
    return n;
}

TEST(irq_pcie_int_written_zero_by_default)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu); /* as the vendor driver leaves it */
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_RINGS) == 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0);
    CHECK_EQ_U32(nonzero_writes(&r, 0x10188u), 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(irq_pcie_int_mt76_option_keeps_old_behaviour)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.pcie_mac_int = MT792X_PCIE_INT_MT76;
    CHECK(rig_postboot(&r) == 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu);       /* the guard leaves it alone in this mode */
    CHECK_EQ_U32(mt792x_irq_counters(r.dev)->pcie_int_fixed, 0);
    mt792x_shutdown(r.dev);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu);       /* back to the value found at S0 */
    rig_free(&r);
}

TEST(irq_guard_clears_what_the_firmware_enables)
{
    struct rig r;
    const struct mt792x_irq_counters *c;
    CHECK(rig_init(&r, 0) == 0);
    CHECK_EQ_U32(r.f->fw_int_ena, 0x20000000u);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    c = mt792x_irq_counters(r.dev);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd4204u), 0);
    CHECK(c->checks >= 3u);                                   /* S23, post-boot, sniffer start */
    CHECK_EQ_U32(c->host_int_ena_fixed, 1);                   /* set once at FW_START, cleared at S23 */
    CHECK_EQ_U32(c->host_int_ena_last, 0x20000000u);
    CHECK(c->host_int_sta_seen & 0x20000000u);                /* the pending cause is reported, not cleared */
    CHECK_EQ_U32(c->pcie_int_fixed, 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK_EQ_U32(nonzero_writes(&r, 0x10188u), 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(irq_guard_runs_while_receiving)
{
    struct rig r;
    struct mt792x_rx_frame fr;
    uint32_t fixed_before, i;
    CHECK(rig_init(&r, 0) == 0);
    r.f->fw_int_rearm_us = 400000u;                           /* the firmware re-enables every 400 ms */
    CHECK(rig_sniff(&r, 6, 0) == 0);
    fixed_before = mt792x_irq_counters(r.dev)->host_int_ena_fixed;
    for (i = 0; i < 40u; i++) {                               /* 4 s of polling, 100 ms apart */
        CHECK(mt792x_rx_poll(r.dev, &fr) >= 0);
        r.m.clock_us += 100000u;
    }
    /* liveness every 250 ms, polls 100 ms apart -> a check every 300 ms; re-arm 400 ms after each set */
    CHECK(mt792x_irq_counters(r.dev)->host_int_ena_fixed >= fixed_before + 5u);
    CHECK(mt792x_irq_counters(r.dev)->checks >= 16u);         /* liveness every 250 ms */
    /* never left enabled for longer than one liveness period: the last poll cleared it or it was not re-armed */
    mt792x_rx_poll(r.dev, &fr);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd4204u), 0);
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    rig_free(&r);
}

TEST(irq_guard_puts_back_pcie_int)
{
    struct rig r;
    struct mt792x_rx_frame fr;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    mock_set_reg(&r.m, 0x10188u, 0x3u);                        /* something turned it back on */
    r.m.clock_us += 300000u;
    CHECK(mt792x_rx_poll(r.dev, &fr) >= 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0);
    CHECK_EQ_U32(mt792x_irq_counters(r.dev)->pcie_int_fixed, 1);
    CHECK_EQ_U32(mt792x_irq_counters(r.dev)->pcie_int_last, 0x3u);
    rig_free(&r);
}

TEST(irq_shutdown_turns_both_off_first)
{
    struct rig r;
    uint32_t from, i, ena_at = 0, pcie_at = 0, glo_at = 0;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    from = r.m.trace_len;
    mt792x_shutdown(r.dev);
    for (i = from; i < r.m.trace_len; i++) {
        const struct mock_access *a = &r.m.trace[i];
        if (!a->is_write) continue;
        if (a->offset == 0xd4204u && !ena_at) ena_at = i;
        if (a->offset == 0x10188u && !pcie_at) pcie_at = i;
        if (a->offset == 0xd4208u && !glo_at && i > rig_shutdown_regs_from(&r, from)) glo_at = i;
    }
    CHECK(ena_at && pcie_at && glo_at);
    CHECK(ena_at < glo_at && pcie_at < glo_at);               /* before the DMA is stopped */
    CHECK_EQ_U32(r.m.trace[pcie_at].value, 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd4204u), 0);
    /* handed back as found (0xff) only after the WFSYS reset: the last 0x10188 write follows the last
     * write of the WFSYS reset register (L1 window offset 0x40140) */
    {
        uint32_t last_pcie = 0, last_wfsys = 0;
        for (i = from; i < r.m.trace_len; i++) {
            if (!r.m.trace[i].is_write) continue;
            if (r.m.trace[i].offset == 0x10188u) last_pcie = i;
            if (r.m.trace[i].offset == 0x40140u) last_wfsys = i;
        }
        CHECK(last_wfsys && last_pcie > last_wfsys);
        CHECK_EQ_U32(r.m.trace[last_pcie].value, 0xffu);
    }
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(irq_stop_list_turns_pcie_int_off)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.dev->opt.pcie_mac_int = MT792X_PCIE_INT_MT76;
    CHECK(rig_postboot(&r) == 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu);
    mock_set_reg(&r.m, 0xd4204u, 0x20000000u);                /* as the firmware leaves it */
    rig_replay_stop_ops(&r);                                  /* the client died: only the proxy acts */
    /* interrupts off (both gates), DMA stopped, WFSYS reset; then the PCIe gate back to 0xff */
    CHECK_EQ_U32(mock_get_reg(&r.m, 0xd4204u), 0);
    CHECK_EQ_U32(mock_get_reg(&r.m, 0x10188u), 0xffu);
    CHECK(!(fake_glo_cfg(r.f) & 0x5u));
    rig_free(&r);
}
