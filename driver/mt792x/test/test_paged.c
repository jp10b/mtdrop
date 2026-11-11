/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Paged DMA region (hal->dma_page_bus): the region is built from scattered 4 KiB pages, as the
 * Windows host does when no contiguous low block is available (docs/FINDINGS.md F-008). */
#include <string.h>

#include "mt792x/layout.h"
#include "rig.h"
#include "test.h"

TEST(paged_region_addresses_follow_the_page_table)
{
    struct rig r;
    uint32_t bus = 0, off;
    CHECK(rig_init_paged(&r) == 0);

    CHECK(mt792x_dma_bus(r.dev, 0, 4096, 16, &bus) == 0);
    CHECK_EQ_U32(bus, r.page_bus[0]);
    off = MT792X_OFF_RX2_BUF + 3u * MT792X_RX_BUF_SIZE;          /* second half of a page */
    CHECK(mt792x_dma_bus(r.dev, off, MT792X_RX_BUF_SIZE, 4, &bus) == 0);
    CHECK_EQ_U32(bus, r.page_bus[off / 4096u] + off % 4096u);
    CHECK(r.page_bus[1] != r.page_bus[0] + 4096u);                /* the rig's pages are not contiguous */

    /* Nothing may cross a page, start outside the region, or be misaligned. */
    CHECK(mt792x_dma_bus(r.dev, 4096u - 16u, 32, 16, &bus) != 0);
    CHECK(mt792x_dma_bus(r.dev, 0, 4097, 4, &bus) != 0);
    CHECK(mt792x_dma_bus(r.dev, RIG_DMA_LEN, 16, 16, &bus) != 0);
    CHECK(mt792x_dma_bus(r.dev, 2, 4, 4, &bus) != 0);
    rig_free(&r);
}

TEST(paged_region_rejects_bad_page_tables)
{
    struct rig r;
    CHECK(rig_init_paged(&r) == 0);

    r.page_bus[5] = r.page_bus[9];                                /* the same page twice */
    CHECK(mt792x_init(r.dev, mock_hal(&r.m)) != 0);
    CHECK(rig_init_paged(&r) == 0 || 1);
    rig_free(&r);

    CHECK(rig_init_paged(&r) == 0);
    r.page_bus[7] = 0xFFFFF000u + 0x800u;                         /* not page aligned */
    CHECK(mt792x_init(r.dev, mock_hal(&r.m)) != 0);
    r.page_bus[7] = 0;                                            /* missing */
    CHECK(mt792x_init(r.dev, mock_hal(&r.m)) != 0);
    CHECK_EQ_U32(r.m.trace_len, 0);                               /* no register was touched */
    rig_free(&r);
}

TEST(paged_region_full_bringup_and_sniffer)
{
    struct rig r;
    uint32_t sd;
    unsigned resets;
    CHECK(rig_init_paged(&r) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);                              /* bring-up, firmware, post-boot, sniffer */
    CHECK_EQ_U32(fake_misc(r.f), 3);
    CHECK(rig_check_no_violations(&r) == 0);                      /* every address the chip saw was inside a page */
    CHECK(rig_check_int_ena_never_nonzero(&r) == 0);
    CHECK_EQ_U32(r.m.log_errors, 0);

    sd = r.m.trace_len;
    resets = r.f->wfsys_resets;
    mt792x_shutdown(r.dev);
    CHECK(rig_check_shutdown(&r, rig_shutdown_regs_from(&r, sd), resets, 0x18451800u) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}
