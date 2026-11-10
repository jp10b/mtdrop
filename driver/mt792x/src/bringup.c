/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * bringup.c - staged MT7921 firmware bring-up (steps S0-S23), orderly shutdown and the
 * "stop all DMA" replay list, for a polling host without interrupts.
 *
 * The sequence is derived from Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear):
 *   mt7921/pci.c (mt7921_pci_probe, mt7921_dma_init, mt7921e_unregister_device),
 *   mt7921/init.c, mt7921/pci_mcu.c, mt792x_core.c (fw/drv pmctrl, mt792x_load_firmware),
 *   mt792x_dma.c (dma enable/disable/cleanup, wfsys reset), mt76_connac_mcu.c (patch and RAM
 *   download), Copyright (C) 2020-2023 MediaTek Inc.
 * Step numbers and [HOST] deviations: docs/MT7921-BRINGUP.md sections 6 and 7.
 */
#include "internal.h"
#include "regs.h"

#define OWN_RETRY_COUNT     10u        /* MT792x_DRV_OWN_RETRY_COUNT */
#define OWN_POLL_US         50000u
#define OWN_TICK_US         1000u
#define ASPM_SLEEP_US       2500u      /* usleep_range(2000, 3000) */
#define WFSYS_ASSERT_US     50000u     /* msleep(50) */
#define WFSYS_POLL_US       500000u
#define TICK_10MS           10000u
#define DMA_BUSY_POLL_US    100000u
#define DRV_OWN2_POLL_US    500000u
#define FW_STATE_POLL_US    1000000u
#define N9_RDY_POLL_US      1500000u
#define EXPECTED_HW_REV_LO  0x10u      /* MEASURED on the target laptop (docs/FINDINGS.md F-004) */

static const char *const stage_names[] = { "?", "OWNERSHIP", "RESET", "RINGS", "MCU", "FIRMWARE", "POSTBOOT" };

static uint32_t ms_since(const struct mt792x *dev, uint64_t t0)
{
    return (uint32_t)((mt_now(dev) - t0) / 1000u);
}

int mt_step_fail(struct mt792x *dev, int step, int err, uint64_t t0)
{
    if (dev->bus_dead && err != MT792X_E_BUS_DEAD && err != MT792X_E_DMA_ADDR) err = MT792X_E_BUS_DEAD;
    dev->failed_step = step;
    mt_logf(dev, MT_LOG_ERROR, "S%d FAILED: %s (%d) after %u ms in this step", step, mt792x_strerror(err), err,
            ms_since(dev, t0));
    return err;
}

void mt_step_done(struct mt792x *dev, int step)
{
    dev->last_step = step;
}

#define fail mt_step_fail
#define done mt_step_done

/* ---- S1 / S2: ownership handshakes on MT_CONN_ON_LPCTL ---- */

static int fw_own(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev), el = 0;
    uint32_t i, v = 0;
    int rc = MT_POLL_TIMEOUT;

    for (i = 0; i < OWN_RETRY_COUNT; i++) {
        mt_wr(dev, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);
        rc = mt_poll(dev, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, PCIE_LPCR_HOST_OWN_SYNC, OWN_POLL_US,
                     OWN_TICK_US, &v, &el);
        if (rc != MT_POLL_TIMEOUT) break;
        mt_logf(dev, MT_LOG_WARN, "S1 fw-own attempt %u: OWN_SYNC still 0 (LPCTL 0x%08x)", i + 1u, v);
    }
    if (rc == MT792X_E_BUS_DEAD) return fail(dev, 1, MT792X_E_BUS_DEAD, t0);
    if (rc) {
        mt_logf(dev, MT_LOG_ERROR, "S1 fw-own: OWN_SYNC never set in %u attempts, LPCTL 0x%08x", OWN_RETRY_COUNT, v);
        return fail(dev, 1, MT792X_E_FW_OWN, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S1 fw-own: LPCTL 0x%08x (OWN_SYNC=1) at attempt %u, %u ms", v, i + 1u, ms_since(dev, t0));
    done(dev, 1);
    return 0;
}

/* Used by S2 and by shutdown (7.1 row 2). Returns 0, MT792X_E_BUS_DEAD or MT792X_E_DRV_OWN. */
static int drv_own(struct mt792x *dev, uint32_t *attempt, uint32_t *last)
{
    uint64_t el = 0;
    uint32_t i, v = 0;
    int rc = MT_POLL_TIMEOUT;

    for (i = 0; i < OWN_RETRY_COUNT; i++) {
        mt_wr(dev, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
        /* ASSUMPTION (spec 2.3): we cannot read aspm_supported, so always sleep as with ASPM. */
        if (!dev->opt.skip_aspm_sleep) mt_sleep(dev, ASPM_SLEEP_US);
        rc = mt_poll(dev, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, 0, OWN_POLL_US, OWN_TICK_US, &v, &el);
        if (rc != MT_POLL_TIMEOUT) break;
        mt_logf(dev, MT_LOG_WARN, "drv-own attempt %u: OWN_SYNC still 1 (LPCTL 0x%08x)", i + 1u, v);
    }
    if (attempt) *attempt = i + 1u;
    if (last) *last = v;
    if (rc == MT792X_E_BUS_DEAD) return MT792X_E_BUS_DEAD;
    return rc ? MT792X_E_DRV_OWN : 0;
}

static int step_drv_own(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev);
    uint32_t attempt = 0, v = 0;
    int err = drv_own(dev, &attempt, &v);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "S2 drv-own: OWN_SYNC never cleared in %u attempts, LPCTL 0x%08x", attempt, v);
        return fail(dev, 2, err, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S2 drv-own: LPCTL 0x%08x (OWN_SYNC=0) at attempt %u, %u ms", v, attempt, ms_since(dev, t0));
    done(dev, 2);
    return 0;
}

/* ---- S3: chip identification ---- */

static int chip_id(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev);
    uint32_t id = 0, bound = 0, rev = 0;
    int err = mt_l1_rr(dev, MT_HW_CHIPID_L1, &id);
    if (!err) err = mt_l1_rr(dev, MT_HW_BOUND_L1, &bound);
    if (!err) err = mt_l1_rr(dev, MT_HW_REV_L1, &rev);
    if (err) return fail(dev, 3, err, t0);

    dev->chip_id = id & 0xffffu; /* mt76 stores it in a u16 */
    dev->hw_bound = bound;
    dev->hw_rev = (dev->chip_id << 16) | (rev & 0xffu);
    mt_logf(dev, MT_LOG_INFO, "S3 chip: HW_CHIPID 0x%08x HW_BOUND 0x%08x HW_REV 0x%08x -> rev 0x%08x",
            id, bound, rev, dev->hw_rev);
    if (dev->chip_id != MT792X_CHIP_ID) {
        mt_logf(dev, MT_LOG_ERROR, "S3 chip id 0x%04x is not 0x%04x", dev->chip_id, MT792X_CHIP_ID);
        return fail(dev, 3, MT792X_E_CHIP_ID, t0);
    }
    if (bound & MT_HW_BOUND_MT7920) {
        mt_logf(dev, MT_LOG_ERROR, "S3 HW_BOUND bit 7 set: this is an MT7920, not supported");
        return fail(dev, 3, MT792X_E_CHIP_ID, t0);
    }
    if ((rev & 0xffu) != EXPECTED_HW_REV_LO) {
        mt_logf(dev, MT_LOG_WARN, "S3 HW_REV low byte 0x%02x differs from the measured 0x%02x", rev & 0xffu,
                EXPECTED_HW_REV_LO);
        if (dev->opt.strict_hw_rev) return fail(dev, 3, MT792X_E_CHIP_ID, t0);
    }
    done(dev, 3);
    return 0;
}

/* ---- S4: WFSYS reset (mt792x_wfsys_reset_default); also used by shutdown ---- */

static int wfsys_reset(struct mt792x *dev, uint32_t *last, uint64_t *el)
{
    int err = mt_l1_rmw(dev, MT_WFSYS_SW_RST_B_L1, WFSYS_SW_RST_B, 0);
    if (err) return err;
    mt_sleep(dev, WFSYS_ASSERT_US);
    err = mt_l1_rmw(dev, MT_WFSYS_SW_RST_B_L1, 0, WFSYS_SW_RST_B);
    if (err) return err;
    err = mt_poll_l1(dev, MT_WFSYS_SW_RST_B_L1, WFSYS_SW_INIT_DONE, WFSYS_SW_INIT_DONE, WFSYS_POLL_US, TICK_10MS,
                     last, el);
    if (err == MT_POLL_TIMEOUT) return MT792X_E_WFSYS_RESET;
    return err;
}

static int step_wfsys_reset(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev), el = 0;
    uint32_t v = 0;
    int err = wfsys_reset(dev, &v, &el);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "S4 WFSYS reset: INIT_DONE not set, 0x18000140 = 0x%08x after %u ms of polling", v,
                (uint32_t)(el / 1000u));
        return fail(dev, 4, err, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S4 WFSYS reset: INIT_DONE after %u ms (0x18000140 = 0x%08x)", (uint32_t)(el / 1000u), v);
    done(dev, 4);
    return 0;
}

/* ---- S5-S7 ---- */

static int step_irq_mask(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev);
    uint32_t v;
    mt_wr(dev, MT_WFDMA0_HOST_INT_ENA, 0);
    v = mt_rr(dev, MT_WFDMA0_HOST_INT_ENA);
    if (dev->bus_dead) return fail(dev, 5, MT792X_E_BUS_DEAD, t0);
    if (v != 0) {
        mt_logf(dev, MT_LOG_ERROR, "S5 HOST_INT_ENA reads 0x%08x after writing 0", v);
        return fail(dev, 5, MT792X_E_INT_MASK, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S5 HOST_INT_ENA = 0x%08x", v);
    done(dev, 5);

    t0 = mt_now(dev);
    if (dev->opt.skip_pcie_mac_int) {
        v = mt_rr(dev, MT_PCIE_MAC_INT_ENABLE);
        mt_logf(dev, MT_LOG_INFO, "S6 PCIE_MAC_INT_ENABLE left as found: 0x%08x (option)", v);
    } else {
        mt_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xffu);
        v = mt_rr(dev, MT_PCIE_MAC_INT_ENABLE);
        mt_logf(dev, MT_LOG_INFO, "S6 PCIE_MAC_INT_ENABLE = 0x%08x (mt76 writes 0xff; TODO-VERIFY R1)", v);
    }
    if (dev->bus_dead) return fail(dev, 6, MT792X_E_BUS_DEAD, t0);
    done(dev, 6);
    return 0;
}

static int step_dma_disable(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev), el = 0;
    uint32_t v = 0;
    int err = mt_rmw(dev, MT_WFDMA0_GLO_CFG, GLO_CFG_DISABLE_CLEAR, 0);
    if (err) return fail(dev, 7, err, t0);
    err = mt_poll(dev, MT_WFDMA0_GLO_CFG, GLO_CFG_BUSY_MASK, 0, DMA_BUSY_POLL_US, 1000u, &v, &el);
    if (err == MT_POLL_TIMEOUT) {
        mt_logf(dev, MT_LOG_ERROR, "S7 DMA still busy: GLO_CFG 0x%08x after %u ms", v, (uint32_t)(el / 1000u));
        return fail(dev, 7, MT792X_E_DMA_BUSY, t0);
    }
    if (err) return fail(dev, 7, err, t0);
    err = mt_rmw(dev, MT_WFDMA0_GLO_CFG_EXT0, MT_WFDMA0_CSR_TX_DMASHDL_EN, 0);
    if (!err) err = mt_rmw(dev, MT_DMASHDL_SW_CONTROL, 0, MT_DMASHDL_DMASHDL_BYPASS);
    if (!err) err = mt_rmw(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST, 0);
    if (!err) err = mt_rmw(dev, MT_WFDMA0_RST, 0, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
    if (err) return fail(dev, 7, err, t0);
    v = mt_rr(dev, MT_WFDMA0_GLO_CFG);
    if (dev->bus_dead) return fail(dev, 7, MT792X_E_BUS_DEAD, t0);
    if (v & (GLO_CFG_EN_MASK | GLO_CFG_BUSY_MASK)) {
        mt_logf(dev, MT_LOG_ERROR, "S7 GLO_CFG 0x%08x: DMA enable/busy bits still set", v);
        return fail(dev, 7, MT792X_E_DMA_BUSY, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S7 DMA disabled (force): GLO_CFG 0x%08x", v);
    done(dev, 7);
    return 0;
}

/* ---- S8-S10: rings and DMA enable ---- */

static int step_rings(struct mt792x *dev)
{
    static const int order[] = { MT792X_RING_TX0, MT792X_RING_TX17, MT792X_RING_TX16,
                                 MT792X_RING_RX0, MT792X_RING_RX4, MT792X_RING_RX2 };
    static const int fill_order[] = { MT792X_RING_RX2, MT792X_RING_RX0, MT792X_RING_RX4 };
    uint64_t t0 = mt_now(dev);
    unsigned i;
    int err;

    /* All ring memory (descriptors and RX buffers) is final before the first ring register write. */
    err = mt_rings_prepare_memory(dev);
    if (err) return fail(dev, 8, err, t0);
    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        err = mt_ring_program(dev, &dev->ring[order[i]]);
        if (err) return fail(dev, 8, err, t0);
        if (order[i] == MT792X_RING_TX0) mt_wr(dev, MT_WFDMA0_TX_RING_EXT_CTRL(0), 0x4u);
    }
    done(dev, 8);

    t0 = mt_now(dev);
    for (i = 0; i < sizeof(fill_order) / sizeof(fill_order[0]); i++) {
        struct mt792x_ring *r = &dev->ring[fill_order[i]];
        err = mt_ring_rx_kick_initial(dev, r);
        if (err) return fail(dev, 9, err, t0);
        mt_logf(dev, MT_LOG_INFO, "S9 RX ring %u: %u buffers armed, cpu_idx %u", r->hw_idx, r->queued, r->head);
    }
    done(dev, 9);
    return 0;
}

static int step_dma_enable(struct mt792x *dev)
{
    /* mt792x_dma_prefetch, non-mt7925 branch (spec 3.4), in source order */
    static const uint32_t prefetch[14][2] = {
        { 0xd4680u, MT_PREFETCH(0x000, 4) }, { 0xd4688u, MT_PREFETCH(0x040, 4) },
        { 0xd468cu, MT_PREFETCH(0x080, 4) }, { 0xd4690u, MT_PREFETCH(0x0c0, 4) },
        { 0xd4694u, MT_PREFETCH(0x100, 4) }, { 0xd4600u, MT_PREFETCH(0x140, 4) },
        { 0xd4604u, MT_PREFETCH(0x180, 4) }, { 0xd4608u, MT_PREFETCH(0x1c0, 4) },
        { 0xd460cu, MT_PREFETCH(0x200, 4) }, { 0xd4610u, MT_PREFETCH(0x240, 4) },
        { 0xd4614u, MT_PREFETCH(0x280, 4) }, { 0xd4618u, MT_PREFETCH(0x2c0, 4) },
        { 0xd4640u, MT_PREFETCH(0x340, 4) }, { 0xd4644u, MT_PREFETCH(0x380, 4) },
    };
    uint64_t t0 = mt_now(dev);
    uint32_t glo, dummy;
    unsigned i;
    int err;

    for (i = 0; i < 14; i++) mt_wr(dev, prefetch[i][0], prefetch[i][1]);
    mt_wr(dev, MT_WFDMA0_RST_DTX_PTR, 0xffffffffu);
    mt_wr(dev, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);
    err = mt_rmw(dev, MT_WFDMA0_GLO_CFG, 0, GLO_CFG_ENABLE_SET);
    if (!err) err = mt_rmw(dev, MT_WFDMA0_GLO_CFG, 0, GLO_CFG_EN_MASK);
    if (!err) err = mt_rmw(dev, MT_WFDMA_DUMMY_CR, 0, MT_WFDMA_NEED_REINIT);
    /* [HOST] mt76_connac_irq_enable is skipped: HOST_INT_ENA stays 0 (no ISR here, risk R1). */
    if (!err && !dev->opt.skip_mcu2host_sw_int) err = mt_rmw(dev, MT_MCU2HOST_SW_INT_ENA, 0, MT_MCU_CMD_WAKE_RX_PCIE);
    if (err) return fail(dev, 10, err, t0);
    glo = mt_rr(dev, MT_WFDMA0_GLO_CFG);
    dummy = mt_rr(dev, MT_WFDMA_DUMMY_CR);
    if (dev->bus_dead) return fail(dev, 10, MT792X_E_BUS_DEAD, t0);
    if ((glo & GLO_CFG_EN_MASK) != GLO_CFG_EN_MASK) {
        mt_logf(dev, MT_LOG_ERROR, "S10 GLO_CFG 0x%08x: TX/RX DMA enable did not stick", glo);
        return fail(dev, 10, MT792X_E_DMA_ENABLE, t0);
    }
    if (!(dummy & MT_WFDMA_NEED_REINIT))
        mt_logf(dev, MT_LOG_WARN, "S10 WFDMA_DUMMY_CR 0x%08x: NEED_REINIT bit did not stick", dummy);
    mt_logf(dev, MT_LOG_INFO, "S10 DMA enabled: GLO_CFG 0x%08x (expect 0x5030b875 from the measured state), DUMMY_CR 0x%08x",
            glo, dummy);
    done(dev, 10);
    return 0;
}

/* ---- S11-S13 ---- */

static int step_pre_mcu(struct mt792x *dev)
{
    uint64_t t0 = mt_now(dev), el = 0;
    uint32_t bar = 0, v = 0;
    int err;

    mt_wr(dev, MT_SWDEF_MODE, MT_SWDEF_NORMAL_MODE);
    if (dev->bus_dead) return fail(dev, 11, MT792X_E_BUS_DEAD, t0);
    mt_logf(dev, MT_LOG_INFO, "S11 SWDEF_MODE = normal (0)");
    done(dev, 11);

    /* mt7921e_driver_own: one remap, then write and poll the mapped offset directly. */
    t0 = mt_now(dev);
    err = mt_l1_map(dev, MT_TOP_LPCR_HOST_BAND0_L1, &bar);
    if (err) return fail(dev, 12, err, t0);
    mt_wr(dev, bar, MT_TOP_LPCR_HOST_DRV_OWN);
    err = mt_poll(dev, bar, MT_TOP_LPCR_HOST_FW_OWN, 0, DRV_OWN2_POLL_US, TICK_10MS, &v, &el);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "S12 driver own (TOP_LPCR_HOST_BAND0): reads 0x%08x after %u ms", v,
                (uint32_t)(el / 1000u));
        return fail(dev, 12, err == MT_POLL_TIMEOUT ? MT792X_E_DRV_OWN2 : err, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S12 driver own (TOP_LPCR_HOST_BAND0 @0x%05x): 0x%08x after %u ms", bar, v,
            (uint32_t)(el / 1000u));
    done(dev, 12);

    t0 = mt_now(dev);
    err = mt_rmw(dev, MT_PCIE_MAC_PM, 0, MT_PCIE_MAC_PM_L0S_DIS);
    v = mt_rr(dev, MT_PCIE_MAC_PM);
    if (err || dev->bus_dead) return fail(dev, 13, MT792X_E_BUS_DEAD, t0);
    mt_logf(dev, MT_LOG_INFO, "S13 PCIE_MAC_PM = 0x%08x (L0s disabled: %s)", v, (v & MT_PCIE_MAC_PM_L0S_DIS) ? "yes" : "NO");
    done(dev, 13);
    return 0;
}

/* ---- S14-S16 ---- */

static int is_fatal(int err)
{
    return err == MT792X_E_BUS_DEAD || err == MT792X_E_DMA_ADDR || err == MT792X_E_INVAL;
}

static int sem_ctrl(struct mt792x *dev, uint32_t op, int *result)
{
    uint8_t p[4];
    mt_put_le32(p, op);
    return mt_mcu_cmd(dev, MCU_CMD_PATCH_SEM_CONTROL, p, sizeof(p), 1, result);
}

static int sem_release(struct mt792x *dev, int step)
{
    uint64_t t0 = mt_now(dev);
    int result = -1;
    int err = sem_ctrl(dev, PATCH_SEM_RELEASE, &result);
    if (err) return fail(dev, step, err, t0);
    if (result != (int)PATCH_REL_SEM_SUCCESS) {
        mt_logf(dev, MT_LOG_ERROR, "S%d patch semaphore release: result %d (want 3 = PATCH_REL_SEM_SUCCESS)", step, result);
        return fail(dev, step, MT792X_E_SEM_RELEASE, t0);
    }
    dev->sem_held = 0;
    mt_logf(dev, MT_LOG_INFO, "S%d patch semaphore released (result 3), response on RX ring %u", step, dev->evt_ring);
    return 0;
}

static int step_mcu(struct mt792x *dev)
{
    static const uint8_t power_on[4] = { 1, 0, 0, 0 };
    uint64_t t0 = mt_now(dev), el = 0;
    uint32_t v = 0;
    int err, result = -1;

    err = mt_mcu_cmd(dev, MCU_CMD_NIC_POWER_CTRL, power_on, sizeof(power_on), 0, NULL);
    if (err) return fail(dev, 14, err, t0);
    mt_logf(dev, MT_LOG_INFO, "S14 NIC_POWER_CTRL sent (seq %u), TX17 consumed", dev->msg_seq & 0xfu);
    done(dev, 14);

    t0 = mt_now(dev);
    err = mt_poll(dev, MT_CONN_ON_MISC, MT_TOP_MISC_FW_STATE, MT_TOP_MISC2_FW_PWR_ON, FW_STATE_POLL_US, TICK_10MS, &v, &el);
    if (err == MT792X_E_BUS_DEAD) return fail(dev, 15, err, t0);
    if (err) {
        mt_logf(dev, dev->opt.lenient_fw_state ? MT_LOG_WARN : MT_LOG_ERROR,
                "S15 MCU is not ready for firmware download: CONN_ON_MISC 0x%08x after %u ms%s", v,
                (uint32_t)(el / 1000u), dev->opt.lenient_fw_state ? " (continuing, as mt76 does)" : "");
        if (!dev->opt.lenient_fw_state) return fail(dev, 15, MT792X_E_FW_STATE, t0);
    } else {
        mt_logf(dev, MT_LOG_INFO, "S15 CONN_ON_MISC 0x%08x (FW_STATE = 1, ready for download) after %u ms", v,
                (uint32_t)(el / 1000u));
    }
    done(dev, 15);

    t0 = mt_now(dev);
    err = sem_ctrl(dev, PATCH_SEM_GET, &result);
    if (err) return fail(dev, 16, err, t0);
    mt_logf(dev, MT_LOG_INFO, "S16 patch semaphore get: result %d, response on RX ring %u", result, dev->evt_ring);
    if (result == (int)PATCH_NOT_DL_SEM_SUCCESS) {
        dev->sem_held = 1;
    } else if (result == (int)PATCH_IS_DL) {
        dev->patch_was_loaded = 1;
        mt_logf(dev, MT_LOG_INFO, "S16 PATCH_IS_DL: ROM patch already loaded, semaphore not taken");
    } else {
        mt_logf(dev, MT_LOG_ERROR, "S16 failed to get patch semaphore (result %d)", result);
        return fail(dev, 16, MT792X_E_SEM_GET, t0);
    }
    done(dev, 16);
    return 0;
}

/* ---- S17-S23 ---- */

static int init_download(struct mt792x *dev, int step, uint8_t cmd, uint32_t addr, uint32_t len, uint32_t mode)
{
    uint8_t p[12];
    const uint8_t *e = dev->evt;
    int err;

    mt_put_le32(p + 0, addr);
    mt_put_le32(p + 4, len);
    mt_put_le32(p + 8, mode);
    err = mt_mcu_cmd(dev, cmd, p, sizeof(p), 1, NULL);
    if (err) return err;
    /* R16: mt76 does not check the payload of this response; log it for the human reader. */
    mt_logf(dev, MT_LOG_INFO, "S%d init download cmd 0x%02x addr 0x%08x len 0x%x mode 0x%08x: response ring %u, %u bytes, payload %08x",
            step, cmd, addr, len, mode, dev->evt_ring, dev->evt_len,
            dev->evt_len >= MT_MCU_RXD_LEN + 4u ? mt_get_le32(e + MT_MCU_RXD_LEN) : 0u);
    return 0;
}

static int load_patch(struct mt792x *dev, const uint8_t *file)
{
    static const uint8_t finish[4] = { 0, 0, 0, 0 };
    uint64_t t0 = mt_now(dev);
    uint32_t i;
    int err = 0, result = -1, step = 17;

    for (i = 0; i < dev->patch.n_sections && !err; i++) {
        const struct mt792x_patch_section *s = &dev->patch.sec[i];
        step = 17;
        err = init_download(dev, 17, s->cmd_id, s->addr, s->len, s->mode);
        if (err) break;
        step = 18;
        err = mt_mcu_send_firmware(dev, file + s->offs, s->len);
        if (!err)
            mt_logf(dev, MT_LOG_INFO, "S18 patch section %u: %u bytes sent in %u chunks", i, s->len,
                    (s->len + MT_FW_CHUNK_MAX - 1u) / MT_FW_CHUNK_MAX);
    }
    if (!err) {
        done(dev, 17);
        done(dev, 18);
        step = 19;
        err = mt_mcu_cmd(dev, MCU_CMD_PATCH_FINISH_REQ, finish, sizeof(finish), 1, &result);
        if (!err && result != 0) {
            mt_logf(dev, MT_LOG_ERROR, "S19 PATCH_FINISH_REQ result %d: failed to start patch", result);
            err = MT792X_E_PATCH_FINISH;
        }
        if (!err) {
            mt_logf(dev, MT_LOG_INFO, "S19 PATCH_FINISH_REQ: result 0");
            done(dev, 19);
        }
    }
    if (err) {
        int first = fail(dev, step, err, t0);
        /* mt76 always releases the semaphore after a download attempt; not on a dead bus. */
        if (!is_fatal(first) && !dev->bus_dead) {
            int rel = sem_release(dev, 20);
            if (rel) mt_logf(dev, MT_LOG_ERROR, "S20 release after the S%d failure also failed", step);
            dev->failed_step = step;
        }
        return first;
    }
    err = sem_release(dev, 20);
    if (err) return err;
    done(dev, 20);
    return 0;
}

static int load_ram(struct mt792x *dev, const uint8_t *file)
{
    uint64_t t0 = mt_now(dev), el = 0;
    uint32_t i, v = 0;
    uint8_t start[8];
    int err;

    for (i = 0; i < dev->ram.n_regions; i++) {
        const struct mt792x_ram_region *r = &dev->ram.reg[i];
        if (!r->download) {
            mt_logf(dev, MT_LOG_INFO, "S21 RAM region %u: NON_DL (type %u, %u bytes) skipped", i, r->type, r->len);
            continue;
        }
        err = init_download(dev, 21, r->cmd_id, r->addr, r->len, r->mode);
        if (!err) err = mt_mcu_send_firmware(dev, file + r->file_offset, r->len);
        if (err) return fail(dev, 21, err, t0);
        mt_logf(dev, MT_LOG_INFO, "S21 RAM region %u: %u bytes sent", i, r->len);
    }
    done(dev, 21);

    t0 = mt_now(dev);
    mt_put_le32(start + 0, dev->ram.fw_start_option);
    mt_put_le32(start + 4, dev->ram.override_addr);
    err = mt_mcu_cmd(dev, MCU_CMD_FW_START_REQ, start, sizeof(start), 1, NULL);
    if (err) return fail(dev, 22, err, t0);
    mt_logf(dev, MT_LOG_INFO, "S22 FW_START_REQ option 0x%x addr 0x%08x: response on RX ring %u (%u bytes)",
            dev->ram.fw_start_option, dev->ram.override_addr, dev->evt_ring, dev->evt_len);
    done(dev, 22);

    t0 = mt_now(dev);
    err = mt_poll(dev, MT_CONN_ON_MISC, MT_TOP_MISC2_FW_N9_RDY, MT_TOP_MISC2_FW_N9_RDY, N9_RDY_POLL_US, TICK_10MS, &v, &el);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "S23 timeout for initializing firmware: CONN_ON_MISC 0x%08x after %u ms", v,
                (uint32_t)(el / 1000u));
        return fail(dev, 23, err == MT_POLL_TIMEOUT ? MT792X_E_N9_RDY : err, t0);
    }
    mt_logf(dev, MT_LOG_INFO, "S23 firmware ready: CONN_ON_MISC 0x%08x (N9_RDY) after %u ms", v, (uint32_t)(el / 1000u));
    done(dev, 23);
    return 0;
}

static void snapshot(struct mt792x *dev, const char *what)
{
    uint32_t lpctl, misc, glo, ena, dummy;
    if (dev->bus_dead) return;
    lpctl = mt_rr(dev, MT_CONN_ON_LPCTL);
    misc = mt_rr(dev, MT_CONN_ON_MISC);
    glo = mt_rr(dev, MT_WFDMA0_GLO_CFG);
    ena = mt_rr(dev, MT_WFDMA0_HOST_INT_ENA);
    dummy = mt_rr(dev, MT_WFDMA_DUMMY_CR);
    mt_logf(dev, MT_LOG_INFO, "%s: LPCTL 0x%08x MISC 0x%08x GLO_CFG 0x%08x HOST_INT_ENA 0x%08x DUMMY_CR 0x%08x",
            what, lpctl, misc, glo, ena, dummy);
}

static int validate_fw(struct mt792x *dev, const struct mt792x_fw *fw)
{
    int err;
    uint32_t i;

    if (!fw) return 0;
    if (fw->patch) {
        err = mt792x_patch_parse(fw->patch, fw->patch_len, &dev->patch);
        if (err) {
            mt_logf(dev, MT_LOG_ERROR, "patch file rejected: %s (%d)", mt792x_strerror(err), err);
            return err;
        }
        mt_logf(dev, MT_LOG_INFO, "patch: build %s platform %s hw_sw_ver 0x%08x patch_ver 0x%08x, %u section(s), %u bytes",
                dev->patch.build_date, dev->patch.platform, dev->patch.hw_sw_ver, dev->patch.patch_ver,
                dev->patch.n_sections, dev->patch.total_dl_bytes);
        for (i = 0; i < dev->patch.n_sections; i++) {
            const struct mt792x_patch_section *s = &dev->patch.sec[i];
            mt_logf(dev, MT_LOG_INFO, "  section %u: type 0x%08x offs 0x%x addr 0x%08x len 0x%x sec 0x%08x mode 0x%08x cmd 0x%02x",
                    i, s->type, s->offs, s->addr, s->len, s->sec_key_idx, s->mode, s->cmd_id);
        }
    }
    if (fw->ram) {
        err = mt792x_ram_parse(fw->ram, fw->ram_len, &dev->ram);
        if (err) {
            mt_logf(dev, MT_LOG_ERROR, "RAM file rejected: %s (%d)", mt792x_strerror(err), err);
            return err;
        }
        mt_logf(dev, MT_LOG_INFO, "RAM: version %s build %s chip 0x%02x eco %u, %u region(s), %u bytes to download",
                dev->ram.fw_ver, dev->ram.build_date, dev->ram.chip_id, dev->ram.eco_code, dev->ram.n_regions,
                dev->ram.total_dl_bytes);
        mt_logf(dev, MT_LOG_INFO, "RAM: release-info feature byte 0x%02x (%s): CNM %s, NAN %s",
                dev->ram.fw_features,
                dev->ram.fw_features_status > 0 ? "found" : dev->ram.fw_features_status == 0 ? "no feature tag" : "block missing/invalid",
                (dev->ram.fw_features & MT792X_FW_CAP_CNM) ? "yes" : "no", (dev->ram.fw_features & MT792X_FW_CAP_NAN) ? "yes" : "no");
        for (i = 0; i < dev->ram.n_regions; i++) {
            const struct mt792x_ram_region *r = &dev->ram.reg[i];
            mt_logf(dev, MT_LOG_INFO, "  region %u: file 0x%x addr 0x%08x len 0x%x feature 0x%02x type %u%s mode 0x%08x cmd 0x%02x",
                    i, r->file_offset, r->addr, r->len, r->feature_set, r->type, r->download ? "" : " NON_DL", r->mode,
                    r->cmd_id);
        }
    }
    dev->fw_valid = fw->patch && fw->ram;
    return 0;
}

static int run(struct mt792x *dev, const struct mt792x_fw *fw, enum mt792x_stage until)
{
    uint64_t t0 = mt_now(dev);
    int err;

    /* S0, register part: remember the L1 remap register for the restore at shutdown. */
    dev->remap_saved = mt_rr(dev, MT_HIF_REMAP_L1);
    if (dev->bus_dead) return fail(dev, 0, MT792X_E_BUS_DEAD, t0);
    dev->remap_saved_valid = 1;
    mt_logf(dev, MT_LOG_INFO, "S0 MT_HIF_REMAP_L1 = 0x%08x saved; DMA region bus 0x%08x len 0x%x, layout 0x%x bytes",
            dev->remap_saved, dev->hal->dma_bus, dev->hal->dma_len, MT792X_LAYOUT_TOTAL);
    done(dev, 0);

    if ((err = fw_own(dev)) != 0) return err;
    if ((err = step_drv_own(dev)) != 0) return err;
    if ((err = chip_id(dev)) != 0) return err;
    if (until == MT792X_STAGE_OWNERSHIP) return 0;

    if ((err = step_wfsys_reset(dev)) != 0) return err;
    if ((err = step_irq_mask(dev)) != 0) return err;
    if ((err = step_dma_disable(dev)) != 0) return err;
    if (until == MT792X_STAGE_RESET) return 0;

    if ((err = step_rings(dev)) != 0) return err;
    if ((err = step_dma_enable(dev)) != 0) return err;
    if ((err = step_pre_mcu(dev)) != 0) return err;
    if (until == MT792X_STAGE_RINGS) return 0;

    if ((err = step_mcu(dev)) != 0) return err;
    if (until == MT792X_STAGE_MCU) {
        if (dev->sem_held) {
            err = sem_release(dev, 16);
            if (err) return err;
        }
        return 0;
    }

    if (dev->sem_held) {
        if ((err = load_patch(dev, fw->patch)) != 0) return err;
    } else {
        mt_logf(dev, MT_LOG_INFO, "S17-S20 skipped: ROM patch already loaded (PATCH_IS_DL)");
    }
    if ((err = load_ram(dev, fw->ram)) != 0) return err;
    if (until == MT792X_STAGE_FIRMWARE) return 0;

    return mt_postboot(dev);
}

int mt792x_init(struct mt792x *dev, struct mt_hal *hal)
{
    int err;
    if (!dev || !hal) return MT792X_E_INVAL;
    mt_memset(dev, 0, sizeof(*dev));
    dev->last_step = -1;
    dev->failed_step = -1;
    dev->sniff_failed_step = -1;
    if (!hal->rr || !hal->wr || !hal->sleep_us || !hal->now_us || !hal->dma_virt) return MT792X_E_INVAL;
    dev->hal = hal;
    err = mt_layout_init(dev);
    if (err) {
        dev->hal = NULL;
        return err;
    }
    dev->state = MT_ST_READY;
    return 0;
}

int mt792x_bringup(struct mt792x *dev, const struct mt792x_fw *fw, enum mt792x_stage until)
{
    uint64_t t0;
    int err;

    if (!dev) return MT792X_E_INVAL;
    if (dev->state != MT_ST_READY) return MT792X_E_STATE;
    if ((int)until < (int)MT792X_STAGE_OWNERSHIP || (int)until > (int)MT792X_STAGE_POSTBOOT) return MT792X_E_INVAL;
    if ((int)until >= (int)MT792X_STAGE_FIRMWARE && (!fw || !fw->patch || !fw->ram)) return MT792X_E_FW_MISSING;

    /* Untrusted files are validated completely before the first register access. */
    err = validate_fw(dev, fw);
    if (err) {
        dev->error = err;
        return err;
    }

    mt_logf(dev, MT_LOG_INFO, "bring-up to stage %d (%s) starts", (int)until, stage_names[until]);
    dev->state = MT_ST_TOUCHED;
    t0 = mt_now(dev);
    err = run(dev, fw, until);
    dev->error = err;
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "bring-up FAILED at S%d: %s; last completed step S%d; %u ms total",
                dev->failed_step, mt792x_strerror(err), dev->last_step, (uint32_t)((mt_now(dev) - t0) / 1000u));
    } else {
        snapshot(dev, "stage reached");
        if (dev->bus_dead) {
            dev->error = err = MT792X_E_BUS_DEAD;
            mt_logf(dev, MT_LOG_ERROR, "bring-up FAILED: bus dead while reading the final state");
            return err;
        }
        mt_logf(dev, MT_LOG_INFO, "bring-up reached stage %d (%s), last step S%d, %u ms total", (int)until,
                stage_names[until], dev->last_step, (uint32_t)((mt_now(dev) - t0) / 1000u));
    }
    return err;
}

/* ---- shutdown (spec 7.3) ---- */

static void note_shutdown(struct mt792x *dev, int err)
{
    if (!dev->shutdown_error) dev->shutdown_error = err;
}

void mt792x_shutdown(struct mt792x *dev)
{
    uint64_t t0, el = 0;
    uint32_t v = 0, attempt = 0;
    int err, i;

    if (!dev) return;
    if (dev->state == MT_ST_READY) {
        dev->state = MT_ST_SHUTDOWN; /* nothing was touched */
        return;
    }
    if (dev->state != MT_ST_TOUCHED) return;

    t0 = mt_now(dev);
    dev->in_shutdown = 1;
    if (dev->bus_dead) mt_logf(dev, MT_LOG_WARN, "shutdown: bus was dead during bring-up; trying anyway");
    mt_logf(dev, MT_LOG_INFO, "shutdown: start (last step S%d)", dev->last_step);
    /* 7.3 step 1: sniffer off, BSS/DEV off, radio off, MAC disable - only on a live bus. */
    mt_sniffer_shutdown(dev);
    dev->bus_dead = 0;

    /* 7.1 row 2: make sure the driver owns the chip (CLR_OWN only; never SET_OWN). */
    err = drv_own(dev, &attempt, &v);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "shutdown: drv-own failed (%s), LPCTL 0x%08x; continuing", mt792x_strerror(err), v);
        note_shutdown(dev, err);
    }

    /* 7.1 row 3: mt792x_dma_cleanup */
    dev->bus_dead = 0;
    err = mt_rmw(dev, MT_WFDMA0_GLO_CFG, GLO_CFG_DISABLE_CLEAR, 0);
    if (err) note_shutdown(dev, err);
    err = mt_poll(dev, MT_WFDMA0_GLO_CFG, GLO_CFG_BUSY_MASK, 0, DMA_BUSY_POLL_US, 1000u, &v, &el);
    if (err) {
        mt_logf(dev, MT_LOG_WARN, "shutdown: DMA busy poll %s, GLO_CFG 0x%08x (result ignored, as mt76)",
                err == MT_POLL_TIMEOUT ? "timed out" : "hit a dead bus", v);
        if (err == MT792X_E_BUS_DEAD) note_shutdown(dev, err);
    }
    dev->bus_dead = 0;
    err = mt_rmw(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST, 0);
    if (!err) err = mt_rmw(dev, MT_WFDMA0_RST, 0, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
    if (err) note_shutdown(dev, err);
    v = mt_rr(dev, MT_WFDMA0_GLO_CFG);
    mt_logf(dev, MT_LOG_INFO, "shutdown: DMA stopped, GLO_CFG 0x%08x", v);

    /* 7.1 row 4: WFSYS reset with the 50 ms assert and the INIT_DONE poll. */
    dev->bus_dead = 0;
    err = wfsys_reset(dev, &v, &el);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "shutdown: WFSYS reset: %s, 0x18000140 = 0x%08x", mt792x_strerror(err), v);
        note_shutdown(dev, err);
    } else {
        mt_logf(dev, MT_LOG_INFO, "shutdown: WFSYS reset done, INIT_DONE after %u ms", (uint32_t)(el / 1000u));
    }

    /* 7.3 step 3: restore the low 16 bits of the L1 remap register saved at S0. */
    dev->bus_dead = 0;
    if (dev->remap_saved_valid) {
        err = mt_rmw(dev, MT_HIF_REMAP_L1, MT_HIF_REMAP_L1_MASK, dev->remap_saved & MT_HIF_REMAP_L1_MASK);
        v = mt_rr(dev, MT_HIF_REMAP_L1);
        if (err || v != dev->remap_saved) {
            mt_logf(dev, MT_LOG_WARN, "shutdown: MT_HIF_REMAP_L1 reads 0x%08x, saved 0x%08x", v, dev->remap_saved);
            if (err) note_shutdown(dev, err);
        } else {
            mt_logf(dev, MT_LOG_INFO, "shutdown: MT_HIF_REMAP_L1 restored to 0x%08x", v);
        }
    }

    for (i = 0; i < MT792X_RING_COUNT; i++) {
        dev->ring[i].head = dev->ring[i].tail = dev->ring[i].queued = dev->ring[i].rearm_pending = 0;
        dev->ring[i].in_frag = 0;
    }
    dev->sem_held = 0;
    dev->sniff_started = 0;
    dev->rx_active = dev->sniffer_on = dev->bss_active = dev->dev_active = dev->radio_on = dev->mac_enabled = 0;
    dev->postboot_done = 0;
    dev->in_shutdown = 0;
    dev->state = MT_ST_SHUTDOWN;
    mt_logf(dev, MT_LOG_INFO, "shutdown: done in %u ms%s", (uint32_t)((mt_now(dev) - t0) / 1000u),
            dev->shutdown_error ? " (with errors)" : "");
}

/* ---- spec 7.2 stop list ---- */

size_t mt792x_stop_ops(struct mt792x_stop_op *ops, size_t max)
{
    static const struct mt792x_stop_op list[MT792X_STOP_OPS_COUNT] = {
        { MT792X_STOP_WRITE, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN },          /* driver ownership */
        { MT792X_STOP_WRITE, MT_WFDMA0_HOST_INT_ENA, 0 },                         /* mask WFDMA interrupts */
        { MT792X_STOP_CLEAR, MT_WFDMA0_GLO_CFG, GLO_CFG_DISABLE_CLEAR },          /* stop TX and RX DMA */
        { MT792X_STOP_CLEAR, MT_WFDMA0_GLO_CFG, GLO_CFG_DISABLE_CLEAR },          /* again (no busy poll possible) */
        { MT792X_STOP_CLEAR, MT_WFDMA0_RST, 0x30u },                              /* WFDMA logic + DMASHDL reset */
        { MT792X_STOP_SET, MT_WFDMA0_RST, 0x30u },                                /* ... release */
        { MT792X_STOP_CLEAR, MT_HIF_REMAP_L1, MT_HIF_REMAP_L1_MASK },             /* L1 window ... */
        { MT792X_STOP_SET, MT_HIF_REMAP_L1, 0x1800u },                            /* ... -> 0x1800xxxx */
        { MT792X_STOP_CLEAR, MT_HIF_REMAP_BASE_L1 + 0x0140u, WFSYS_SW_RST_B },    /* assert WFSYS reset */
        { MT792X_STOP_SET, MT_HIF_REMAP_BASE_L1 + 0x0140u, WFSYS_SW_RST_B },      /* release it */
    };
    size_t i;
    for (i = 0; ops && i < max && i < MT792X_STOP_OPS_COUNT; i++) ops[i] = list[i];
    return MT792X_STOP_OPS_COUNT;
}

/* ---- status ---- */

const char *mt792x_strerror(int err)
{
    switch (err) {
    case MT792X_OK: return "success";
    case MT792X_E_INVAL: return "invalid argument";
    case MT792X_E_STATE: return "not allowed in the current state";
    case MT792X_E_LAYOUT: return "DMA region cannot hold the ring layout";
    case MT792X_E_DMA_ADDR: return "bus address outside the DMA region refused";
    case MT792X_E_BUS_DEAD: return "bus dead (all-ones read or ring index out of range)";
    case MT792X_E_FW_OWN: return "firmware own handshake failed";
    case MT792X_E_DRV_OWN: return "driver own handshake failed";
    case MT792X_E_CHIP_ID: return "unexpected chip id";
    case MT792X_E_WFSYS_RESET: return "WFSYS reset: INIT_DONE timeout";
    case MT792X_E_INT_MASK: return "HOST_INT_ENA does not read back 0";
    case MT792X_E_DMA_BUSY: return "DMA busy did not clear";
    case MT792X_E_RING_PROGRAM: return "ring registers do not read back";
    case MT792X_E_DMA_ENABLE: return "DMA enable did not stick";
    case MT792X_E_DRV_OWN2: return "driver own (TOP_LPCR_HOST_BAND0) timeout";
    case MT792X_E_TX_TIMEOUT: return "TX descriptor not consumed by the chip";
    case MT792X_E_RING_FULL: return "TX ring full";
    case MT792X_E_MCU_TIMEOUT: return "MCU response timeout";
    case MT792X_E_MCU_EVENT: return "malformed MCU response";
    case MT792X_E_FW_STATE: return "MCU not ready for firmware download";
    case MT792X_E_SEM_GET: return "failed to get patch semaphore";
    case MT792X_E_SEM_RELEASE: return "failed to release patch semaphore";
    case MT792X_E_PATCH_FINISH: return "patch finish failed";
    case MT792X_E_N9_RDY: return "timeout for initializing firmware (N9_RDY)";
    case MT792X_E_FW_MISSING: return "firmware files required for this stage";
    case MT792X_E_FW_TRUNCATED: return "firmware file truncated";
    case MT792X_E_FW_COUNT: return "firmware section/region count invalid";
    case MT792X_E_FW_SECTION_TYPE: return "firmware patch section type invalid";
    case MT792X_E_FW_BOUNDS: return "firmware data outside the file";
    case MT792X_E_FW_OVERLAP: return "firmware ranges overlap";
    case MT792X_E_FW_LENGTH: return "firmware length zero or address wraps";
    case MT792X_E_FW_ENC_TYPE: return "unknown firmware encryption type";
    case MT792X_E_FW_OVERRIDE: return "firmware override address 0";
    case MT792X_E_MCU_STATUS: return "MCU command answered with an error status";
    case MT792X_E_NIC_CAPAB: return "NIC capability reply unusable";
    case MT792X_E_NO_MAC: return "no usable MAC address";
    case MT792X_E_FW_DEAD: return "firmware no longer ready";
    case MT792X_E_DMA_REINIT: return "WFDMA lost its configuration (NEED_REINIT cleared)";
    default: return "unknown error";
    }
}

int mt792x_last_step(const struct mt792x *dev) { return dev ? dev->last_step : -1; }
int mt792x_failed_step(const struct mt792x *dev) { return dev ? dev->failed_step : -1; }
uint32_t mt792x_chip_id(const struct mt792x *dev) { return dev ? dev->chip_id : 0; }
uint32_t mt792x_hw_rev(const struct mt792x *dev) { return dev ? dev->hw_rev : 0; }
int mt792x_patch_was_loaded(const struct mt792x *dev) { return dev ? dev->patch_was_loaded : 0; }

const struct mt792x_patch_info *mt792x_patch_info(const struct mt792x *dev)
{
    return (dev && dev->patch.n_sections) ? &dev->patch : NULL;
}

const struct mt792x_ram_info *mt792x_ram_info(const struct mt792x *dev)
{
    return (dev && dev->ram.n_regions) ? &dev->ram : NULL;
}
