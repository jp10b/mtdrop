/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fault injection: every knob of the simulated chip yields its specific error, nothing that
 * enables DMA is written after the fault (except the semaphore release mt76 mandates), and
 * shutdown / the stop list still bring the chip to a safe state. */
#include "test.h"
#include "rig.h"

#include <stdio.h>
#include <string.h>

struct fault_case {
    const char *name;
    void (*setup)(struct rig *r);
    int err;
    int step;
    int allowed_enabling; /* DMA-enabling writes allowed after the fault point; -1 = not checked */
};

static void k_fw_own(struct rig *r) { r->f->fw_own_never = 1; }
static void k_drv_own(struct rig *r) { r->f->drv_own_never = 1; }
static void k_chip(struct rig *r) { r->f->chip_id = 0x7922u; }
static void k_bound(struct rig *r) { r->f->hw_bound = 0x80u; }
static void k_wfsys(struct rig *r) { r->f->wfsys_never_done = 1; }
static void k_busy(struct rig *r) { r->f->dma_busy_stuck = 1; }
static void k_drv_own2(struct rig *r) { r->f->drv_own2_never = 1; }
static void k_fw_state(struct rig *r) { r->f->fw_state_never_ready = 1; }
static void k_sem_fail(struct rig *r) { r->f->sem_get_result = 0; }
static void k_sem_weird(struct rig *r) { r->f->sem_get_result = 3; }
static void k_drop_sem(struct rig *r) { r->f->drop_response_nth = 2; }
static void k_drop_init(struct rig *r) { r->f->drop_response_nth = 3; }
static void k_drop_ram_init(struct rig *r) { r->f->drop_response_nth = 8; }
static void k_drop_start(struct rig *r) { r->f->drop_response_nth = 11; }
static void k_finish(struct rig *r) { r->f->finish_result = 1; }
static void k_release(struct rig *r) { r->f->sem_release_result = 0; }
static void k_n9(struct rig *r) { r->f->n9_never = 1; }
static void k_rx_unarmed(struct rig *r) { r->f->rx_never_armed = 1; }
static void k_stall17(struct rig *r) { r->f->tx_stall_ring = 17; }
static void k_stall16(struct rig *r) { r->f->tx_stall_ring = 16; }
static void k_short(struct rig *r) { r->f->short_response_nth = 3; }

static const struct fault_case cases[] = {
    { "fw-own never", k_fw_own, MT792X_E_FW_OWN, 1, 0 },
    { "drv-own never", k_drv_own, MT792X_E_DRV_OWN, 2, 0 },
    { "wrong chip id", k_chip, MT792X_E_CHIP_ID, 3, 0 },
    { "MT7920 bound bit", k_bound, MT792X_E_CHIP_ID, 3, 0 },
    { "WFSYS INIT_DONE never", k_wfsys, MT792X_E_WFSYS_RESET, 4, 0 },
    { "DMA busy stuck", k_busy, MT792X_E_DMA_BUSY, 7, 0 },
    { "driver own 2 never", k_drv_own2, MT792X_E_DRV_OWN2, 12, 0 },
    { "FW state never ready", k_fw_state, MT792X_E_FW_STATE, 15, 0 },
    { "semaphore fails", k_sem_fail, MT792X_E_SEM_GET, 16, 0 },
    { "semaphore odd result", k_sem_weird, MT792X_E_SEM_GET, 16, 0 },
    { "no response to semaphore", k_drop_sem, MT792X_E_MCU_TIMEOUT, 16, 0 },
    { "no response to patch init", k_drop_init, MT792X_E_MCU_TIMEOUT, 17, 1 },
    { "no response to RAM init", k_drop_ram_init, MT792X_E_MCU_TIMEOUT, 21, 0 },
    { "no response to FW_START", k_drop_start, MT792X_E_MCU_TIMEOUT, 22, 0 },
    { "patch finish result 1", k_finish, MT792X_E_PATCH_FINISH, 19, 1 },
    { "semaphore release fails", k_release, MT792X_E_SEM_RELEASE, 20, 0 },
    { "N9 never ready", k_n9, MT792X_E_N9_RDY, 23, 0 },
    { "RX ring not armed", k_rx_unarmed, MT792X_E_MCU_TIMEOUT, 16, -1 },
    { "TX17 never consumed", k_stall17, MT792X_E_TX_TIMEOUT, 14, 0 },
    { "TX16 never consumed", k_stall16, MT792X_E_TX_TIMEOUT, 18, 1 },
    { "33-byte init response", k_short, MT792X_E_MCU_EVENT, 17, 1 },
};

TEST(fault_33_byte_semaphore_response_is_enough)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->short_response_nth = 2; /* PATCH_SEM_CONTROL needs only 33 bytes (result at 0x20) */
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    mt792x_shutdown(r.dev);
    rig_free(&r);
}

TEST(fault_knobs)
{
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct fault_case *c = &cases[i];
        struct rig r;
        uint32_t sd;
        unsigned resets, after;
        int err;

        CHECK(rig_init(&r, 0) == 0);
        c->setup(&r);
        r.m.log_watch = "FAILED";
        err = mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE);
        if (err != c->err || mt792x_failed_step(r.dev) != c->step) {
            printf("  case '%s': err %d (%s) at S%d, expected %d at S%d\n", c->name, err, mt792x_strerror(err),
                   mt792x_failed_step(r.dev), c->err, c->step);
            CHECK(0);
        }
        CHECK(r.m.log_watch_hits >= 1);
        CHECK(mt792x_last_step(r.dev) < c->step);
        if (rig_check_no_violations(&r)) printf("  ... in case '%s'\n", c->name);
        CHECK(r.f->violations == 0);
        CHECK(r.f->fault_idx != 0);
        if (c->allowed_enabling >= 0) {
            after = fake_enabling_after(r.f, r.f->fault_idx);
            if (after != (unsigned)c->allowed_enabling) {
                printf("  case '%s': %u DMA-enabling writes after the fault, %d allowed\n", c->name, after,
                       c->allowed_enabling);
                CHECK(0);
            }
        }
        if (c->allowed_enabling == 1) {      /* the one allowed kick is the semaphore release */
            CHECK_EQ_U32(r.f->cmds[r.f->n_cmds - 1u].cid, 0x10);
            CHECK(r.f->sem_held == 0);
        }
        CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
        CHECK(rig_check_set_own_only_in_s1(&r) == 0);

        sd = r.m.trace_len;
        resets = r.f->wfsys_resets;
        r.f->wfsys_never_done = 0;           /* let the shutdown reset complete */
        r.f->dma_busy_stuck = 0;
        r.f->drv_own_never = 0;
        mt792x_shutdown(r.dev);
        if (rig_check_shutdown(&r, sd, resets, 0x18451800u)) {
            printf("  ... shutdown after case '%s'\n", c->name);
            CHECK(0);
        }
        CHECK(r.f->violations == 0);
        rig_free(&r);
    }
}

TEST(fault_fw_state_lenient_continues)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->fw_state_never_ready = 1;
    r.dev->opt.lenient_fw_state = 1;
    r.m.log_watch = "continuing, as mt76 does";
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    CHECK_EQ_U32(r.m.log_watch_hits, 1);
    CHECK(rig_check_no_violations(&r) == 0);
    mt792x_shutdown(r.dev);
    rig_free(&r);
}

TEST(fault_strict_hw_rev)
{
    struct rig r;
    CHECK(rig_init(&r, 0) == 0);
    r.f->hw_rev = 0x8a11u;
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_OWNERSHIP) == 0); /* warning only by default */
    mt792x_shutdown(r.dev);
    rig_free(&r);
    CHECK(rig_init(&r, 0) == 0);
    r.f->hw_rev = 0x8a11u;
    r.dev->opt.strict_hw_rev = 1;
    CHECK(mt792x_bringup(r.dev, NULL, MT792X_STAGE_OWNERSHIP) == MT792X_E_CHIP_ID);
    mt792x_shutdown(r.dev);
    rig_free(&r);
}

/* The bus dies after N register accesses, for N spread over a whole bring-up. */
TEST(fault_bus_dead_sweep)
{
    struct rig r;
    uint32_t total, n, stride;
    unsigned runs = 0;

    CHECK(rig_init(&r, 0) == 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE) == 0);
    total = r.f->accesses;
    rig_free(&r);
    CHECK(total > 100u);
    stride = total / 97u ? total / 97u : 1u;

    for (n = 1; n < total; n += stride) {
        int err;
        uint32_t sd;
        CHECK(rig_init(&r, 0) == 0);
        r.f->bus_dead_after = n;
        err = mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_FIRMWARE);
        if (err != MT792X_E_BUS_DEAD) {
            printf("  bus dead after %u accesses: err %d (%s) at S%d\n", n, err, mt792x_strerror(err),
                   mt792x_failed_step(r.dev));
            CHECK(0);
        }
        CHECK_EQ_U32(r.f->writes_after_dead_read, 0); /* nothing written once all-ones was seen */
        CHECK(r.f->violations == 0);
        sd = r.m.trace_len;
        mt792x_shutdown(r.dev);                       /* best effort, must terminate quickly */
        CHECK(r.dev->shutdown_error != 0);
        CHECK(r.m.trace_len - sd < 200u);
        rig_free(&r);
        runs++;
    }
    CHECK(runs >= 90u);
}
