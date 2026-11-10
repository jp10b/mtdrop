/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mt792x/mt792x.h - public entry point of the portable MT7921 chip logic.
 *
 * Staged firmware bring-up of an MT7921 (PCI 14c3:7961) from a polling,
 * interrupt-less host, following docs/MT7921-BRINGUP.md (steps S0-S23, which
 * mirror Linux mt76: openwrt/mt76 @ 0936617, BSD-3-Clause-Clear).
 *
 * Typical use:
 *
 *     static struct mt792x dev;                // ~6 KiB, caller-owned
 *     mt792x_init(&dev, hal);                  // layout + zero the DMA region, no register access
 *     err = mt792x_bringup(&dev, &fw, MT792X_STAGE_FIRMWARE);
 *     ...
 *     mt792x_shutdown(&dev);                   // always; safe after any outcome
 *
 * A host that can lose the process must also arrange for mt792x_stop_ops() to be
 * replayed when it dies (the Windows proxy does that on handle close).
 */
#ifndef MT792X_MT792X_H
#define MT792X_MT792X_H

#include <stddef.h>
#include <stdint.h>

#include "mt792x/fw.h"
#include "mt792x/hal.h"
#include "mt792x/layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns the library version as a static string, e.g. "0.1.0". */
const char *mt792x_version(void);

#define MT792X_CHIP_ID 0x7961u

/* How far mt792x_bringup goes; for gated hardware tests. */
enum mt792x_stage {
    MT792X_STAGE_OWNERSHIP = 1, /* S1-S3: fw-own/drv-own handshake, chip id check. No resets, no DMA. */
    MT792X_STAGE_RESET = 2,     /* + S4-S7: WFSYS reset, interrupts masked, DMA disabled */
    MT792X_STAGE_RINGS = 3,     /* + S8-S13: rings programmed and DMA enabled, no command sent */
    MT792X_STAGE_MCU = 4,       /* + S14-S16: NIC_POWER_CTRL, wait FW state, patch semaphore get (+ release) */
    MT792X_STAGE_FIRMWARE = 5   /* + S17-S23: patch + RAM download, firmware ready */
};

/* Error codes: 0 = success, negative = failure. */
enum mt792x_error {
    MT792X_OK = 0,
    MT792X_E_INVAL = -1,          /* bad argument */
    MT792X_E_STATE = -2,          /* call not allowed in the current state (bring-up runs once per init) */
    MT792X_E_LAYOUT = -3,         /* the HAL DMA region cannot hold the ring layout */
    MT792X_E_DMA_ADDR = -4,       /* a bus address outside the DMA region was requested (internal guard) */
    MT792X_E_BUS_DEAD = -5,       /* all-ones register read or ring index >= ring size */
    MT792X_E_FW_OWN = -6,         /* S1 */
    MT792X_E_DRV_OWN = -7,        /* S2 */
    MT792X_E_CHIP_ID = -8,        /* S3 */
    MT792X_E_WFSYS_RESET = -9,    /* S4 */
    MT792X_E_INT_MASK = -10,      /* S5: HOST_INT_ENA does not read back 0 */
    MT792X_E_DMA_BUSY = -11,      /* S7 */
    MT792X_E_RING_PROGRAM = -12,  /* S8: ring registers do not read back */
    MT792X_E_DMA_ENABLE = -13,    /* S10: GLO_CFG enable bits do not read back */
    MT792X_E_DRV_OWN2 = -14,      /* S12 */
    MT792X_E_TX_TIMEOUT = -15,    /* the chip did not consume a TX descriptor */
    MT792X_E_RING_FULL = -16,
    MT792X_E_MCU_TIMEOUT = -17,   /* no response event within 3 s */
    MT792X_E_MCU_EVENT = -18,     /* malformed response event */
    MT792X_E_FW_STATE = -19,      /* S15: MCU not ready for download */
    MT792X_E_SEM_GET = -20,       /* S16 */
    MT792X_E_SEM_RELEASE = -21,   /* S20 */
    MT792X_E_PATCH_FINISH = -22,  /* S19 */
    MT792X_E_N9_RDY = -23,        /* S23 */
    MT792X_E_FW_MISSING = -24,    /* stage needs firmware files that were not given */
    /* firmware file validation */
    MT792X_E_FW_TRUNCATED = -30,  /* file shorter than its fixed header/trailer */
    MT792X_E_FW_COUNT = -31,      /* section/region count zero, above the limit, or table outside the file */
    MT792X_E_FW_SECTION_TYPE = -32,
    MT792X_E_FW_BOUNDS = -33,     /* section/region data outside the file or inside the header/table */
    MT792X_E_FW_OVERLAP = -34,    /* file ranges or load-address ranges overlap */
    MT792X_E_FW_LENGTH = -35,     /* zero length or address + length wraps 32 bits */
    MT792X_E_FW_ENC_TYPE = -36,   /* unknown patch encryption type */
    MT792X_E_FW_OVERRIDE = -37    /* override flag with address 0 (cannot be expressed in FW_START) */
};

/* Behaviour switches for points the spec marks ASSUMPTION / TODO-VERIFY. All default to 0. */
struct mt792x_options {
    uint8_t lenient_fw_state;     /* S15 timeout: 0 = fail with MT792X_E_FW_STATE, 1 = warn and go on (mt76) */
    uint8_t skip_pcie_mac_int;    /* S6: 1 = do not write MT_PCIE_MAC_INT_ENABLE = 0xff (risk R1) */
    uint8_t skip_aspm_sleep;      /* S2: 1 = no 2.5 ms sleep after CLR_OWN (mt76 sleeps only with ASPM) */
    uint8_t skip_mcu2host_sw_int; /* S10: 1 = do not set MT_MCU2HOST_SW_INT_ENA bit 0 */
    uint8_t strict_hw_rev;        /* S3: 1 = fail unless HW_REV & 0xff == 0x10 (as measured) */
};

/* One ring: host-side state. */
struct mt792x_ring {
    uint32_t regs;      /* BAR offset of the ring register block */
    uint32_t ndesc;
    uint32_t desc_off;  /* descriptor array offset in the DMA region */
    uint32_t buf_off;   /* first slot buffer offset; meaningful only if buf_size != 0 */
    uint32_t buf_size;  /* bytes per slot buffer (0 = ring has no buffers) */
    uint32_t head;
    uint32_t tail;
    uint32_t queued;
    uint8_t hw_idx;
    uint8_t is_rx;
};

enum {
    MT792X_RING_TX0 = 0,
    MT792X_RING_TX16 = 1,
    MT792X_RING_TX17 = 2,
    MT792X_RING_RX0 = 3,
    MT792X_RING_RX4 = 4,
    MT792X_RING_RX2 = 5,
    MT792X_RING_COUNT = 6
};

/* Device state. Public so the caller can allocate it; treat the members as private. */
struct mt792x {
    struct mt_hal *hal;
    struct mt792x_options opt;
    int state;
    struct mt792x_ring ring[MT792X_RING_COUNT];

    uint32_t remap_saved;     /* MT_HIF_REMAP_L1 as found at S0 */
    int remap_saved_valid;
    int bus_dead;             /* sticky: no further register writes during bring-up */
    int in_shutdown;          /* mt792x_shutdown is running: writes allowed best-effort */

    uint32_t chip_id;
    uint32_t hw_bound;
    uint32_t hw_rev;          /* (chip_id << 16) | (HW_REV & 0xff), as mt76 computes it */
    int last_step;            /* highest step S<n> completed, -1 = none */
    int failed_step;          /* step that failed, -1 = none */
    int error;                /* result of the last mt792x_bringup */
    int shutdown_error;       /* 0, or the first problem seen by mt792x_shutdown */

    uint32_t msg_seq;         /* mt76 dev->mcu.msg_seq */
    int sem_held;             /* patch semaphore obtained (result 2) and not yet released */
    int patch_was_loaded;     /* semaphore answered PATCH_IS_DL */
    int fw_valid;
    struct mt792x_patch_info patch;
    struct mt792x_ram_info ram;

    uint8_t evt[MT792X_RX_BUF_SIZE]; /* last MCU event copied out of the RX ring */
    uint32_t evt_len;
    uint32_t evt_ring;        /* hardware RX ring the last response came from */
};

/* Firmware images in memory. Untrusted: they are validated completely before any register access. */
struct mt792x_fw {
    const uint8_t *patch;
    size_t patch_len;
    const uint8_t *ram;
    size_t ram_len;
};

/* S0: check the HAL, lay out and zero the DMA region. No register access. */
int mt792x_init(struct mt792x *dev, struct mt_hal *hal);

/* Run steps S0.. up to the end of 'until'. Once per mt792x_init. fw may be NULL below
 * MT792X_STAGE_FIRMWARE; if given it is validated first in every case. */
int mt792x_bringup(struct mt792x *dev, const struct mt792x_fw *fw, enum mt792x_stage until);

/* Spec 7.3: drv-own, stop DMA, WFSYS reset (with real polls and the 50 ms delay), restore the
 * L1 remap register. Safe at any point, idempotent. */
void mt792x_shutdown(struct mt792x *dev);

/* Spec 7.2: register operations a proxy replays to stop all DMA when the client dies. */
#define MT792X_STOP_WRITE 1u  /* register = value */
#define MT792X_STOP_SET   2u  /* register |= value */
#define MT792X_STOP_CLEAR 3u  /* register &= ~value */
#define MT792X_STOP_OPS_COUNT 10u
struct mt792x_stop_op {
    uint8_t kind;
    uint32_t bar_offset;
    uint32_t value;
};
/* Copies min(max, MT792X_STOP_OPS_COUNT) operations; returns MT792X_STOP_OPS_COUNT. */
size_t mt792x_stop_ops(struct mt792x_stop_op *ops, size_t max);

const char *mt792x_strerror(int err);

/* Read-only status. */
int mt792x_last_step(const struct mt792x *dev);
int mt792x_failed_step(const struct mt792x *dev);
uint32_t mt792x_chip_id(const struct mt792x *dev);
uint32_t mt792x_hw_rev(const struct mt792x *dev);
int mt792x_patch_was_loaded(const struct mt792x *dev);
const struct mt792x_patch_info *mt792x_patch_info(const struct mt792x *dev); /* NULL if no fw parsed */
const struct mt792x_ram_info *mt792x_ram_info(const struct mt792x *dev);     /* NULL if no fw parsed */

/* The one function that turns a region offset into a bus address for the chip.
 * Fails (MT792X_E_DMA_ADDR, logged, nothing written) if [offset, offset+len) is not inside the
 * region, len is 0, the address is not 'align'-aligned (align = power of two), or the region
 * would end above 4 GiB. Exposed for tests. */
int mt792x_dma_bus(const struct mt792x *dev, uint32_t offset, uint32_t len, uint32_t align, uint32_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* MT792X_MT792X_H */
