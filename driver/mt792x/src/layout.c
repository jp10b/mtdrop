/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * layout.c - placement of every ring and buffer inside the single HAL DMA region,
 * and mt792x_dma_bus(), the only function that produces bus addresses for the chip.
 *
 * Ring register numbers follow mt7921_dma_init() in Linux mt76 (mt7921/pci.c,
 * openwrt/mt76 @ 0936617, BSD-3-Clause-Clear, Copyright (C) 2020 MediaTek Inc.).
 */
#include "internal.h"
#include "regs.h"

_Static_assert(MT792X_TX0_NDESC * MT792X_DESC_SIZE <= MT792X_OFF_TX16_DESC - MT792X_OFF_TX0_DESC, "TX0 descriptors overlap");
_Static_assert(MT792X_TX16_NDESC * MT792X_DESC_SIZE <= MT792X_OFF_TX17_DESC - MT792X_OFF_TX16_DESC, "TX16 descriptors overlap");
_Static_assert(MT792X_TX17_NDESC * MT792X_DESC_SIZE <= MT792X_OFF_RX0_DESC - MT792X_OFF_TX17_DESC, "TX17 descriptors overlap");
_Static_assert(MT792X_RX0_NDESC * MT792X_DESC_SIZE <= MT792X_OFF_RX4_DESC - MT792X_OFF_RX0_DESC, "RX0 descriptors overlap");
_Static_assert(MT792X_RX4_NDESC * MT792X_DESC_SIZE <= MT792X_OFF_RX2_DESC - MT792X_OFF_RX4_DESC, "RX4 descriptors overlap");
_Static_assert(MT792X_RX2_NDESC * MT792X_DESC_SIZE <= MT792X_OFF_IDLE - MT792X_OFF_RX2_DESC, "RX2 descriptors overlap");
_Static_assert(MT792X_OFF_IDLE + MT792X_IDLE_LEN <= MT792X_OFF_TX16_BUF, "idle page overlaps");
_Static_assert(MT792X_LAYOUT_TOTAL <= 2u * 1024u * 1024u - 512u * 1024u, "layout must leave margin in a 2 MiB block");
_Static_assert(MT792X_RX_BUF_SIZE <= 0x3fffu && MT792X_TX_BUF_SIZE <= 0x3fffu, "SD_LEN0 is 14 bits");

int mt792x_dma_bus(const struct mt792x *dev, uint32_t offset, uint32_t len, uint32_t align, uint32_t *bus)
{
    uint64_t end, region_end, addr;

    if (!dev || !dev->hal || !dev->hal->dma_virt || !bus) return MT792X_E_DMA_ADDR;
    region_end = (uint64_t)dev->hal->dma_bus + dev->hal->dma_len;
    end = (uint64_t)offset + len;
    addr = (uint64_t)dev->hal->dma_bus + offset;
    if (align == 0 || (align & (align - 1u)) != 0 || len == 0 || end > dev->hal->dma_len ||
        region_end > 0x100000000ull || (addr & (align - 1u)) != 0) {
        mt_logf(dev, MT_LOG_ERROR,
                "DMA address refused: offset 0x%x len 0x%x align %u (region bus 0x%08x len 0x%x)",
                offset, len, align, dev->hal->dma_bus, dev->hal->dma_len);
        return MT792X_E_DMA_ADDR;
    }
    *bus = (uint32_t)addr;
    return 0;
}

static void ring_set(struct mt792x_ring *r, int is_rx, uint32_t hw_idx, uint32_t ndesc, uint32_t desc_off,
                     uint32_t buf_off, uint32_t buf_size)
{
    r->is_rx = (uint8_t)is_rx;
    r->hw_idx = (uint8_t)hw_idx;
    r->regs = (is_rx ? MT_RX_RING_BASE : MT_TX_RING_BASE) + hw_idx * MT_RING_STRIDE;
    r->ndesc = ndesc;
    r->desc_off = desc_off;
    r->buf_off = buf_off;
    r->buf_size = buf_size;
    r->head = r->tail = r->queued = 0;
}

int mt_layout_init(struct mt792x *dev)
{
    const struct mt_hal *h = dev->hal;
    uint32_t bus;
    int i;

    if ((uintptr_t)h->dma_virt % 16u != 0 || h->dma_bus % MT792X_AREA_ALIGN != 0 ||
        h->dma_len < MT792X_LAYOUT_TOTAL || (uint64_t)h->dma_bus + h->dma_len > 0x100000000ull) {
        mt_logf(dev, MT_LOG_ERROR,
                "S0 DMA region unusable: bus 0x%08x len 0x%x (need len >= 0x%x, bus 4 KiB aligned, end <= 4 GiB)",
                h->dma_bus, h->dma_len, MT792X_LAYOUT_TOTAL);
        return MT792X_E_LAYOUT;
    }

    /* mt7921_dma_init order and numbering (spec 2.6, 3.3) */
    ring_set(&dev->ring[MT792X_RING_TX0], 0, MT7921_TXQ_BAND0, MT792X_TX0_NDESC, MT792X_OFF_TX0_DESC, 0, 0);
    ring_set(&dev->ring[MT792X_RING_TX16], 0, MT7921_TXQ_FWDL, MT792X_TX16_NDESC, MT792X_OFF_TX16_DESC,
             MT792X_OFF_TX16_BUF, MT792X_TX_BUF_SIZE);
    ring_set(&dev->ring[MT792X_RING_TX17], 0, MT7921_TXQ_MCU_WM, MT792X_TX17_NDESC, MT792X_OFF_TX17_DESC,
             MT792X_OFF_TX17_BUF, MT792X_TX_BUF_SIZE);
    ring_set(&dev->ring[MT792X_RING_RX0], 1, MT7921_RXQ_MCU_WM, MT792X_RX0_NDESC, MT792X_OFF_RX0_DESC,
             MT792X_OFF_RX0_BUF, MT792X_RX_BUF_SIZE);
    ring_set(&dev->ring[MT792X_RING_RX4], 1, MT7921_RXQ_MCU_WA, MT792X_RX4_NDESC, MT792X_OFF_RX4_DESC,
             MT792X_OFF_RX4_BUF, MT792X_RX_BUF_SIZE);
    ring_set(&dev->ring[MT792X_RING_RX2], 1, MT7921_RXQ_BAND0, MT792X_RX2_NDESC, MT792X_OFF_RX2_DESC,
             MT792X_OFF_RX2_BUF, MT792X_RX_BUF_SIZE);

    /* Every area must pass the address check now, so later failures can only be bugs. */
    for (i = 0; i < MT792X_RING_COUNT; i++) {
        const struct mt792x_ring *r = &dev->ring[i];
        if (mt792x_dma_bus(dev, r->desc_off, r->ndesc * MT792X_DESC_SIZE, MT792X_DESC_SIZE, &bus))
            return MT792X_E_LAYOUT;
        if (r->buf_size && mt792x_dma_bus(dev, r->buf_off, r->ndesc * r->buf_size, 4u, &bus))
            return MT792X_E_LAYOUT;
    }
    if (mt792x_dma_bus(dev, MT792X_OFF_IDLE, MT792X_IDLE_LEN, 4u, &bus)) return MT792X_E_LAYOUT;

    /* Zero everything; nothing has been handed to the chip yet. */
    mt_memset(h->dma_virt, 0, h->dma_len);
    return 0;
}
