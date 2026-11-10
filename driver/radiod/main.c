/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radiod: user-mode host of the mt792x chip library on Windows. Bring-up commands only for now.
 *
 *   radiod dma-probe                           allocate the DMA block, verify it is writable, free it
 *                                              (no device access)
 *   radiod bringup <stage 1-5> <patch> <ram>   staged firmware bring-up (docs/MT7921-BRINGUP.md):
 *                                              1 ownership, 2 reset, 3 rings, 4 MCU, 5 firmware
 */
#include <stdio.h>
#include <stdlib.h>
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

/* Reads a whole file into a buffer of exactly its size. */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n > 0 && n <= 64L * 1024L * 1024L && fseek(f, 0, SEEK_SET) == 0) {
        b = malloc((size_t)n);
        if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

static struct mt792x g_dev; /* ~6 KiB: keep it off the stack */

static int bringup(int stage, const char *patch_path, const char *ram_path) {
    struct mt792x_fw fw = {0};
    struct mt792x_patch_info *pinfo = NULL;
    struct mt792x_ram_info *rinfo = NULL;
    struct halwin *h = NULL;
    char error[256];
    int err, rc = 1;

    if (stage < MT792X_STAGE_OWNERSHIP || stage > MT792X_STAGE_FIRMWARE) {
        printf("RESULT: FAIL - stage must be 1..5\n");
        return 2;
    }

    /* 1. Read and validate the firmware files before anything else. */
    uint8_t *patch = read_file(patch_path, &fw.patch_len);
    uint8_t *ram = read_file(ram_path, &fw.ram_len);
    fw.patch = patch;
    fw.ram = ram;
    if (!patch || !ram) {
        printf("RESULT: FAIL - cannot read %s\n", !patch ? patch_path : ram_path);
        goto out;
    }
    pinfo = calloc(1, sizeof(*pinfo));
    rinfo = calloc(1, sizeof(*rinfo));
    if (!pinfo || !rinfo) goto out;
    err = mt792x_patch_parse(patch, fw.patch_len, pinfo);
    if (err) {
        printf("RESULT: FAIL - patch file rejected: %s (%d)\n", mt792x_strerror(err), err);
        goto out;
    }
    err = mt792x_ram_parse(ram, fw.ram_len, rinfo);
    if (err) {
        printf("RESULT: FAIL - RAM file rejected: %s (%d)\n", mt792x_strerror(err), err);
        goto out;
    }
    printf("firmware: patch %s (%u section(s)), RAM %s %s (%u region(s))\n", pinfo->build_date, pinfo->n_sections,
           rinfo->fw_ver, rinfo->build_date, rinfo->n_regions);

    /* 2. DMA block. */
    h = halwin_create(error, sizeof(error));
    if (!h) {
        printf("RESULT: FAIL - %s\n", error);
        goto out;
    }

    /* 3. Proxy. */
    if (halwin_open_proxy(h, error, sizeof(error)) != 0) {
        printf("RESULT: FAIL - %s\n", error);
        goto out;
    }

    /* 4. The stop list must be armed before the chip gets any chance to do DMA. */
    {
        struct mt792x_stop_op ops[MT792X_STOP_OPS_COUNT];
        struct halwin_close_op close_ops[MT792X_STOP_OPS_COUNT + 1];
        size_t n = mt792x_stop_ops(ops, MT792X_STOP_OPS_COUNT), m = 0;
        for (size_t i = 0; i < n; i++) {
            /* mt76 holds the WFSYS reset for 50 ms; the proxy can wait, so put the delay before the
             * last operation (the release of the reset asserted by the one before it). */
            if (i + 1 == n) {
                close_ops[m].offset = 0;
                close_ops[m].value = 50;
                close_ops[m].kind = 4;
                m++;
            }
            close_ops[m].offset = ops[i].bar_offset;
            close_ops[m].value = ops[i].value;
            close_ops[m].kind = ops[i].kind; /* same numbering: 1 write, 2 set, 3 clear */
            m++;
        }
        n = m;
        if (halwin_set_close_ops(h, close_ops, (uint32_t)n) != 0) {
            printf("RESULT: FAIL - the proxy did not accept the stop list; not starting\n");
            goto out;
        }
        printf("stop list armed in the proxy (%u operations)\n", (unsigned)n);
    }

    /* 5. Bring-up. */
    err = mt792x_init(&g_dev, halwin_hal(h));
    if (err) {
        printf("RESULT: FAIL - init: %s (%d)\n", mt792x_strerror(err), err);
        goto out;
    }
    err = mt792x_bringup(&g_dev, &fw, (enum mt792x_stage)stage);
    printf("chip id 0x%04x, rev 0x%08x, last step S%d, failed step S%d, proxy I/O errors %u\n", mt792x_chip_id(&g_dev),
           mt792x_hw_rev(&g_dev), mt792x_last_step(&g_dev), mt792x_failed_step(&g_dev), halwin_io_errors(h));
    if (err) {
        printf("RESULT: FAIL - stage %d: %s (%d) at S%d\n", stage, mt792x_strerror(err), err, mt792x_failed_step(&g_dev));
    } else {
        printf("RESULT: PASS - stage %d reached\n", stage);
        rc = 0;
    }

out:
    /* Always: orderly shutdown (a no-op if nothing was touched), then close the proxy (which replays
     * the stop list) and only then free the DMA block. */
    mt792x_shutdown(&g_dev);
    if (g_dev.shutdown_error) printf("shutdown reported: %s\n", mt792x_strerror(g_dev.shutdown_error));
    halwin_destroy(h);
    free(pinfo);
    free(rinfo);
    free(patch);
    free(ram);
    return rc;
}

int main(int argc, char **argv) {
    printf("radiod (mt792x library %s)\n", mt792x_version());
    if (argc >= 2 && strcmp(argv[1], "dma-probe") == 0) return dma_probe();
    if (argc == 5 && strcmp(argv[1], "bringup") == 0) return bringup(atoi(argv[2]), argv[3], argv[4]);
    printf("usage: radiod dma-probe\n"
           "       radiod bringup <stage 1-5> <patch file> <ram file>\n"
           "         stages: 1 ownership, 2 reset, 3 rings, 4 MCU, 5 firmware\n");
    return 2;
}
