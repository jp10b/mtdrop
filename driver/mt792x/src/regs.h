/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * regs.h - MT7921 (PCIe) register map used by the bring-up sequence.
 *
 * Register addresses, bit definitions and constants in this file are derived
 * from Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear; see
 * driver/mt792x/LICENSE.mt76):
 *   mt792x_regs.h, mt7921/regs.h, mt7921/mt7921.h, mt792x.h, dma.h,
 *   mt76_connac_mcu.h, mt76_connac2_mac.h       Copyright (C) 2020-2023 MediaTek Inc.,
 *                                                Copyright (C) 2016 Felix Fietkau
 * The values are documented with source line references in docs/MT7921-BRINGUP.md.
 *
 * All addresses below are BAR0 offsets that the library writes directly. Logical
 * (chip-internal) addresses that need the L1 remap window carry an _L1 suffix.
 */
#ifndef MT792X_SRC_REGS_H
#define MT792X_SRC_REGS_H

/* ---- L1 remap window (spec 1.3) ---- */
#define MT_HIF_REMAP_L1             0xfe24cu
#define MT_HIF_REMAP_L1_MASK        0x0000ffffu
#define MT_HIF_REMAP_BASE_L1        0x40000u
#define MT_HIF_REMAP_WINDOW_LEN     0x10000u

/* ---- logical addresses reached through the L1 window ---- */
#define MT_HW_BOUND_L1              0x70010020u
#define MT_HW_CHIPID_L1             0x70010200u
#define MT_HW_REV_L1                0x70010204u
#define MT_WFSYS_SW_RST_B_L1        0x18000140u
#define MT_TOP_LPCR_HOST_BAND0_L1   0x18060010u

#define MT_HW_BOUND_MT7920          0x00000080u   /* bit 7: chip is an MT7920 */
#define WFSYS_SW_RST_B              0x00000001u
#define WFSYS_SW_INIT_DONE          0x00000010u
#define MT_TOP_LPCR_HOST_FW_OWN     0x00000001u
#define MT_TOP_LPCR_HOST_DRV_OWN    0x00000002u

/* ---- PCIe MAC ---- */
#define MT_PCIE_MAC_INT_ENABLE      0x10188u
#define MT_PCIE_MAC_PM              0x10194u
#define MT_PCIE_MAC_PM_L0S_DIS      0x00000100u

/* ---- CONN_INFRA (fixed map #17: logical 0x7c060000 -> BAR 0xe0000) ---- */
#define MT_CONN_ON_LPCTL            0xe0010u      /* logical 0x7c060010 */
#define PCIE_LPCR_HOST_SET_OWN      0x00000001u
#define PCIE_LPCR_HOST_CLR_OWN      0x00000002u
#define PCIE_LPCR_HOST_OWN_SYNC     0x00000004u
#define MT_CONN_ON_MISC             0xe00f0u      /* logical 0x7c0600f0 */
#define MT_TOP_MISC_FW_STATE        0x00000007u
#define MT_TOP_MISC2_FW_PWR_ON      0x00000001u
#define MT_TOP_MISC2_FW_N9_RDY      0x00000003u

/* ---- other fixed-map registers ---- */
#define MT_SWDEF_MODE               0x9f23cu      /* logical 0x41f23c, fixed map #9 */
#define MT_SWDEF_NORMAL_MODE        0u
#define MT_WFDMA_DUMMY_CR           0x02120u      /* logical 0x54000120, fixed map #11 */
#define MT_WFDMA_NEED_REINIT        0x00000002u
#define MT_DMASHDL_SW_CONTROL       0xd6004u      /* logical 0x7c026004, fixed map #16 */
#define MT_DMASHDL_DMASHDL_BYPASS   0x10000000u

/* ---- WFDMA0 (direct, base 0xd4000) ---- */
#define MT_WFDMA0_RST               0xd4100u
#define MT_WFDMA0_RST_LOGIC_RST     0x00000010u
#define MT_WFDMA0_RST_DMASHDL_ALL_RST 0x00000020u
#define MT_MCU_CMD                  0xd41f0u
#define MT_MCU2HOST_SW_INT_ENA      0xd41f4u
#define MT_MCU_CMD_WAKE_RX_PCIE     0x00000001u
#define MT_WFDMA0_HOST_INT_STA      0xd4200u
#define MT_WFDMA0_HOST_INT_ENA      0xd4204u
#define MT_WFDMA0_GLO_CFG           0xd4208u
#define MT_WFDMA0_RST_DTX_PTR       0xd420cu
#define MT_WFDMA0_GLO_CFG_EXT0      0xd42b0u
#define MT_WFDMA0_CSR_TX_DMASHDL_EN 0x00000040u
#define MT_WFDMA0_PRI_DLY_INT_CFG0  0xd42f0u

/* GLO_CFG bits (spec 3.7) */
#define GLO_CFG_TX_DMA_EN           0x00000001u
#define GLO_CFG_TX_DMA_BUSY         0x00000002u
#define GLO_CFG_RX_DMA_EN           0x00000004u
#define GLO_CFG_RX_DMA_BUSY         0x00000008u
#define GLO_CFG_BUSY_MASK           (GLO_CFG_TX_DMA_BUSY | GLO_CFG_RX_DMA_BUSY)
#define GLO_CFG_EN_MASK             (GLO_CFG_TX_DMA_EN | GLO_CFG_RX_DMA_EN)
/* mt792x_dma_disable / mt792x_dma_cleanup clear mask:
 * TX_DMA_EN | RX_DMA_EN | CSR_DISP_BASE_PTR_CHAIN_EN | OMIT_TX_INFO | OMIT_RX_INFO | OMIT_RX_INFO_PFET2 */
#define GLO_CFG_DISABLE_CLEAR       0x18208005u
/* mt792x_dma_enable, non-mt7925 branch: TX_WB_DDONE | FIFO_LITTLE_ENDIAN | CLK_GAT_DIS |
 * OMIT_TX_INFO | DMA_SIZE(3) | FIFO_DIS_CHECK | RX_WB_DDONE | CSR_DISP_BASE_PTR_CHAIN_EN |
 * OMIT_RX_INFO_PFET2 */
#define GLO_CFG_ENABLE_SET          0x5020b870u

/* ring register blocks (spec 3.2): ring n at base + n * 0x10 */
#define MT_TX_RING_BASE             0xd4300u
#define MT_RX_RING_BASE             0xd4500u
#define MT_RING_STRIDE              0x10u
#define MT_RING_DESC_BASE           0x0u
#define MT_RING_SIZE                0x4u
#define MT_RING_CPU_IDX             0x8u
#define MT_RING_DMA_IDX             0xcu

#define MT_WFDMA0_TX_RING_EXT_CTRL(n) (0xd4600u + 4u * (uint32_t)(n))
#define MT_WFDMA0_RX_RING_EXT_CTRL(n) (0xd4680u + 4u * (uint32_t)(n))
#define MT_PREFETCH(base, depth)    (((uint32_t)(base) << 16) | (uint32_t)(depth))

/* hardware ring numbers (mt7921/mt7921.h) */
#define MT7921_TXQ_BAND0            0u
#define MT7921_TXQ_FWDL             16u
#define MT7921_TXQ_MCU_WM           17u
#define MT7921_RXQ_MCU_WM           0u    /* "event from WM before firmware download" */
#define MT7921_RXQ_BAND0            2u    /* RX data */
#define MT7921_RXQ_MCU_WA           4u    /* "Rx mcu after fw downloaded" (MT_WFDMA0(0x540)) */

/* ---- DMA descriptor (struct mt76_desc, dma.h) ---- */
#define MT_DESC_LEN                 16u
#define MT_DESC_BUF0                0u
#define MT_DESC_CTRL                4u
#define MT_DESC_BUF1                8u
#define MT_DESC_INFO                12u
#define MT_DMA_CTL_SD_LEN1_MASK     0x00003fffu
#define MT_DMA_CTL_LAST_SEC1        0x00004000u
#define MT_DMA_CTL_SD_LEN0_SHIFT    16u
#define MT_DMA_CTL_SD_LEN0_MASK     0x3fffu       /* after shifting */
#define MT_DMA_CTL_LAST_SEC0        0x40000000u
#define MT_DMA_CTL_DMA_DONE         0x80000000u

/* ---- MCU message format (mt76_connac_mcu.h, mt76_connac2_mac.h; spec 4.1, 4.5) ---- */
#define MT_MCU_TXD_LEN              64u
#define MT_MCU_TXD0_PKTFMT_QIDX     0x41000000u   /* PKT_FMT(MT_TX_TYPE_CMD=2) | Q_IDX(MT_TX_MCU_PORT_RX_Q0=0x20) */
#define MT_MCU_TXD1                 0x80010000u   /* LONG_FORMAT | HDR_FORMAT(MT_HDR_FORMAT_CMD=1) */
#define MT_MCU_PQ_ID                0x8000u       /* MCU_PQ_ID(MT_TX_PORT_IDX_MCU=1, MT_TX_MCU_PORT_RX_Q0=0x20) */
#define MT_MCU_PKT_ID               0xa0u
#define MT_MCU_Q_NA                 3u
#define MT_MCU_S2D_H2N              0u

#define MT_MCU_RXD_LEN              36u           /* sizeof(struct mt76_connac2_mcu_rxd) */
#define MT_MCU_RXD_MIN_ACCEPT       0x21u         /* offsetofend(rxd, ext_eid): shorter events are dropped */
#define MT_MCU_RXD_EID              0x1cu
#define MT_MCU_RXD_SEQ              0x1du
#define MT_MCU_RXD_OPTION           0x1eu
#define MT_MCU_RXD_EXT_EID          0x20u
#define MT_MCU_RXD_RESULT_BYTE      0x20u         /* PATCH_SEM_CONTROL / PATCH_FINISH_REQ result */
#define MT_MCU_UNSOLICITED_OPTION   0x04u         /* MCU_UNI_CMD_UNSOLICITED_EVENT */
#define MT_RXD0_PKT_TYPE_SHIFT      27u
#define MT_RXD0_PKT_FLAG_SHIFT      16u
#define MT_RXD0_PKT_FLAG_MASK       0xfu
#define MT_PKT_TYPE_RX_EVENT        7u

/* MCU command ids (spec 4.4) */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ 0x01u
#define MCU_CMD_FW_START_REQ        0x02u
#define MCU_CMD_NIC_POWER_CTRL      0x04u
#define MCU_CMD_PATCH_START_REQ     0x05u
#define MCU_CMD_PATCH_FINISH_REQ    0x07u
#define MCU_CMD_PATCH_SEM_CONTROL   0x10u
#define MCU_CMD_FW_SCATTER          0xeeu

#define PATCH_SEM_RELEASE           0u
#define PATCH_SEM_GET               1u
#define PATCH_NOT_DL_SEM_FAIL       0u
#define PATCH_IS_DL                 1u
#define PATCH_NOT_DL_SEM_SUCCESS    2u
#define PATCH_REL_SEM_SUCCESS       3u

#define FW_START_OVERRIDE           0x00000001u
#define MT_FW_CHUNK_MAX             4096u         /* max_len for PCIe (mt76_connac_mcu.c) */

#endif /* MT792X_SRC_REGS_H */
