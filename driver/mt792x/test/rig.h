/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rig.h - mock HAL + fake chip + library device + default synthetic firmware, for tests. */
#ifndef MT792X_TEST_RIG_H
#define MT792X_TEST_RIG_H

#include <stddef.h>
#include <stdint.h>

#include "fake_chip.h"
#include "fw_builder.h"
#include "hal_mock.h"
#include "mt792x/mt792x.h"

#define RIG_DMA_LEN (2u * 1024u * 1024u)

struct rig {
    struct mock m;
    struct fake_chip *f;
    struct mt792x *dev;
    uint8_t *patch, *ram;
    size_t patch_len, ram_len;
    struct mt792x_fw fw;
    int init_err;
};

/* dma_bus 0 = keep the mock default. Builds the default firmware and calls mt792x_init. */
int rig_init(struct rig *r, uint32_t dma_bus);
void rig_free(struct rig *r);
/* Replace the firmware images (takes ownership of the malloc'd buffers). */
void rig_set_fw(struct rig *r, uint8_t *patch, size_t patch_len, uint8_t *ram, size_t ram_len);

/* Checks shared by many tests; return the number of problems found (also printed). */
unsigned rig_check_no_violations(const struct rig *r);
unsigned rig_check_int_ena_never_nonzero(const struct rig *r);
/* SET_OWN (LPCTL write with bit 0) only before the first CLR_OWN, at most 10 times. */
unsigned rig_check_set_own_only_in_s1(const struct rig *r);
/* Shutdown state (spec 7.3) observed from trace index 'from': drv-own write first, DMA disabled,
 * WFDMA reset released, a WFSYS reset happened, remap restored to 'remap'. */
unsigned rig_check_shutdown(const struct rig *r, uint32_t from, unsigned wfsys_resets_before, uint32_t remap);

/* Bring-up to MT792X_STAGE_POSTBOOT, then (if channel != 0) mt792x_sniffer_start on that channel
 * (band derived from the number) with the given MT792X_SNIFF_* flags and the chip's MAC. */
int rig_postboot(struct rig *r);
int rig_sniff(struct rig *r, uint8_t channel, uint32_t flags);
/* Trace index of the drv-own write that starts the register part of a shutdown begun at 'from'. */
uint32_t rig_shutdown_regs_from(const struct rig *r, uint32_t from);

/* Replays mt792x_stop_ops through the raw HAL like the proxy does (set/clear = read-modify-write). */
void rig_replay_stop_ops(struct rig *r);

#endif /* MT792X_TEST_RIG_H */
