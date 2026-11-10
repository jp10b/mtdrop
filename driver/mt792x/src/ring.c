/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ring.c - WFDMA descriptor rings for a polling host.
 *
 * Descriptor format, queue reset, RX fill/re-arm and raw TX enqueue are derived from
 * Linux mt76 dma.c / dma.h (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear,
 * Copyright (C) 2016 Felix Fietkau). Spec: docs/MT7921-BRINGUP.md 2.6, 3.1-3.6.
 *
 * [HOST] deviations from mt76:
 *  - every descriptor always carries a valid bus address inside our region: unused TX
 *    descriptors point at the all-zero idle page (mt76 leaves 0), RX slot k always points at
 *    RX buffer k (also the one slot that is never handed to the chip);
 *  - one buffer per descriptor slot, so a buffer is only rewritten when its slot comes round
 *    again; in addition every TX kick is followed by a wait for dma_idx == cpu_idx.
 */
#include "internal.h"
#include "regs.h"

#define TX_POLL_TICK_US 100u

static uint32_t desc_at(const struct mt792x_ring *r, uint32_t idx)
{
    return r->desc_off + idx * MT792X_DESC_SIZE;
}

static uint32_t buf_at(const struct mt792x_ring *r, uint32_t idx)
{
    return r->buf_off + idx * r->buf_size;
}

static int write_rx_desc(struct mt792x *dev, struct mt792x_ring *r, uint32_t idx, uint32_t ctrl)
{
    uint32_t bus;
    int err = mt792x_dma_bus(dev, buf_at(r, idx), r->buf_size, 4u, &bus);
    if (err) return err;
    mt_dma_st32(dev, desc_at(r, idx) + MT_DESC_BUF0, bus);
    mt_dma_st32(dev, desc_at(r, idx) + MT_DESC_BUF1, 0);
    mt_dma_st32(dev, desc_at(r, idx) + MT_DESC_INFO, 0);
    mt_dma_st32(dev, desc_at(r, idx) + MT_DESC_CTRL, ctrl); /* ctrl last */
    return 0;
}

/* Memory-only preparation of all rings, equivalent to mt76's queue reset (all ctrl = DMA_DONE)
 * followed by the RX fill (ndesc - 1 buffers each). Done before any ring register is written. */
int mt_rings_prepare_memory(struct mt792x *dev)
{
    uint32_t idle_bus;
    int i, err;
    uint32_t k;

    mt_memset(dev->hal->dma_virt, 0, dev->hal->dma_len);
    err = mt792x_dma_bus(dev, MT792X_OFF_IDLE, MT792X_IDLE_LEN, 4u, &idle_bus);
    if (err) return err;

    for (i = 0; i < MT792X_RING_COUNT; i++) {
        struct mt792x_ring *r = &dev->ring[i];
        for (k = 0; k < r->ndesc; k++) {
            if (r->is_rx) {
                uint32_t ctrl = (k + 1u < r->ndesc) ? (r->buf_size << MT_DMA_CTL_SD_LEN0_SHIFT) : MT_DMA_CTL_DMA_DONE;
                err = write_rx_desc(dev, r, k, ctrl);
                if (err) return err;
            } else {
                mt_dma_st32(dev, desc_at(r, k) + MT_DESC_BUF0, idle_bus);
                mt_dma_st32(dev, desc_at(r, k) + MT_DESC_BUF1, 0);
                mt_dma_st32(dev, desc_at(r, k) + MT_DESC_INFO, 0);
                mt_dma_st32(dev, desc_at(r, k) + MT_DESC_CTRL, MT_DMA_CTL_DMA_DONE);
            }
        }
        r->tail = 0;
        r->head = r->is_rx ? r->ndesc - 1u : 0u;
        r->queued = r->is_rx ? r->ndesc - 1u : 0u;
    }
    return 0;
}

/* mt76_dma_queue_reset(reset_idx = true) register part: cpu_idx = 0, dma_idx = 0, ring_size,
 * desc_base, then read dma_idx back (must be 0 for a fresh ring). */
int mt_ring_program(struct mt792x *dev, struct mt792x_ring *r)
{
    uint32_t bus, base_rb, size_rb, dma_idx;
    int err = mt792x_dma_bus(dev, r->desc_off, r->ndesc * MT792X_DESC_SIZE, MT792X_DESC_SIZE, &bus);
    if (err) return err;

    mt_wr(dev, r->regs + MT_RING_CPU_IDX, 0);
    mt_wr(dev, r->regs + MT_RING_DMA_IDX, 0);
    mt_wr(dev, r->regs + MT_RING_SIZE, r->ndesc);
    mt_wr(dev, r->regs + MT_RING_DESC_BASE, bus);
    base_rb = mt_rr(dev, r->regs + MT_RING_DESC_BASE);
    size_rb = mt_rr(dev, r->regs + MT_RING_SIZE);
    dma_idx = mt_rr(dev, r->regs + MT_RING_DMA_IDX);
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    if (dma_idx >= r->ndesc) {
        mt_logf(dev, MT_LOG_ERROR, "S8 %s ring %u: dma_idx reads %u >= size %u (hung bus)",
                r->is_rx ? "RX" : "TX", r->hw_idx, dma_idx, r->ndesc);
        dev->bus_dead = 1;
        return MT792X_E_BUS_DEAD;
    }
    if (base_rb != bus || size_rb != r->ndesc || dma_idx != 0) {
        mt_logf(dev, MT_LOG_ERROR,
                "S8 %s ring %u: readback base 0x%08x (wrote 0x%08x) size %u (wrote %u) dma_idx %u (expect 0)",
                r->is_rx ? "RX" : "TX", r->hw_idx, base_rb, bus, size_rb, r->ndesc, dma_idx);
        return MT792X_E_RING_PROGRAM;
    }
    mt_logf(dev, MT_LOG_INFO, "S8 %s ring %2u @0x%05x: base 0x%08x size %3u dma_idx 0",
            r->is_rx ? "RX" : "TX", r->hw_idx, r->regs, bus, r->ndesc);
    if (!r->is_rx) r->head = r->tail = r->queued = 0;
    return 0;
}

int mt_ring_rx_kick_initial(struct mt792x *dev, struct mt792x_ring *r)
{
    mt_wr(dev, r->regs + MT_RING_CPU_IDX, r->head);
    return dev->bus_dead ? MT792X_E_BUS_DEAD : 0;
}

/* mt76_dma_tx_queue_skb_raw with one segment: [hdr | data] copied into the slot buffer. */
int mt_ring_tx_raw(struct mt792x *dev, struct mt792x_ring *r, const uint8_t *hdr, uint32_t hdr_len,
                   const uint8_t *data, uint32_t data_len)
{
    uint32_t total = hdr_len + data_len, slot, off, bus;
    int err;

    if (r->is_rx || !r->buf_size || total == 0 || total > r->buf_size || hdr_len > r->buf_size) {
        mt_logf(dev, MT_LOG_ERROR, "internal: TX of %u bytes on ring %u refused", total, r->hw_idx);
        return MT792X_E_INVAL;
    }
    if (r->queued + 1u >= r->ndesc - 1u) {
        mt_logf(dev, MT_LOG_ERROR, "TX ring %u full (queued %u of %u)", r->hw_idx, r->queued, r->ndesc);
        return MT792X_E_RING_FULL;
    }
    slot = r->head;
    off = buf_at(r, slot);
    err = mt792x_dma_bus(dev, off, total, 4u, &bus);
    if (err) return err;
    if (hdr_len) mt_dma_write(dev, off, hdr, hdr_len);
    if (data_len) mt_dma_write(dev, off + hdr_len, data, data_len);
    mt_dma_st32(dev, desc_at(r, slot) + MT_DESC_BUF0, bus);
    mt_dma_st32(dev, desc_at(r, slot) + MT_DESC_BUF1, 0);
    mt_dma_st32(dev, desc_at(r, slot) + MT_DESC_INFO, 0);
    mt_dma_st32(dev, desc_at(r, slot) + MT_DESC_CTRL, (total << MT_DMA_CTL_SD_LEN0_SHIFT) | MT_DMA_CTL_LAST_SEC0);
    r->head = (r->head + 1u) % r->ndesc;
    r->queued++;
    /* The HAL write is an opaque call: all descriptor stores above are complete before it. */
    mt_wr(dev, r->regs + MT_RING_CPU_IDX, r->head);
    return dev->bus_dead ? MT792X_E_BUS_DEAD : 0;
}

/* Completion by index compare (mt76_dma_tx_cleanup): wait until dma_idx == cpu_idx. */
int mt_ring_tx_wait(struct mt792x *dev, struct mt792x_ring *r, uint32_t timeout_us)
{
    uint64_t start = mt_now(dev);
    uint32_t idx;

    for (;;) {
        idx = mt_rr(dev, r->regs + MT_RING_DMA_IDX);
        if (idx == 0xffffffffu) return MT792X_E_BUS_DEAD;
        if (idx >= r->ndesc) {
            mt_logf(dev, MT_LOG_ERROR, "TX ring %u: dma_idx reads %u >= size %u (hung bus)", r->hw_idx, idx, r->ndesc);
            dev->bus_dead = 1;
            return MT792X_E_BUS_DEAD;
        }
        if (idx == r->head) {
            r->tail = r->head;
            r->queued = 0;
            return 0;
        }
        if (mt_now(dev) - start >= timeout_us) {
            mt_logf(dev, MT_LOG_ERROR, "TX ring %u not consumed: dma_idx %u cpu_idx %u after %u ms",
                    r->hw_idx, idx, r->head, (uint32_t)((mt_now(dev) - start) / 1000u));
            return MT792X_E_TX_TIMEOUT;
        }
        mt_sleep(dev, TX_POLL_TICK_US);
    }
}

/* Completion detected by DMA_DONE in the descriptor (mt76_dma_dequeue), never by dma_idx. */
int mt_ring_rx_take(struct mt792x *dev, struct mt792x_ring *r)
{
    uint32_t ctrl, len, slot = r->tail;
    int rc = 1, err;

    if (r->queued == 0) return 0;
    ctrl = mt_dma_ld32(dev, desc_at(r, slot) + MT_DESC_CTRL);
    if (!(ctrl & MT_DMA_CTL_DMA_DONE)) return 0;

    len = (ctrl >> MT_DMA_CTL_SD_LEN0_SHIFT) & MT_DMA_CTL_SD_LEN0_MASK;
    if (len == 0 || len > r->buf_size || !(ctrl & MT_DMA_CTL_LAST_SEC0)) {
        /* R14: fragments and oversized lengths are dropped, as mt76 drops them. */
        mt_logf(dev, MT_LOG_WARN, "RX ring %u slot %u: dropped descriptor ctrl 0x%08x", r->hw_idx, slot, ctrl);
        rc = 2;
        dev->evt_len = 0;
    } else {
        mt_dma_read(dev, buf_at(r, slot), dev->evt, len);
        dev->evt_len = len;
        dev->evt_ring = r->hw_idx;
    }
    r->tail = (r->tail + 1u) % r->ndesc;
    r->queued--;

    /* Re-arm the slot that was left empty (head), not the one just consumed. */
    err = write_rx_desc(dev, r, r->head, r->buf_size << MT_DMA_CTL_SD_LEN0_SHIFT);
    if (err) return err;
    r->head = (r->head + 1u) % r->ndesc;
    r->queued++;
    mt_wr(dev, r->regs + MT_RING_CPU_IDX, r->head);
    if (dev->bus_dead) return MT792X_E_BUS_DEAD;
    return rc;
}
