/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mt792x/layout.h - the one place where the DMA ring sizes and the layout of
 * the single HAL DMA region are defined.
 *
 * mt76 uses TX0 2048, TX16 128, TX17 256, RX0 8, RX4 512, RX2 1536 descriptors,
 * which needs ~4.1 MiB. Our host has one 2 MiB block, so every ring is smaller.
 *
 * TODO-VERIFY: the minimum ring sizes the chip accepts are not stated anywhere in
 * mt76 (docs/MT7921-BRINGUP.md 3.3). Evidence that sizes are host-chosen: MT7902
 * uses 512 instead of 8 for RX ring 0 with the same code. ASSUMPTION: sizes >= 8
 * and powers of two are safe (the prefetch depth is 4 entries per ring).
 *
 * Region map (offsets from the start of the region; every area 4 KiB aligned):
 *
 *   0x000000  TX0  descriptors    256 x 16 B   (data ring; programmed, never used in bring-up)
 *   0x001000  TX16 descriptors     32 x 16 B   (firmware download)
 *   0x002000  TX17 descriptors     32 x 16 B   (MCU commands)
 *   0x003000  RX0  descriptors     32 x 16 B   (MCU events before/after firmware)
 *   0x004000  RX4  descriptors     32 x 16 B   (MCU events after firmware, "MCU_WA")
 *   0x005000  RX2  descriptors    256 x 16 B   (RX data)
 *   0x006000  idle page           4096 B       (all-zero target of unused TX descriptors)
 *   0x007000  TX16 buffers         32 x 4096 B (one buffer per descriptor slot)
 *   0x027000  TX17 buffers         32 x 4096 B (one buffer per descriptor slot)
 *   0x047000  RX0  buffers         32 x 2048 B
 *   0x057000  RX4  buffers         32 x 2048 B
 *   0x067000  RX2  buffers        256 x 2048 B
 *   0x0e7000  TX0  slots           16 x 4096 B (raw 802.11 TX, spec 12.4: TXD 32 B + TXP 32 B + frame,
 *                                             one slot per in-flight frame / token)
 *   0x0f7000  end (1011712 bytes = 988 KiB = 247 pages; fits in 1 MiB = 256 pages)
 */
#ifndef MT792X_LAYOUT_H
#define MT792X_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

#define MT792X_TX0_NDESC        256u
#define MT792X_TX16_NDESC       32u
#define MT792X_TX17_NDESC       32u
#define MT792X_RX0_NDESC        32u
#define MT792X_RX4_NDESC        32u
#define MT792X_RX2_NDESC        256u
#define MT792X_TX0_SLOTS        16u     /* frames in flight (tokens) at most; [HOST] mt76 has 8192 */
#define MT792X_TX0_SLOT_SIZE    4096u

#define MT792X_RX_BUF_SIZE      2048u   /* MT_RX_BUF_SIZE (mt76.h) */
#define MT792X_TX_BUF_SIZE      4096u   /* one firmware chunk (max_len 4096) or one MCU command */
#define MT792X_DESC_SIZE        16u
#define MT792X_AREA_ALIGN       4096u

#define MT792X_OFF_TX0_DESC     0x000000u
#define MT792X_OFF_TX16_DESC    0x001000u
#define MT792X_OFF_TX17_DESC    0x002000u
#define MT792X_OFF_RX0_DESC     0x003000u
#define MT792X_OFF_RX4_DESC     0x004000u
#define MT792X_OFF_RX2_DESC     0x005000u
#define MT792X_OFF_IDLE         0x006000u
#define MT792X_IDLE_LEN         4096u
#define MT792X_OFF_TX16_BUF     0x007000u
#define MT792X_OFF_TX17_BUF     (MT792X_OFF_TX16_BUF + MT792X_TX16_NDESC * MT792X_TX_BUF_SIZE)
#define MT792X_OFF_RX0_BUF      (MT792X_OFF_TX17_BUF + MT792X_TX17_NDESC * MT792X_TX_BUF_SIZE)
#define MT792X_OFF_RX4_BUF      (MT792X_OFF_RX0_BUF + MT792X_RX0_NDESC * MT792X_RX_BUF_SIZE)
#define MT792X_OFF_RX2_BUF      (MT792X_OFF_RX4_BUF + MT792X_RX4_NDESC * MT792X_RX_BUF_SIZE)
#define MT792X_OFF_TX0_SLOT     (MT792X_OFF_RX2_BUF + MT792X_RX2_NDESC * MT792X_RX_BUF_SIZE)
#define MT792X_LAYOUT_TOTAL     (MT792X_OFF_TX0_SLOT + MT792X_TX0_SLOTS * MT792X_TX0_SLOT_SIZE)

/* Raw TX (spec 12.4). One 4 KiB slot holds, for one frame: the 64-byte "TXWI" that TX ring 0's
 * descriptor points at (32-byte TXD, MT_TXD_SIZE, + 32-byte struct mt76_connac_hw_txp,
 * mt7921/pci.c:263-264) and, right after it, the frame the TXP points at (cut-through). Both
 * objects stay inside one page. The frame length is also limited by the 12-bit TXP length field
 * (MT_TXD_LEN_MASK GENMASK(11,0), mt76_connac.h:47), which is larger. */
#define MT792X_TXWI_SIZE        64u
#define MT792X_TX_MAX_FRAME     (MT792X_TX0_SLOT_SIZE - MT792X_TXWI_SIZE)   /* 4032 bytes, no FCS */

#ifdef __cplusplus
}
#endif

#endif /* MT792X_LAYOUT_H */
