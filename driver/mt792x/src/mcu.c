/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mcu.c - MCU command messages and response matching for a polling host.
 *
 * Message layout (struct mt76_connac2_mcu_txd / _rxd), sequence numbering, the
 * solicited/unsolicited classification and the response rules are derived from Linux
 * mt76: mt76_connac_mcu.c (mt76_connac2_mcu_fill_message), mcu.c, mt7921/mcu.c
 * (mt7921_mcu_parse_response, mt7921_mcu_rx_event), mt7921/pci_mcu.c
 * (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear, Copyright (C) 2019-2021 MediaTek Inc.,
 * Copyright (C) 2019 Lorenzo Bianconi). Spec: docs/MT7921-BRINGUP.md 3.11, 4.
 */
#include "internal.h"
#include "regs.h"

#define MCU_POLL_TICK_US 500u

uint8_t mt_mcu_next_seq(struct mt792x *dev)
{
    uint8_t seq = (uint8_t)(++dev->msg_seq & 0xfu);
    if (!seq) seq = (uint8_t)(++dev->msg_seq & 0xfu);
    return seq;
}

static int is_result_byte_cmd(uint8_t cid)
{
    return cid == MCU_CMD_PATCH_SEM_CONTROL || cid == MCU_CMD_PATCH_FINISH_REQ;
}

/* mt7921_mcu_rx_event: 1 = goes to the response queue, 0 = unsolicited / not for us. */
static int is_response(const uint8_t *e)
{
    uint8_t eid = e[MT_MCU_RXD_EID];
    if (e[MT_MCU_RXD_OPTION] & MT_MCU_UNSOLICITED_OPTION) return 0;
    if (eid == 0x06) return 1;
    if (e[MT_MCU_RXD_EXT_EID] == 0x87 || eid == 0x13 || eid == 0x23 || eid == 0x96 || eid == 0x0d ||
        eid == 0x0f || eid == 0x27 || eid == 0xf0 || eid == 0x07 || e[MT_MCU_RXD_SEQ] == 0)
        return 0;
    return 1;
}

/* Returns 1 = matching response in dev->evt, 0 = event consumed but not ours, < 0 error. */
static int check_event(struct mt792x *dev, uint8_t cid, uint8_t seq)
{
    const uint8_t *e = dev->evt;
    uint32_t len = dev->evt_len, rxd0, type, flag, min_len;

    if (len < MT_MCU_RXD_MIN_ACCEPT) {
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: short packet (%u bytes) dropped", dev->evt_ring, len);
        return 0;
    }
    rxd0 = mt_get_le32(e);
    type = rxd0 >> MT_RXD0_PKT_TYPE_SHIFT;
    flag = (rxd0 >> MT_RXD0_PKT_FLAG_SHIFT) & MT_RXD0_PKT_FLAG_MASK;
    if (type != MT_PKT_TYPE_RX_EVENT || flag == 1) {
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: packet type %u flag %u (%u bytes) is not an MCU event",
                dev->evt_ring, type, flag, len);
        return 0;
    }
    if (!is_response(e)) {
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: unsolicited event eid 0x%02x ext 0x%02x seq %u option 0x%02x (%u bytes)",
                dev->evt_ring, e[MT_MCU_RXD_EID], e[MT_MCU_RXD_EXT_EID], e[MT_MCU_RXD_SEQ], e[MT_MCU_RXD_OPTION], len);
        return 0;
    }
    if (e[MT_MCU_RXD_SEQ] != seq) {
        mt_logf(dev, MT_LOG_WARN, "RX ring %u: stale response eid 0x%02x seq %u while waiting for seq %u, skipped",
                dev->evt_ring, e[MT_MCU_RXD_EID], e[MT_MCU_RXD_SEQ], seq);
        return 0;
    }
    min_len = is_result_byte_cmd(cid) ? MT_MCU_RXD_LEN - 4u + 1u : MT_MCU_RXD_LEN;
    if (len < min_len) {
        mt_logf(dev, MT_LOG_ERROR, "response to cmd 0x%02x seq %u too short: %u bytes (need %u)", cid, seq, len, min_len);
        return MT792X_E_MCU_EVENT;
    }
    mt_logf(dev, MT_LOG_DEBUG, "response cmd 0x%02x seq %u: ring %u eid 0x%02x len %u byte0x20 0x%02x",
            cid, seq, dev->evt_ring, e[MT_MCU_RXD_EID], len, e[MT_MCU_RXD_RESULT_BYTE]);
    return 1;
}

static int wait_response(struct mt792x *dev, uint8_t cid, uint8_t seq)
{
    static const int rings[2] = { MT792X_RING_RX0, MT792X_RING_RX4 };
    uint64_t start = mt_now(dev);
    int i, rc;

    for (;;) {
        int got_any = 0;
        for (i = 0; i < 2; i++) {
            for (;;) {
                rc = mt_ring_rx_take(dev, &dev->ring[rings[i]]);
                if (rc < 0) return rc;
                if (rc == 0) break;
                got_any = 1;
                if (rc == 2) continue;
                rc = check_event(dev, cid, seq);
                if (rc != 0) return rc < 0 ? rc : 0;
            }
        }
        if (mt_now(dev) - start >= MT_MCU_TIMEOUT_US) {
            mt_logf(dev, MT_LOG_ERROR, "no response to cmd 0x%02x seq %u within %u ms (polled RX rings 0 and 4)",
                    cid, seq, MT_MCU_TIMEOUT_US / 1000u);
            return MT792X_E_MCU_TIMEOUT;
        }
        if (!got_any) mt_sleep(dev, MCU_POLL_TICK_US);
    }
}

int mt_mcu_cmd(struct mt792x *dev, uint8_t cid, const uint8_t *payload, uint32_t payload_len,
               int want_resp, int *result_byte)
{
    uint8_t txd[MT_MCU_TXD_LEN];
    uint32_t total = MT_MCU_TXD_LEN + payload_len;
    uint8_t seq;
    int err;

    if (result_byte) *result_byte = -1;
    if (total > MT792X_TX_BUF_SIZE) return MT792X_E_INVAL;
    seq = mt_mcu_next_seq(dev);

    /* struct mt76_connac2_mcu_txd, filled as mt76_connac2_mcu_fill_message does for MCU_CMD(x) */
    mt_memset(txd, 0, sizeof(txd));
    mt_put_le32(txd + 0x00, MT_MCU_TXD0_PKTFMT_QIDX | (total & 0xffffu));
    mt_put_le32(txd + 0x04, MT_MCU_TXD1);
    mt_put_le16(txd + 0x20, (uint16_t)(total - 32u));
    mt_put_le16(txd + 0x22, MT_MCU_PQ_ID);
    txd[0x24] = cid;
    txd[0x25] = MT_MCU_PKT_ID;
    txd[0x26] = MT_MCU_Q_NA;      /* plain MCU_CMD: no ext id, no CE flag */
    txd[0x27] = seq;
    txd[0x29] = 0;                /* ext_cid */
    txd[0x2a] = MT_MCU_S2D_H2N;
    txd[0x2b] = 0;                /* ext_cid_ack */

    mt_logf(dev, MT_LOG_DEBUG, "cmd 0x%02x seq %u len %u%s", cid, seq, total, want_resp ? "" : " (no wait)");
    err = mt_ring_tx_raw(dev, &dev->ring[MT792X_RING_TX17], txd, sizeof(txd), payload, payload_len);
    if (!err) err = mt_ring_tx_wait(dev, &dev->ring[MT792X_RING_TX17], MT_TX_DONE_TIMEOUT_US);
    if (err || !want_resp) return err;

    err = wait_response(dev, cid, seq);
    if (err) return err;
    if (result_byte) *result_byte = is_result_byte_cmd(cid) ? dev->evt[MT_MCU_RXD_RESULT_BYTE] : 0;
    return 0;
}

/* __mt76_mcu_send_firmware with max_len 4096: raw chunks on TX16, no header, one sequence
 * number consumed per chunk. [HOST] wait for dma_idx == cpu_idx after every chunk. */
int mt_mcu_send_firmware(struct mt792x *dev, const uint8_t *data, uint32_t len)
{
    uint32_t done = 0;
    int err;

    while (done < len) {
        uint32_t cur = len - done < MT_FW_CHUNK_MAX ? len - done : MT_FW_CHUNK_MAX;
        uint8_t seq = mt_mcu_next_seq(dev);
        mt_logf(dev, MT_LOG_DEBUG, "FW_SCATTER seq %u: %u bytes at +0x%x", seq, cur, done);
        err = mt_ring_tx_raw(dev, &dev->ring[MT792X_RING_TX16], NULL, 0, data + done, cur);
        if (!err) err = mt_ring_tx_wait(dev, &dev->ring[MT792X_RING_TX16], MT_TX_DONE_TIMEOUT_US);
        if (err) return err;
        done += cur;
    }
    return 0;
}
