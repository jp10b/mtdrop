/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * internal.h - private helpers shared by the library sources.
 * Freestanding only: <stdarg.h>, <stddef.h>, <stdint.h>.
 */
#ifndef MT792X_SRC_INTERNAL_H
#define MT792X_SRC_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "mt792x/mt792x.h"

/* device states */
#define MT_ST_NONE      0   /* not initialised */
#define MT_ST_READY     1   /* mt792x_init done, no register touched */
#define MT_ST_TOUCHED   2   /* mt792x_bringup has started touching registers */
#define MT_ST_SHUTDOWN  3

/* ---- byte helpers (wire formats are explicit, no packed structs) ---- */
static inline uint16_t mt_get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t mt_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t mt_get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t mt_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void mt_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void mt_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void mt_memset(void *dst, int value, size_t len);
void mt_memcpy(void *dst, const void *src, size_t len);

/* DMA memory shared with the chip: accessed through volatile pointers so the compiler neither
 * tears nor reorders the descriptor words (spec R13). Descriptor words are 4-byte aligned. */
uint32_t mt_dma_ld32(const struct mt792x *dev, uint32_t offset);
void mt_dma_st32(struct mt792x *dev, uint32_t offset, uint32_t value);
void mt_dma_read(const struct mt792x *dev, uint32_t offset, uint8_t *dst, uint32_t len);
void mt_dma_write(struct mt792x *dev, uint32_t offset, const uint8_t *src, uint32_t len);

/* ---- logging (own minimal formatter: %s %c %d %u %x %X, flags '0', width, 'll' for 64 bit) ---- */
void mt_logf(const struct mt792x *dev, enum mt_log_level level, const char *fmt, ...);
int mt_vformat(char *buf, size_t size, const char *fmt, va_list ap);
int mt_format(char *buf, size_t size, const char *fmt, ...);

/* ---- register access (regio.c) ---- */
uint32_t mt_rr(struct mt792x *dev, uint32_t bar);
void mt_wr(struct mt792x *dev, uint32_t bar, uint32_t value);
int mt_rmw(struct mt792x *dev, uint32_t bar, uint32_t clear, uint32_t set);
int mt_l1_map(struct mt792x *dev, uint32_t logical, uint32_t *bar);
int mt_l1_rr(struct mt792x *dev, uint32_t logical, uint32_t *value);
int mt_l1_rmw(struct mt792x *dev, uint32_t logical, uint32_t clear, uint32_t set);
uint64_t mt_now(const struct mt792x *dev);
void mt_sleep(const struct mt792x *dev, uint32_t us);
/* Poll (read & mask) == val; time-based. Returns 0, MT792X_E_BUS_DEAD or 1 (= timeout). */
#define MT_POLL_TIMEOUT 1
int mt_poll(struct mt792x *dev, uint32_t bar, uint32_t mask, uint32_t val,
            uint32_t timeout_us, uint32_t tick_us, uint32_t *last, uint64_t *elapsed_us);
/* Same, but the L1 window is re-mapped before every read (as mt76's __mt76_poll_msec does). */
int mt_poll_l1(struct mt792x *dev, uint32_t logical, uint32_t mask, uint32_t val,
               uint32_t timeout_us, uint32_t tick_us, uint32_t *last, uint64_t *elapsed_us);

/* ---- layout (layout.c) ---- */
int mt_layout_init(struct mt792x *dev);

/* ---- rings (ring.c) ---- */
int mt_rings_prepare_memory(struct mt792x *dev);
int mt_ring_program(struct mt792x *dev, struct mt792x_ring *r);
int mt_ring_rx_kick_initial(struct mt792x *dev, struct mt792x_ring *r);
int mt_ring_tx_raw(struct mt792x *dev, struct mt792x_ring *r, const uint8_t *hdr, uint32_t hdr_len,
                   const uint8_t *data, uint32_t data_len);
int mt_ring_tx_wait(struct mt792x *dev, struct mt792x_ring *r, uint32_t timeout_us);
/* Takes one completed descriptor from an RX ring, copies it into dev->evt, re-arms one slot.
 * Returns 1 if an event was taken, 0 if none is pending, < 0 on error. */
int mt_ring_rx_take(struct mt792x *dev, struct mt792x_ring *r);
/* Same into any buffer; kick = 0 defers the cpu_idx write until mt_ring_rx_flush. */
int mt_ring_rx_take_buf(struct mt792x *dev, struct mt792x_ring *r, uint8_t *dst, uint32_t cap, uint32_t *out_len,
                        int kick);
int mt_ring_rx_flush(struct mt792x *dev, struct mt792x_ring *r);
uint32_t mt_ring_rx_completed(const struct mt792x *dev, const struct mt792x_ring *r);

/* ---- MCU (mcu.c) ---- */
#define MT_MCU_TIMEOUT_US   3000000u   /* mt7921/pci_mcu.c: mcu.timeout = 3 * HZ */
#define MT_MCU_SHUTDOWN_TIMEOUT_US 500000u /* [HOST] best-effort commands during shutdown */
#define MT_TX_DONE_TIMEOUT_US 1000000u /* ASSUMPTION: mt76 has no TX-done timeout (it reaps lazily) */
uint8_t mt_mcu_next_seq(struct mt792x *dev);
/* Sends one command. 'cmd' is encoded like mt76 (MT_MCU_CMD / _EXT / _UNI / _CE, regs.h): UNI
 * commands get the 48-byte header, all others the 64-byte one (mt76_connac2_mcu_fill_message).
 * Waits for TX consumption; if wait, waits up to timeout_us for the response with the same seq
 * (stored in dev->evt). *result: byte 0x20 for PATCH_SEM_CONTROL / PATCH_FINISH_REQ, the le32
 * status for the UNI status commands (mt7921_mcu_parse_response), else 0. Logged at INFO. */
int mt_mcu_send(struct mt792x *dev, const char *name, uint32_t cmd, const uint8_t *payload, uint32_t payload_len,
                int wait, uint32_t timeout_us, int *result);
/* Plain MCU_CMD(cid) with the 3 s timeout (bring-up commands). */
int mt_mcu_cmd(struct mt792x *dev, uint8_t cid, const uint8_t *payload, uint32_t payload_len,
               int want_resp, int *result_byte);
int mt_mcu_send_firmware(struct mt792x *dev, const uint8_t *data, uint32_t len);
/* Counts / logs one MCU event that is not (or no longer) awaited. */
void mt_mcu_note_event(struct mt792x *dev, const uint8_t *e, uint32_t len, uint32_t ring, int solicited);

/* ---- RX (rx.c) ---- */
/* Handles one packet taken from a data ring while no frame is wanted (command waits): counts it. */
void mt_rx_account_packet(struct mt792x *dev, const uint8_t *p, uint32_t len, uint32_t ring, int in_cmd);

/* ---- step bookkeeping (bringup.c) ---- */
int mt_step_fail(struct mt792x *dev, int step, int err, uint64_t t0);
void mt_step_done(struct mt792x *dev, int step);

/* ---- post-boot / sniffer (postboot.c) ---- */
int mt_postboot(struct mt792x *dev);
void mt_sniffer_shutdown(struct mt792x *dev);

#endif /* MT792X_SRC_INTERNAL_H */
