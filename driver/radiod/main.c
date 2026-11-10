/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radiod: user-mode host of the mt792x chip library on Windows. Bring-up commands only for now.
 *
 *   radiod dma-probe     allocate the DMA block, verify it is writable, free it (no device access)
 */
#include <stdio.h>
#include <string.h>

#include "hal_win.h"
#include "mt792x/mt792x.h"

static int dma_probe(void) {
    char error[256];
    struct halwin *h = halwin_create(error, sizeof(error));
    if (!h) {
        printf("RESULT: FAIL - %s\n", error);
        return 1;
    }
    struct mt_hal *hal = halwin_hal(h);
    printf("DMA block: bus address 0x%08x, length 0x%x, low blocks seen %u\n", hal->dma_bus, hal->dma_len,
           halwin_low_blocks_seen(h));
    int ok = halwin_dma_phys(h) + hal->dma_len <= 0x100000000ull && (hal->dma_bus & 0x1FFFFFu) == 0;
    for (uint32_t i = 0; i < hal->dma_len; i += 4096) hal->dma_virt[i] = (uint8_t)(i >> 12);
    hal->dma_virt[hal->dma_len - 1] = 0xA5;
    for (uint32_t i = 0; i < hal->dma_len; i += 4096) ok = ok && hal->dma_virt[i] == (uint8_t)(i >> 12);
    ok = ok && hal->dma_virt[hal->dma_len - 1] == 0xA5;
    uint64_t t0 = hal->now_us(hal->ctx);
    hal->sleep_us(hal->ctx, 500);
    hal->sleep_us(hal->ctx, 5000);
    uint64_t waited = hal->now_us(hal->ctx) - t0;
    printf("clock: 5.5 ms of sleeps measured as %llu us\n", (unsigned long long)waited);
    ok = ok && waited >= 5500 && waited < 60000;
    halwin_destroy(h);
    printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL - block not usable\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    printf("radiod (mt792x library %s)\n", mt792x_version());
    if (argc >= 2 && strcmp(argv[1], "dma-probe") == 0) return dma_probe();
    printf("usage: radiod dma-probe\n");
    return 2;
}
