/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mcu.c - MCU command messages and response matching for a polling host.
 *
 * Message layouts (struct mt76_connac2_mcu_txd / _uni_txd / _rxd), sequence numbering, the
 * solicited/unsolicited classification and the response rules are derived from Linux
 * mt76: mt76_connac_mcu.c (mt76_connac2_mcu_fill_message, lines 3545-3618), mcu.c
 * (mt76_mcu_skb_send_and_get_msg, lines 100-136), mt7921/mcu.c (mt7921_mcu_parse_response
 * lines 16-92, mt7921_mcu_rx_event lines 379-414), mt7921/pci_mcu.c (openwrt/mt76 @ 0936617,
 * BSD-3-Clause-Clear, Copyright (C) 2019-2021 MediaTek Inc., Copyright (C) 2019 Lorenzo
 * Bianconi). Spec: docs/MT7921-BRINGUP.md 3.11, 4, 11.2.
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

static int is_result_byte_cmd(uint32_t cmd)
{
    return cmd == MT_MCU_PLAIN(MCU_CMD_PATCH_SEM_CONTROL) || cmd == MT_MCU_PLAIN(MCU_CMD_PATCH_FINISH_REQ);
}

/* MCU_UNI_CMD(DEV_INFO_UPDATE / BSS_INFO_UPDATE / STA_REC_UPDATE / HIF_CTRL / OFFLOAD / SUSPEND):
 * payload {u8 cid; u8 pad[3]; le32 status} (mt7921/mcu.c:52-68). */
static int is_uni_status_cmd(uint32_t cmd)
{
    return cmd == MT_MCU_UNI(MCU_UNI_DEV_INFO_UPDATE) || cmd == MT_MCU_UNI(MCU_UNI_BSS_INFO_UPDATE) ||
           cmd == MT_MCU_UNI(MCU_UNI_STA_REC_UPDATE) || cmd == MT_MCU_UNI(MCU_UNI_HIF_CTRL) ||
           cmd == MT_MCU_UNI(MCU_UNI_OFFLOAD) || cmd == MT_MCU_UNI(MCU_UNI_SUSPEND);
}

static const char *fmt_name(uint32_t cmd)
{
    if (cmd & MT_MCU_FIELD_UNI) return "UNI";
    if (cmd & MT_MCU_FIELD_CE) return "CE";
    if ((cmd & MT_MCU_FIELD_ID) == MT_MCU_CMD_EXT_CID) return "EXT";
    return "CMD";
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

void mt_mcu_note_event(struct mt792x *dev, const uint8_t *e, uint32_t len, uint32_t ring, int solicited)
{
    dev->rxc.events++;
    if (len > MT_MCU_RXD_EID) dev->rxc.event_by_id[e[MT_MCU_RXD_EID]]++;
    if (solicited) return;
    dev->rxc.unsolicited++;
    if (len < MT_MCU_RXD_MIN_ACCEPT) {
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: short event (%u bytes) dropped", ring, len);
        return;
    }
    if (e[MT_MCU_RXD_EID] == MCU_EVENT_DBG_MSG && len >= MT_MCU_RXD_LEN + 10u && e[MT_MCU_RXD_LEN + 2u] == 3u) {
        /* struct mt7921_debug_msg {le16 id; u8 type; u8 flag; le32 value; le16 len; u8 content[]}
         * (mt7921/mcu.c:239-260): type 3 = firmware log text. Printed sanitised, at DEBUG. */
        char text[96];
        uint32_t n = mt_get_le16(e + MT_MCU_RXD_LEN + 8u), i, k = 0;
        if (n > len - MT_MCU_RXD_LEN - 10u) n = len - MT_MCU_RXD_LEN - 10u;
        for (i = 0; i < n && k + 1u < sizeof(text); i++) {
            uint8_t c = e[MT_MCU_RXD_LEN + 10u + i];
            text[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
        }
        text[k] = '\0';
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: firmware log: %s", ring, text);
        return;
    }
    mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: unsolicited event eid 0x%02x ext 0x%02x seq %u option 0x%02x (%u bytes)", ring,
            e[MT_MCU_RXD_EID], e[MT_MCU_RXD_EXT_EID], e[MT_MCU_RXD_SEQ], e[MT_MCU_RXD_OPTION], len);
}

/* Returns 1 = matching response in dev->evt, 0 = packet consumed but not ours, < 0 error. */
static int check_event(struct mt792x *dev, uint32_t cmd, uint8_t seq)
{
    const uint8_t *e = dev->evt;
    uint32_t len = dev->evt_len, rxd0, type, flag, min_len;

    if (len < 4u) {
        mt_logf(dev, MT_LOG_DEBUG, "RX ring %u: short packet (%u bytes) dropped", dev->evt_ring, len);
        return 0;
    }
    rxd0 = mt_get_le32(e);
    type = rxd0 >> MT_RXD0_PKT_TYPE_SHIFT;
    flag = (rxd0 >> MT_RXD0_PKT_FLAG_SHIFT) & MT_RXD0_PKT_FLAG_MASK;
    if (type != MT_PKT_TYPE_RX_EVENT || flag == 1) {
        mt_rx_account_packet(dev, e, len, dev->evt_ring, 1);
        return 0;
    }
    if (len < MT_MCU_RXD_MIN_ACCEPT || !is_response(e)) {
        mt_mcu_note_event(dev, e, len, dev->evt_ring, 0);
        return 0;
    }
    if (e[MT_MCU_RXD_SEQ] != seq) {
        mt_mcu_note_event(dev, e, len, dev->evt_ring, 0);
        mt_logf(dev, MT_LOG_WARN, "RX ring %u: stale response eid 0x%02x seq %u while waiting for seq %u, skipped",
                dev->evt_ring, e[MT_MCU_RXD_EID], e[MT_MCU_RXD_SEQ], seq);
        return 0;
    }
    mt_mcu_note_event(dev, e, len, dev->evt_ring, 1);
    if (is_result_byte_cmd(cmd)) min_len = MT_MCU_RXD_LEN - 4u + 1u;
    else if (is_uni_status_cmd(cmd)) min_len = MT_MCU_RXD_LEN + MT_MCU_UNI_STATUS_LEN;
    else min_len = MT_MCU_RXD_LEN;
    if (len < min_len) {
        mt_logf(dev, MT_LOG_ERROR, "response to cmd 0x%05x seq %u too short: %u bytes (need %u)", cmd, seq, len, min_len);
        return MT792X_E_MCU_EVENT;
    }
    if (is_uni_status_cmd(cmd) && e[MT_MCU_RXD_LEN] != (cmd & MT_MCU_FIELD_ID)) {
        /* "skip invalid event" (mt7921/mcu.c:66-68): -EAGAIN, keep waiting */
        mt_logf(dev, MT_LOG_WARN, "response seq %u carries cid 0x%02x, waiting for 0x%02x: skipped", seq,
                e[MT_MCU_RXD_LEN], cmd & MT_MCU_FIELD_ID);
        return 0;
    }
    mt_logf(dev, MT_LOG_DEBUG, "response cmd 0x%05x seq %u: ring %u eid 0x%02x len %u byte0x20 0x%02x", cmd, seq,
            dev->evt_ring, e[MT_MCU_RXD_EID], len, e[MT_MCU_RXD_RESULT_BYTE]);
    return 1;
}

static int wait_response(struct mt792x *dev, uint32_t cmd, uint8_t seq, uint32_t timeout_us)
{
    static const int rings[3] = { MT792X_RING_RX2, MT792X_RING_RX0, MT792X_RING_RX4 };
    uint64_t start = mt_now(dev);
    int i, rc;

    for (;;) {
        int got_any = 0;
        /* Data ring 2 first and only after post-boot: frames must not stall it during a command,
         * and mt76 dispatches events identically on every RX ring (mt7921/mac.c:578-621). */
        for (i = dev->rx_active ? 0 : 1; i < 3; i++) {
            struct mt792x_ring *r = &dev->ring[rings[i]];
            int is_data = rings[i] == MT792X_RING_RX2;
            uint32_t budget = r->ndesc;
            while (budget--) {
                uint32_t len = 0;
                rc = mt_ring_rx_take_buf(dev, r, dev->evt, sizeof(dev->evt), &len, !is_data);
                if (rc < 0) return rc;
                if (rc == 0) break;
                got_any = 1;
                if (rc == 2) continue;
                dev->evt_len = len;
                dev->evt_ring = r->hw_idx;
                rc = check_event(dev, cmd, seq);
                if (rc != 0) {
                    if (is_data) {
                        int ferr = mt_ring_rx_flush(dev, r);
                        if (ferr) return ferr;
                    }
                    return rc < 0 ? rc : 0;
                }
            }
            if (is_data) {
                rc = mt_ring_rx_flush(dev, r);
                if (rc) return rc;
            }
        }
        if (mt_now(dev) - start >= timeout_us) {
            mt_logf(dev, MT_LOG_ERROR, "no response to cmd 0x%05x seq %u within %u ms (polled RX rings 0, 4%s)", cmd, seq,
                    timeout_us / 1000u, dev->rx_active ? " and 2" : "");
            return MT792X_E_MCU_TIMEOUT;
        }
        if (!got_any) mt_sleep(dev, MCU_POLL_TICK_US);
    }
}

/* mt76_connac2_mcu_fill_message: returns the header length (48 UNI, 64 otherwise). */
static uint32_t fill_header(uint8_t *txd, uint32_t cmd, uint32_t total, uint8_t seq)
{
    uint8_t id = (uint8_t)(cmd & MT_MCU_FIELD_ID);
    uint8_t ext = (uint8_t)((cmd >> MT_MCU_FIELD_EXT_SHIFT) & 0xffu);

    mt_memset(txd, 0, MT_MCU_TXD_LEN);
    mt_put_le32(txd + 0x00, MT_MCU_TXD0_PKTFMT_QIDX | (total & 0xffffu));
    mt_put_le32(txd + 0x04, MT_MCU_TXD1);
    mt_put_le16(txd + 0x20, (uint16_t)(total - 32u));
    if (cmd & MT_MCU_FIELD_UNI) {
        /* struct mt76_connac2_mcu_uni_txd (mt76_connac_mcu.h:103-123) */
        mt_put_le16(txd + 0x22, id);  /* cid, 16 bit */
        txd[0x24] = 0;                /* rsv */
        txd[0x25] = MT_MCU_PKT_ID;
        txd[0x26] = 0;                /* frag_n */
        txd[0x27] = seq;
        mt_put_le16(txd + 0x28, 0);   /* checksum */
        txd[0x2a] = MT_MCU_S2D_H2N;
        txd[0x2b] = MT_MCU_UNI_OPTION;
        return MT_MCU_UNI_TXD_LEN;
    }
    /* struct mt76_connac2_mcu_txd (mt76_connac_mcu.h:55-72) */
    mt_put_le16(txd + 0x22, MT_MCU_PQ_ID);
    txd[0x24] = id;
    txd[0x25] = MT_MCU_PKT_ID;
    txd[0x27] = seq;
    txd[0x29] = ext;
    if (ext || (cmd & MT_MCU_FIELD_CE)) {
        txd[0x26] = (cmd & MT_MCU_FIELD_QUERY) ? MT_MCU_Q_QUERY : MT_MCU_Q_SET;
        txd[0x2b] = ext ? 1u : 0u;    /* ext_cid_ack */
    } else {
        txd[0x26] = MT_MCU_Q_NA;
    }
    txd[0x2a] = MT_MCU_S2D_H2N;       /* no WA flag on MT7921 */
    return MT_MCU_TXD_LEN;
}

int mt_mcu_send(struct mt792x *dev, const char *name, uint32_t cmd, const uint8_t *payload, uint32_t payload_len,
                int wait, uint32_t timeout_us, int *result)
{
    uint8_t txd[MT_MCU_TXD_LEN];
    uint32_t hdr_len = (cmd & MT_MCU_FIELD_UNI) ? MT_MCU_UNI_TXD_LEN : MT_MCU_TXD_LEN;
    uint32_t total = hdr_len + payload_len;
    uint64_t t0;
    uint8_t seq;
    int err, res = 0;

    if (result) *result = -1;
    if (total > MT792X_TX_BUF_SIZE) return MT792X_E_INVAL;
    seq = mt_mcu_next_seq(dev);
    (void)fill_header(txd, cmd, total, seq);

    t0 = mt_now(dev);
    err = mt_ring_tx_raw(dev, &dev->ring[MT792X_RING_TX17], txd, hdr_len, payload, payload_len);
    if (!err) err = mt_ring_tx_wait(dev, &dev->ring[MT792X_RING_TX17], MT_TX_DONE_TIMEOUT_US);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "MCU %s (%s 0x%02x/0x%02x) seq %u: not sent: %s", name, fmt_name(cmd),
                cmd & MT_MCU_FIELD_ID, (cmd >> MT_MCU_FIELD_EXT_SHIFT) & 0xffu, seq, mt792x_strerror(err));
        return err;
    }
    if (!wait) {
        mt_logf(dev, MT_LOG_INFO, "MCU %s (%s 0x%02x/0x%02x) seq %u len %u: sent, no response awaited", name,
                fmt_name(cmd), cmd & MT_MCU_FIELD_ID, (cmd >> MT_MCU_FIELD_EXT_SHIFT) & 0xffu, seq, total);
        if (result) *result = 0;
        return 0;
    }
    err = wait_response(dev, cmd, seq, timeout_us);
    if (err) {
        mt_logf(dev, MT_LOG_ERROR, "MCU %s (%s 0x%02x/0x%02x) seq %u: %s", name, fmt_name(cmd), cmd & MT_MCU_FIELD_ID,
                (cmd >> MT_MCU_FIELD_EXT_SHIFT) & 0xffu, seq, mt792x_strerror(err));
        return err;
    }
    if (is_result_byte_cmd(cmd)) res = dev->evt[MT_MCU_RXD_RESULT_BYTE];
    else if (is_uni_status_cmd(cmd)) res = (int)mt_get_le32(dev->evt + MT_MCU_RXD_LEN + 4u);
    if (result) *result = res;
    mt_logf(dev, MT_LOG_INFO, "MCU %s (%s 0x%02x/0x%02x) seq %u len %u: response ring %u, %u bytes, eid 0x%02x, result %d, payload %08x, %u us",
            name, fmt_name(cmd), cmd & MT_MCU_FIELD_ID, (cmd >> MT_MCU_FIELD_EXT_SHIFT) & 0xffu, seq, total,
            dev->evt_ring, dev->evt_len, dev->evt[MT_MCU_RXD_EID], res,
            dev->evt_len >= MT_MCU_RXD_LEN + 4u ? mt_get_le32(dev->evt + MT_MCU_RXD_LEN) : 0u,
            (uint32_t)(mt_now(dev) - t0));
    return 0;
}

static const char *bringup_name(uint8_t cid)
{
    switch (cid) {
    case MCU_CMD_TARGET_ADDRESS_LEN_REQ: return "TARGET_ADDRESS_LEN_REQ";
    case MCU_CMD_FW_START_REQ: return "FW_START_REQ";
    case MCU_CMD_NIC_POWER_CTRL: return "NIC_POWER_CTRL";
    case MCU_CMD_PATCH_START_REQ: return "PATCH_START_REQ";
    case MCU_CMD_PATCH_FINISH_REQ: return "PATCH_FINISH_REQ";
    case MCU_CMD_PATCH_SEM_CONTROL: return "PATCH_SEM_CONTROL";
    default: return "CMD";
    }
}

int mt_mcu_cmd(struct mt792x *dev, uint8_t cid, const uint8_t *payload, uint32_t payload_len,
               int want_resp, int *result_byte)
{
    int res = -1;
    int err = mt_mcu_send(dev, bringup_name(cid), MT_MCU_PLAIN(cid), payload, payload_len, want_resp, MT_MCU_TIMEOUT_US,
                          &res);
    if (result_byte) *result_byte = (!err && want_resp) ? res : -1;
    return err;
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
