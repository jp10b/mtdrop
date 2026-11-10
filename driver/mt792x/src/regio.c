/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * regio.c - register access: plain BAR access, the L1 remap window, time-based polls.
 *
 * L1 remap procedure derived from mt7921_reg_map_l1() (mt7921/mt7921.h) and the poll
 * helpers from util.c of Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear,
 * Copyright (C) 2020 MediaTek Inc., Copyright (C) 2016 Felix Fietkau).
 *
 * Bus-dead policy: a register read returning 0xffffffff marks the device bus-dead
 * (sticky). Afterwards bring-up writes are suppressed; read-modify-write never writes
 * a value derived from an all-ones read. Only mt792x_shutdown may write again.
 */
#include "internal.h"
#include "regs.h"

uint64_t mt_now(const struct mt792x *dev)
{
    return dev->hal->now_us(dev->hal->ctx);
}

void mt_sleep(const struct mt792x *dev, uint32_t us)
{
    dev->hal->sleep_us(dev->hal->ctx, us);
}

uint32_t mt_rr(struct mt792x *dev, uint32_t bar)
{
    uint32_t v = dev->hal->rr(dev->hal->ctx, bar);
    if (v == 0xffffffffu) {
        if (!dev->bus_dead) mt_logf(dev, MT_LOG_ERROR, "bus dead: register 0x%05x read 0xffffffff", bar);
        dev->bus_dead = 1;
    }
    return v;
}

void mt_wr(struct mt792x *dev, uint32_t bar, uint32_t value)
{
    if (dev->bus_dead && !dev->in_shutdown) {
        mt_logf(dev, MT_LOG_DEBUG, "write 0x%05x = 0x%08x suppressed (bus dead)", bar, value);
        return;
    }
    dev->hal->wr(dev->hal->ctx, bar, value);
}

int mt_rmw(struct mt792x *dev, uint32_t bar, uint32_t clear, uint32_t set)
{
    uint32_t v = mt_rr(dev, bar);
    if (v == 0xffffffffu) return MT792X_E_BUS_DEAD;
    mt_wr(dev, bar, (v & ~clear) | set);
    return 0;
}

static int l1_logical_ok(uint32_t a)
{
    return (a >= 0x18000000u && a < 0x18c00000u) || (a >= 0x70000000u && a < 0x78000000u) ||
           (a >= 0x7c000000u && a < 0x7c400000u);
}

/* The only code that writes MT_HIF_REMAP_L1 during bring-up (shutdown restores it in bringup.c
 * through mt_rmw on the same register). Bits 31:16 are preserved, as mt76 does. */
int mt_l1_map(struct mt792x *dev, uint32_t logical, uint32_t *bar)
{
    uint32_t v;
    if (!l1_logical_ok(logical)) {
        mt_logf(dev, MT_LOG_ERROR, "internal: 0x%08x is not an L1 address", logical);
        return MT792X_E_INVAL;
    }
    v = mt_rr(dev, MT_HIF_REMAP_L1);
    if (v == 0xffffffffu) return MT792X_E_BUS_DEAD;
    mt_wr(dev, MT_HIF_REMAP_L1, (v & ~MT_HIF_REMAP_L1_MASK) | (logical >> 16));
    /* "use read to push write" */
    if (mt_rr(dev, MT_HIF_REMAP_L1) == 0xffffffffu) return MT792X_E_BUS_DEAD;
    *bar = MT_HIF_REMAP_BASE_L1 + (logical & 0xffffu);
    return 0;
}

int mt_l1_rr(struct mt792x *dev, uint32_t logical, uint32_t *value)
{
    uint32_t bar;
    int err = mt_l1_map(dev, logical, &bar);
    if (err) return err;
    *value = mt_rr(dev, bar);
    return *value == 0xffffffffu ? MT792X_E_BUS_DEAD : 0;
}

/* One remap, then read + write at the mapped offset (mt7921_rmw translates once). */
int mt_l1_rmw(struct mt792x *dev, uint32_t logical, uint32_t clear, uint32_t set)
{
    uint32_t bar;
    int err = mt_l1_map(dev, logical, &bar);
    if (err) return err;
    return mt_rmw(dev, bar, clear, set);
}

static int poll_common(struct mt792x *dev, int l1, uint32_t addr, uint32_t mask, uint32_t val,
                       uint32_t timeout_us, uint32_t tick_us, uint32_t *last, uint64_t *elapsed_us)
{
    uint64_t start = mt_now(dev);
    uint64_t el = 0;
    uint32_t v = 0;
    int rc;

    for (;;) {
        if (l1) {
            rc = mt_l1_rr(dev, addr, &v);
            if (rc) {
                if (rc == MT792X_E_BUS_DEAD) v = 0xffffffffu;
                break;
            }
        } else {
            v = mt_rr(dev, addr);
            if (v == 0xffffffffu) {
                rc = MT792X_E_BUS_DEAD;
                break;
            }
        }
        el = mt_now(dev) - start;
        if ((v & mask) == val) {
            rc = 0;
            break;
        }
        if (el >= timeout_us) {
            rc = MT_POLL_TIMEOUT;
            break;
        }
        mt_sleep(dev, tick_us);
    }
    if (last) *last = v;
    if (elapsed_us) *elapsed_us = mt_now(dev) - start;
    return rc;
}

int mt_poll(struct mt792x *dev, uint32_t bar, uint32_t mask, uint32_t val,
            uint32_t timeout_us, uint32_t tick_us, uint32_t *last, uint64_t *elapsed_us)
{
    return poll_common(dev, 0, bar, mask, val, timeout_us, tick_us, last, elapsed_us);
}

int mt_poll_l1(struct mt792x *dev, uint32_t logical, uint32_t mask, uint32_t val,
               uint32_t timeout_us, uint32_t tick_us, uint32_t *last, uint64_t *elapsed_us)
{
    return poll_common(dev, 1, logical, mask, val, timeout_us, tick_us, last, elapsed_us);
}
