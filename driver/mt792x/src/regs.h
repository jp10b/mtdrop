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

/* ======================================================================================
 * Post-boot, sniffer and RX data path (spec section 11). Every value below carries the
 * mt76 file:line it was read from (openwrt/mt76 @ 0936617).
 * ====================================================================================== */

/* ---- command word, encoded exactly like mt76's 'int cmd' (mt76_connac_mcu.h:1225-1245) ---- */
#define MT_MCU_FIELD_ID             0x000000ffu   /* __MCU_CMD_FIELD_ID GENMASK(7,0) */
#define MT_MCU_FIELD_EXT_SHIFT      8u            /* __MCU_CMD_FIELD_EXT_ID GENMASK(15,8) */
#define MT_MCU_FIELD_QUERY          0x00010000u   /* BIT(16) */
#define MT_MCU_FIELD_UNI            0x00020000u   /* BIT(17) */
#define MT_MCU_FIELD_CE             0x00040000u   /* BIT(18) */
#define MT_MCU_CMD_EXT_CID          0xedu         /* MCU_CMD_EXT_CID (mt76_connac_mcu.h:1384) */
#define MT_MCU_PLAIN(id)            ((uint32_t)(id))  /* MCU_CMD(x) */
#define MT_MCU_EXT(ext)             (MT_MCU_CMD_EXT_CID | ((uint32_t)(ext) << MT_MCU_FIELD_EXT_SHIFT))
#define MT_MCU_EXT_QUERY(ext)       (MT_MCU_EXT(ext) | MT_MCU_FIELD_QUERY)
#define MT_MCU_UNI(id)              (MT_MCU_FIELD_UNI | (uint32_t)(id))
#define MT_MCU_CE(id)               (MT_MCU_FIELD_CE | (uint32_t)(id))

#define MT_MCU_UNI_TXD_LEN          48u           /* sizeof(struct mt76_connac2_mcu_uni_txd) (mt76_connac_mcu.h:103-123) */
#define MT_MCU_UNI_OPTION           0x07u         /* MCU_CMD_UNI_EXT_ACK = ACK|UNI|SET (mt76_connac_mcu.h:1212-1217) */
#define MT_MCU_Q_QUERY              0u            /* mt76_connac_mcu.h:1118-1123 */
#define MT_MCU_Q_SET                1u

/* CE ("offload") command ids (mt76_connac_mcu.h:1392-1416) */
#define MCU_CE_SET_RX_FILTER        0x0au
#define MCU_CE_SET_CHAN_DOMAIN      0x0fu
#define MCU_CE_SET_BSS_ABORT        0x17u
#define MCU_CE_GET_NIC_CAPAB        0x8au
#define MCU_CE_FWLOG_2_HOST         0xc5u
#define MCU_CE_CHIP_CONFIG          0xcau
/* EXT command ids (mt76_connac_mcu.h:1263-1316) */
#define MCU_EXT_ID_RADIO_ON_OFF_CTRL 0x05u
#define MCU_EXT_CHANNEL_SWITCH      0x08u
#define MCU_EXT_EFUSE_BUFFER_MODE   0x21u
#define MCU_EXT_PROTECT_CTRL        0x3eu
#define MCU_EXT_MAC_INIT_CTRL       0x46u
#define MCU_EXT_SET_RX_PATH         0x4eu
#define MCU_EXT_WF_RF_PIN_CTRL      0xbdu
/* UNI command ids (mt76_connac_mcu.h:1318-1370) */
#define MCU_UNI_DEV_INFO_UPDATE     0x01u
#define MCU_UNI_BSS_INFO_UPDATE     0x02u
#define MCU_UNI_STA_REC_UPDATE      0x03u
#define MCU_UNI_SUSPEND             0x05u
#define MCU_UNI_OFFLOAD             0x06u
#define MCU_UNI_HIF_CTRL            0x07u
#define MCU_UNI_SNIFFER             0x24u

/* event ids used for logging (mt76_connac_mcu.h:1053-1074) */
#define MCU_EVENT_DBG_MSG           0x27u
#define MT_MCU_UNI_STATUS_LEN       8u            /* struct mt76_connac_mcu_uni_event {u8 cid; u8 pad[3]; le32 status;} */
#define MT76_CONNAC_MCU_STATUS_WLAN_FAILURE 0xc0000001u /* mt76_connac_mcu.h:1963 */

/* payload values */
#define EE_MODE_EFUSE               0u            /* mt76_connac_mcu.h:1193-1196 */
#define EE_FORMAT_WHOLE             1u            /* mt76_connac_mcu.h:1198-1202 */
#define EXT_CMD_RADIO_LED_CTRL_ENABLE 0x1u        /* mt7921/mt7921.h:40-42 */
#define EXT_CMD_RADIO_ON_LED        0x2u
#define EXT_CMD_RADIO_OFF_LED       0x3u
#define WF_RF_PIN_INIT              0x0u          /* mt7921/mt7921.h:44 */
#define CH_SWITCH_NORMAL            0u            /* mt76_connac_mcu.h:1155 */
#define DEV_INFO_ACTIVE             0u            /* mt76_connac_mcu.h:1047-1050 */
#define UNI_BSS_INFO_BASIC          0u            /* mt76_connac_mcu.h:1426 */
#define CONNECTION_INFRA_AP         0x00010002u   /* STA_TYPE_AP BIT(1) | NETWORK_INFRA BIT(16) (mt76_connac_mcu.h:898-914) */
#define HW_BSSID_0                  0u            /* mt76_connac.h:73 */
#define EXT_BSSID_START             0x10u         /* mt76_connac.h:78 */
#define MT7921_THERMAL_PROT_LOW_TEMP  105         /* mt7921/mcu.c:1521-1522 */
#define MT7921_THERMAL_PROT_HIGH_TEMP 115
#define MT_CONNAC_CONFIG_LEN        328u          /* struct mt76_connac_config (mt76_connac_mcu.h:1874-1881) */
#define MT_CONNAC_CONFIG_DATA_OFF   8u
#define MT_RTS_THRESH_DEFAULT       0x92bu        /* mt7921/init.c:80 */
#define MT_RTS_PKT_THRESH           0x2u          /* mt76_connac_mcu.c:267 */

/* GET_NIC_CAPAB TLV types (mt76_connac_mcu.h:1471-1492) */
#define MT_NIC_CAP_HW_VER           0x05u
#define MT_NIC_CAP_SW_VER           0x06u
#define MT_NIC_CAP_MAC_ADDR         0x07u
#define MT_NIC_CAP_PHY              0x08u
#define MT_NIC_CAP_6G               0x18u
#define MT_NIC_CAP_CHIP_CAP         0x20u
#define MT792X_CHIP_CAP_WF_RF_PIN_CTRL_EVT_EN 0x08u /* BIT(3) (mt792x.h:31) */

/* RX filter (mt7921/mt7921.h:10-13, mt7921/mcu.c:1127-1128, mt792x_regs.h:234) */
#define MT7921_FILTER_FCSFAIL       0x00000004u
#define MT7921_FILTER_CONTROL       0x00000020u
#define MT7921_FILTER_OTHER_BSS     0x00000040u
#define MT7921_FILTER_ENABLE        0x80000000u
#define MT7921_FIF_BIT_SET          0x1u
#define MT7921_FIF_BIT_CLR          0x2u
#define MT_WF_RFCR_DROP_OTHER_BEACON 0x00000800u

/* ---- MAC registers written by mt7921_mac_init (mt7921/init.c:64-81) and mt792x_mac_init_band
 * (mt792x_mac.c:289-316). Logical addresses translated through the fixed map of spec 1.2. ---- */
#define MT_MDP_DCR0                 0x0f000u      /* 0x820cd000 (mt7921/regs.h:12), fixed map #22/#23 */
#define MT_MDP_DCR0_DAMSDU_EN       0x00008000u   /* BIT(15) */
#define MT_MDP_DCR0_RX_HDR_TRANS_EN 0x00080000u   /* BIT(19) */
#define MT_MDP_DCR1                 0x0f004u      /* 0x820cd004 (mt7921/regs.h:16), fixed map #23 */
#define MT_MAX_RX_LEN_MASK          0x0000fff8u   /* MT_MDP_DCR1_MAX_RX_LEN / MT_DMA_DCR0_MAX_RX_LEN GENMASK(15,3) */
#define MT_MAX_RX_LEN_1536          (1536u << 3)
#define MT_WTBL_UPDATE              0x34230u      /* 0x820d4230 = MT_WTBLON_TOP(0x230) (mt7921/regs.h:77), fixed map #0 */
#define MT_WTBL_UPDATE_WLAN_IDX     0x000003ffu   /* GENMASK(9,0) */
#define MT_WTBL_UPDATE_ADM_COUNT_CLEAR 0x00001000u /* BIT(12) */
#define MT_WTBL_UPDATE_BUSY         0x80000000u   /* BIT(31) (mt792x_regs.h:162) */
#define MT792X_WTBL_SIZE            20u           /* mt792x.h:18 */
#define MT792X_WTBL_RESERVED        19u           /* mt792x.h:19; vif idx i uses wcid 19 - i (mt7921/main.c:332) */
/* per band 0 / 1 */
#define MT_TMAC_CTCR0(b)            ((b) ? 0xa10f4u : 0x210f4u) /* 0x820e40f4 / 0x820f40f4, map #2 / #36 */
#define MT_TMAC_CTCR0_INS_DDLMT_REFTIME 0x0000003fu /* GENMASK(5,0) */
#define MT_TMAC_CTCR0_INS_DDLMT_EN  0x00020000u   /* BIT(17) */
#define MT_TMAC_CTCR0_INS_DDLMT_VHT_SMPDU_EN 0x00040000u /* BIT(18) */
#define MT_WF_RMAC_MIB_TIME0(b)     ((b) ? 0xa17c4u : 0x217c4u) /* RMAC + 0x3c4, map #7 / #37 */
#define MT_WF_RMAC_MIB_AIRTIME0(b)  ((b) ? 0xa1780u : 0x21780u) /* RMAC + 0x380 */
#define MT_WF_RMAC_MIB_RXTIME_EN    0x40000000u   /* BIT(30) */
#define MT_MIB_SCR1(b)              ((b) ? 0xa4804u : 0x24804u) /* MIB + 0x004, map #1 / #43 */
#define MT_MIB_TXDUR_EN             0x00000100u   /* BIT(8) */
#define MT_MIB_RXDUR_EN             0x00000200u   /* BIT(9) */
#define MT_DMA_DCR0(b)              ((b) ? 0xa1e00u : 0x21e00u) /* WF_DMA + 0, map #3 / #38 */
#define MT_DMA_DCR0_RXD_G5_EN       0x00800000u   /* BIT(23) */
#define MT_WTBLOFF_TOP_RSCR(b)      ((b) ? 0xa3408u : 0x23408u) /* WTBLOFF + 0x008, map #29 / #39 */
#define MT_WTBLOFF_TOP_RSCR_RCPI_MASK 0xc3000000u /* RCPI_MODE GENMASK(31,30) | RCPI_PARAM GENMASK(25,24) */
#define MT_WTBLOFF_TOP_RSCR_RCPI_SET  0x03000000u /* RCPI_MODE 0, RCPI_PARAM 0x3 */

/* mt792x_mac_set_timeing (mt792x_mac.c:35-74), band 0 only; registers mt792x_regs.h:37-46,196-215 */
#define MT_TMAC_CDTR0               0x21090u      /* 0x820e4090, map #2 */
#define MT_TMAC_ODTR0               0x21094u      /* 0x820e4094 */
#define MT_TMAC_ICR0_0              0x210a4u      /* 0x820e40a4 */
#define MT_AGG_ACR0_0               0x20884u      /* 0x820e2084, map #5 */
#define MT_AGG_ACR_CFEND_RATE       0x00003fffu   /* GENMASK(13,0) */
#define MT_ARB_SCR0                 0x20c80u      /* 0x820e3080, map #6 */
#define MT_ARB_SCR_TX_DISABLE       0x00000100u   /* BIT(8) */
#define MT_ARB_SCR_RX_DISABLE       0x00000200u   /* BIT(9) */
#define MT_TIMING_CCK               (231u | (48u << 16))  /* PLCP 231, CCA 48 */
#define MT_TIMING_OFDM              (60u | (28u << 16))   /* PLCP 60, CCA 28 */
#define MT_IFS(eifs, rifs, sifs, slot) ((uint32_t)(eifs) | ((uint32_t)(rifs) << 10) | ((uint32_t)(sifs) << 16) | ((uint32_t)(slot) << 24))
#define MT792X_SLOTTIME_DEFAULT     9u            /* phy->slottime = 9 (mt792x_core.c:771) */
#define MT792X_CFEND_RATE_DEFAULT   0x49u         /* OFDM 24M (mt792x.h:22) */
#define MT792X_CFEND_RATE_11B       0x03u

/* ---- RX descriptor (mt76_connac2_mac.h:184-277, mt76_connac.h:9-21) ---- */
#define MT_PKT_TYPE_TXS             0u
#define MT_PKT_TYPE_NORMAL          2u
#define MT_PKT_TYPE_TXRX_NOTIFY     6u
#define MT_PKT_TYPE_NORMAL_MCU      8u
#define MT_RXD0_LENGTH              0x0000ffffu
#define MT_RXD1_WLAN_IDX            0x000003ffu
#define MT_RXD1_GROUP_1             0x00000800u
#define MT_RXD1_GROUP_2             0x00001000u
#define MT_RXD1_GROUP_3             0x00002000u
#define MT_RXD1_GROUP_4             0x00004000u
#define MT_RXD1_GROUP_5             0x00008000u
#define MT_RXD1_SEC_MODE_SHIFT      16u           /* GENMASK(20,16) */
#define MT_RXD1_SEC_MODE_MASK       0x1fu
#define MT_RXD1_CM                  0x00800000u
#define MT_RXD1_CLM                 0x01000000u
#define MT_RXD1_ICV_ERR             0x02000000u
#define MT_RXD1_TKIP_MIC_ERR        0x04000000u
#define MT_RXD1_FCS_ERR             0x08000000u
#define MT_RXD1_BAND_IDX            0x10000000u
#define MT_RXD2_HDR_TRANS           0x00002000u
#define MT_RXD2_HDR_OFFSET_SHIFT    14u           /* GENMASK(15,14) */
#define MT_RXD2_AMSDU_ERR           0x00800000u
#define MT_RXD2_MAX_LEN_ERROR       0x01000000u
#define MT_RXD2_FRAG                0x08000000u
#define MT_RXD2_NON_AMPDU           0x40000000u
#define MT_RXD3_CH_FREQ_SHIFT       8u            /* GENMASK(15,8) */
#define MT_RXD3_ADDR_TYPE_SHIFT     16u           /* GENMASK(17,16) */
#define MT_RXD3_U2M                 1u
#define MT_RXD4_PAYLOAD_FORMAT      0x3u
#define MT_PRXV_TX_RATE             0x0000007fu   /* GENMASK(6,0) */
#define MT_PRXV_NSTS_SHIFT          7u            /* GENMASK(9,7) */
#define MT_PRXV_HT_AD_CODE          0x00000800u   /* BIT(11) */
#define MT_PRXV_FRAME_MODE_SHIFT    12u           /* GENMASK(14,12) */
#define MT_PRXV_HT_SGI_SHIFT        15u           /* GENMASK(16,15) */
#define MT_PRXV_HT_STBC_SHIFT       22u           /* GENMASK(23,22) */
#define MT_PRXV_TX_MODE_SHIFT       24u           /* GENMASK(27,24) */
#define MT_RCPI_OFFSET              220           /* to_rssi(): (RCPI - 220) / 2 (mt7921/mt7921.h:114) */
#define MT_TX_FREE_MSDU_CNT         0x000003ffu   /* GENMASK(9,0) (mt76_connac2_mac.h:40) */
#define MT_TX_FREE_PAIR             0x80000000u   /* BIT(31) */
#define MT_TXS_HDR_WORDS            2u            /* "rxd += 2" (mt7921/mac.c:606) */
#define MT_TXS_RECORD_WORDS         8u

/* ======================================================================================
 * Raw TX (spec section 12), openwrt/mt76 @ 0936617.
 * ====================================================================================== */
/* TXD (mt76_connac2_mac.h:7-139), MT_TXD_SIZE 32 (mt76_connac.h:34) */
#define MT_TXD_SIZE                 32u
#define MT_HW_TXP_SIZE              32u           /* struct mt76_connac_hw_txp (mt76_connac.h:211-224) */
#define MT_TXD0_TX_BYTES            0x0000ffffu   /* GENMASK(15,0) */
#define MT_TXD0_PKT_FMT_SHIFT       23u           /* GENMASK(24,23); MT_TX_TYPE_CT = 0 */
#define MT_TXD0_Q_IDX_SHIFT         25u           /* GENMASK(31,25) */
#define MT_TX_TYPE_CT               0u
#define MT_LMAC_ALTX0               0x10u         /* mt76_connac2_mac.h:29 */
#define MT_TXD1_LONG_FORMAT         0x80000000u   /* BIT(31) */
#define MT_TXD1_OWN_MAC_SHIFT       24u           /* GENMASK(29,24) */
#define MT_TXD1_TID_SHIFT           20u           /* GENMASK(22,20) */
#define MT_TXD1_HDR_FORMAT_SHIFT    16u           /* GENMASK(17,16) */
#define MT_HDR_FORMAT_802_11        2u
#define MT_TXD1_HDR_INFO_SHIFT      11u           /* GENMASK(15,11): header length / 2 */
#define MT_TXD1_WLAN_IDX            0x000003ffu   /* GENMASK(9,0) */
#define MT_TXD2_FIX_RATE            0x80000000u   /* BIT(31) */
#define MT_TXD2_HTC_VLD             0x00002000u   /* BIT(13) */
#define MT_TXD2_MULTICAST           0x00000400u   /* BIT(10) */
#define MT_TXD2_FRAME_TYPE_SHIFT    4u            /* GENMASK(5,4) */
#define MT_TXD3_SN_VALID            0x80000000u   /* BIT(31) */
#define MT_TXD3_BA_DISABLE          0x10000000u   /* BIT(28) */
#define MT_TXD3_SEQ_SHIFT           16u           /* GENMASK(27,16) */
#define MT_TXD3_REM_TX_COUNT_SHIFT  11u           /* GENMASK(15,11) */
#define MT_TXD3_NO_ACK              0x00000001u   /* BIT(0) */
#define MT_TXD5_TX_STATUS_HOST      0x00000400u   /* BIT(10) */
#define MT_TXD5_PID                 0x000000ffu   /* GENMASK(7,0) */
#define MT_TXD6_TX_RATE_SHIFT       16u           /* GENMASK(29,16) */
#define MT_TXD6_FIXED_BW            0x00000004u   /* BIT(2) */
#define MT_TXD7_TYPE_SHIFT          20u           /* GENMASK(21,20) */
#define MT_TXD7_SUB_TYPE_SHIFT      16u           /* GENMASK(19,16) */
#define MT_TXD7_SPE_IDX_SHIFT       11u           /* GENMASK(15,11) */
#define MT_TX_RATE_MODE_SHIFT       6u            /* GENMASK(9,6) */
#define MT_PHY_TYPE_CCK             0u            /* mt76.h:335-336 */
#define MT_PHY_TYPE_OFDM            1u
#define MT_TX_REM_COUNT_DEFAULT     15u           /* FIELD_PREP(MT_TXD3_REM_TX_COUNT, 15) (mt76_connac_mac.c:568) */
#define MT_PACKET_ID_FIRST          3u            /* mt76.h:494 */
#define MT_PACKET_ID_MASK           0x7fu         /* mt76.h:490: idr_alloc(.., FIRST, MASK) gives 3..126 */
/* TXP (mt76_connac.h:44-47,211-224) */
#define MT_MSDU_ID_VALID            0x8000u       /* BIT(15) */
#define MT_TXD_LEN_LAST             0x8000u       /* BIT(15), connac2 */
#define MT_TXD_LEN_MASK             0x0fffu       /* GENMASK(11,0) */
/* TX-free entries (mt76_connac2_mac.h:40-48) */
#define MT_TX_FREE_WLAN_ID_SHIFT    14u           /* GENMASK(23,14) */
#define MT_TX_FREE_MSDU_ID_SHIFT    16u           /* GENMASK(30,16) */
#define MT_TX_FREE_MSDU_ID_MASK     0x7fffu
#define MT_TX_FREE_STATUS_SHIFT     13u           /* GENMASK(14,13), 0 = success */
#define MT_TX_FREE_COUNT            0x00001fffu   /* GENMASK(12,0) */
/* TXS (mt76_connac2_mac.h:141-174) */
#define MT_TXS0_TXS_FORMAT_SHIFT    23u           /* GENMASK(24,23); mt7921_mac_add_txs ignores > 1 */
#define MT_TXS0_ACK_ERROR_MASK      0x00070000u   /* GENMASK(18,16) */
#define MT_TXS0_TX_RATE             0x00003fffu   /* GENMASK(13,0) */
#define MT_TXS2_WCID_SHIFT          16u           /* GENMASK(25,16) */
#define MT_TXS3_PID_SHIFT           24u           /* GENMASK(31,24) */
#define MCU_EVENT_TX_DONE           0x0fu         /* mt76_connac_mcu.h:1062; txs[] at payload offset 28 (mt7921/mcu.h:9-34) */
#define MT_TX_DONE_EVENT_TXS_OFF    28u
/* Per-rate TX power (mt76_connac_mcu.c:2104-2353, mt76_connac_mcu.h:1852-1872) */
#define MCU_CE_SET_RATE_TX_POWER    0x5du         /* mt76_connac_mcu.h:1405 */
#define MCU_CE_REG_READ             0xc0u         /* mt76_connac_mcu.h:1412, sent as MCU_CE_QUERY */
#define MT_PSE_BASE                 0x820c8000u   /* mt792x_regs.h:28 */
#define MT_SKU_POWER_LIMIT          161u
#define MT_TX_POWER_TLV_LEN         44u           /* struct mt76_connac_tx_power_limit_tlv */
#define MT_TX_POWER_BATCH           8u            /* batch_len for connac2 */
#define MT_SKU_MAX_POWER            127           /* "no limit" filler for connac2 */

#endif /* MT792X_SRC_REGS_H */
