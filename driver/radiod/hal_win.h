/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows backend of the mt792x HAL: registers through the mtdrop UMDF proxy,
 * DMA memory from one locked 2 MiB physical block below 4 GiB. */
#ifndef MTDROP_HAL_WIN_H
#define MTDROP_HAL_WIN_H

#include <stdint.h>
#include "mt792x/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALWIN_DMA_BLOCK_BYTES (2u * 1024u * 1024u)

struct halwin_close_op {
    uint32_t offset; /* BAR0 offset */
    uint32_t value;
    uint8_t kind;    /* 1 = write value, 2 = set bits, 3 = clear bits, 4 = wait `value` ms (offset unused) */
};

struct halwin;

/* Allocates the DMA block only (no device needed). Returns NULL and writes a reason on failure.
 * Requires SeLockMemoryPrivilege, which in practice means running as SYSTEM (docs/FINDINGS.md F-005). */
struct halwin *halwin_create(char *error, size_t error_len);

/* Opens the proxy device. Returns 0 on success. */
int halwin_open_proxy(struct halwin *h, char *error, size_t error_len);

/* Register writes the proxy replays when our handle closes, however the process dies (at most 32). */
int halwin_set_close_ops(struct halwin *h, const struct halwin_close_op *ops, uint32_t count);

struct mt_hal *halwin_hal(struct halwin *h);
uint64_t halwin_dma_phys(const struct halwin *h);
uint32_t halwin_low_blocks_seen(const struct halwin *h);
uint32_t halwin_io_errors(const struct halwin *h);

/* Closes the proxy handle (which triggers the close ops) and frees the DMA block. */
void halwin_destroy(struct halwin *h);

#ifdef __cplusplus
}
#endif
#endif
