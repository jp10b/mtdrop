/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests of the DMA layout, mt792x_dma_bus and mt792x_init. */
#include "test.h"
#include "rig.h"
#include "../src/internal.h"

#include <stdlib.h>
#include <string.h>

struct area {
    uint32_t off, len;
};

TEST(layout_fits_with_margin_and_no_overlap)
{
    const struct area a[] = {
        { MT792X_OFF_TX0_DESC, MT792X_TX0_NDESC * 16u },
        { MT792X_OFF_TX16_DESC, MT792X_TX16_NDESC * 16u },
        { MT792X_OFF_TX17_DESC, MT792X_TX17_NDESC * 16u },
        { MT792X_OFF_RX0_DESC, MT792X_RX0_NDESC * 16u },
        { MT792X_OFF_RX4_DESC, MT792X_RX4_NDESC * 16u },
        { MT792X_OFF_RX2_DESC, MT792X_RX2_NDESC * 16u },
        { MT792X_OFF_IDLE, MT792X_IDLE_LEN },
        { MT792X_OFF_TX16_BUF, MT792X_TX16_NDESC * MT792X_TX_BUF_SIZE },
        { MT792X_OFF_TX17_BUF, MT792X_TX17_NDESC * MT792X_TX_BUF_SIZE },
        { MT792X_OFF_RX0_BUF, MT792X_RX0_NDESC * MT792X_RX_BUF_SIZE },
        { MT792X_OFF_RX4_BUF, MT792X_RX4_NDESC * MT792X_RX_BUF_SIZE },
        { MT792X_OFF_RX2_BUF, MT792X_RX2_NDESC * MT792X_RX_BUF_SIZE },
        { MT792X_OFF_TX0_SLOT, MT792X_TX0_SLOTS * MT792X_TX0_SLOT_SIZE },
    };
    size_t i, j, n = sizeof(a) / sizeof(a[0]);
    for (i = 0; i < n; i++) {
        CHECK(a[i].off % 4096u == 0);
        CHECK(a[i].off + a[i].len <= MT792X_LAYOUT_TOTAL);
        for (j = 0; j < i; j++) CHECK(a[i].off >= a[j].off + a[j].len || a[j].off >= a[i].off + a[i].len);
    }
    CHECK_EQ_U32(MT792X_LAYOUT_TOTAL, 0xf7000u);
    CHECK(MT792X_LAYOUT_TOTAL <= 1024u * 1024u);                 /* 256 pages suffice */
    CHECK_EQ_U32(MT792X_TX_MAX_FRAME, 4032u);
}

static void check_bases(uint32_t bus)
{
    struct rig r;
    uint32_t b = 0;
    int i;
    CHECK(rig_init(&r, bus) == 0);
    for (i = 0; i < MT792X_RING_COUNT; i++) {
        const struct mt792x_ring *ring = &r.dev->ring[i];
        CHECK(mt792x_dma_bus(r.dev, ring->desc_off, ring->ndesc * 16u, 16u, &b) == 0);
        CHECK(b >= bus && (uint64_t)b + ring->ndesc * 16u <= (uint64_t)bus + RIG_DMA_LEN);
        if (ring->buf_size) {
            CHECK(mt792x_dma_bus(r.dev, ring->buf_off, ring->ndesc * ring->buf_size, 4u, &b) == 0);
            CHECK((uint64_t)b + ring->ndesc * ring->buf_size <= (uint64_t)bus + RIG_DMA_LEN);
        }
    }
    CHECK(mt792x_dma_bus(r.dev, RIG_DMA_LEN - 4u, 4u, 4u, &b) == 0);
    CHECK_EQ_U32(b, bus + RIG_DMA_LEN - 4u);
    CHECK_EQ_U32(r.m.trace_len, 0); /* init touches no register */
    rig_free(&r);
}

TEST(layout_addresses_in_range_for_several_bases)
{
    check_bases(0x00001000u);
    check_bases(0x10000000u);
    check_bases(0x7fe00000u);
    check_bases(0xffe00000u); /* region ends exactly at 4 GiB */
}

TEST(init_rejects_region_crossing_4gib)
{
    struct rig r;
    CHECK(rig_init(&r, 0xfff00000u) == MT792X_E_LAYOUT);
    CHECK_EQ_U32(r.m.trace_len, 0);
    CHECK(mt792x_bringup(r.dev, &r.fw, MT792X_STAGE_OWNERSHIP) == MT792X_E_STATE);
    CHECK_EQ_U32(r.m.trace_len, 0);
    rig_free(&r);
}

TEST(init_rejects_bad_regions)
{
    struct mock m;
    struct mt792x *dev = (struct mt792x *)calloc(1, sizeof(*dev));
    CHECK(mock_init(&m, MT792X_LAYOUT_TOTAL - 4096u) == 0);
    CHECK(mt792x_init(dev, mock_hal(&m)) == MT792X_E_LAYOUT); /* too small */
    mock_free(&m);

    CHECK(mock_init(&m, RIG_DMA_LEN) == 0);
    m.hal.dma_bus = 0x10000800u;                                 /* not 4 KiB aligned */
    CHECK(mt792x_init(dev, mock_hal(&m)) == MT792X_E_LAYOUT);
    m.hal.dma_bus = 0x10000000u;
    m.hal.now_us = NULL;
    CHECK(mt792x_init(dev, mock_hal(&m)) == MT792X_E_INVAL);
    CHECK(mt792x_init(NULL, mock_hal(&m)) == MT792X_E_INVAL);
    CHECK(mt792x_init(dev, NULL) == MT792X_E_INVAL);
    mock_free(&m);

    CHECK(mock_init(&m, MT792X_LAYOUT_TOTAL) == 0);              /* exactly enough */
    CHECK(mt792x_init(dev, mock_hal(&m)) == 0);
    mock_free(&m);
    free(dev);
}

TEST(init_zeroes_the_region)
{
    struct mock m;
    struct mt792x *dev = (struct mt792x *)calloc(1, sizeof(*dev));
    uint32_t i;
    int nonzero = 0;
    CHECK(mock_init(&m, RIG_DMA_LEN) == 0);
    memset(m.hal.dma_virt, 0xa5, RIG_DMA_LEN);
    CHECK(mt792x_init(dev, mock_hal(&m)) == 0);
    for (i = 0; i < RIG_DMA_LEN; i++) nonzero |= m.hal.dma_virt[i];
    CHECK(nonzero == 0);
    mock_free(&m);
    free(dev);
}

TEST(dma_bus_refuses_bad_requests)
{
    struct rig r;
    uint32_t b = 0x12345678u;
    CHECK(rig_init(&r, 0x20000000u) == 0);
    r.m.log_errors = 0;
    CHECK(mt792x_dma_bus(r.dev, RIG_DMA_LEN - 4u, 8u, 4u, &b) == MT792X_E_DMA_ADDR); /* runs past the end */
    CHECK(mt792x_dma_bus(r.dev, RIG_DMA_LEN, 1u, 1u, &b) == MT792X_E_DMA_ADDR);
    CHECK(mt792x_dma_bus(r.dev, 0xfffffff0u, 0x20u, 4u, &b) == MT792X_E_DMA_ADDR);   /* 32-bit wrap */
    CHECK(mt792x_dma_bus(r.dev, 0, 0, 4u, &b) == MT792X_E_DMA_ADDR);                 /* zero length */
    CHECK(mt792x_dma_bus(r.dev, 4u, 16u, 16u, &b) == MT792X_E_DMA_ADDR);             /* misaligned */
    CHECK(mt792x_dma_bus(r.dev, 0, 16u, 3u, &b) == MT792X_E_DMA_ADDR);               /* align not a power of 2 */
    CHECK(mt792x_dma_bus(r.dev, 0, 16u, 0u, &b) == MT792X_E_DMA_ADDR);
    CHECK(mt792x_dma_bus(NULL, 0, 16u, 4u, &b) == MT792X_E_DMA_ADDR);
    CHECK_EQ_U32(b, 0x12345678u);                                                      /* untouched */
    CHECK(r.m.log_errors >= 7u);
    r.m.hal.dma_bus = 0xfff00000u;                                                     /* region now crosses 4 GiB */
    CHECK(mt792x_dma_bus(r.dev, 0, 16u, 4u, &b) == MT792X_E_DMA_ADDR);
    CHECK(mt792x_dma_bus(r.dev, 0, 16u, 4u, NULL) == MT792X_E_DMA_ADDR);
    rig_free(&r);
}

TEST(formatter_basics)
{
    char buf[64];
    mt_format(buf, sizeof(buf), "S%d %s 0x%08x %u %X %llu %c%%", 4, "ok", 0x1au, 12u, 0xabu, 5000000000ull, 'z');
    CHECK(strcmp(buf, "S4 ok 0x0000001a 12 AB 5000000000 z%") == 0);
    mt_format(buf, sizeof(buf), "%d|%3u|%-", -42, 7u);
    CHECK(strcmp(buf, "-42|  7|%-") == 0);
    mt_format(buf, 8, "%s", "truncate me");
    CHECK(strcmp(buf, "truncat") == 0);
}
