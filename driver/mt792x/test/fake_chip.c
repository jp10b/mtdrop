/* SPDX-License-Identifier: GPL-3.0-or-later */
/* fake_chip.c - simulated MT7921, see fake_chip.h. Test code: libc allowed. */
#include "fake_chip.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* register map (kept independent from src/regs.h on purpose: the fake encodes the spec) */
#define R_REMAP        0xfe24cu
#define R_LPCTL        0xe0010u
#define R_MISC         0xe00f0u
#define R_INT_ENA      0xd4204u
#define R_GLO          0xd4208u
#define R_DTX_PTR      0xd420cu
#define R_RST          0xd4100u
#define R_DUMMY        0x02120u
#define R_MCU2HOST     0xd41f4u
#define R_PCIE_INT     0x10188u
#define R_TX_BASE      0xd4300u
#define R_RX_BASE      0xd4500u
#define L1_WIN         0x40000u
#define L1_CHIPID      0x70010200u
#define L1_BOUND       0x70010020u
#define L1_REV         0x70010204u
#define L1_WFSYS       0x18000140u
#define L1_TOP_LPCR    0x18060010u

#define GLO_TX_EN      0x1u
#define GLO_RX_EN      0x4u
#define GLO_TX_WB      0x40u
#define GLO_RX_WB      0x2000u

static void violation(struct fake_chip *f, const char *fmt, ...)
{
    va_list ap;
    if (f->violations++ == 0) {
        va_start(ap, fmt);
        vsnprintf(f->first_violation, sizeof(f->first_violation), fmt, ap);
        va_end(ap);
    }
    if (f->m->verbose) {
        char buf[256];
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fprintf(stderr, "[fake VIOLATION] %s\n", buf);
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint64_t now(const struct fake_chip *f) { return f->m->clock_us; }

static void mark_enabling(struct fake_chip *f)
{
    if (f->n_enabling < FAKE_MAX_ENABLING) f->enabling_idx[f->n_enabling++] = f->m->trace_len ? f->m->trace_len - 1u : 0u;
}

static void mark_fault(struct fake_chip *f)
{
    if (!f->fault_idx) f->fault_idx = f->m->trace_len;
}

/* Translate a bus address handed to us by the host into the HAL region. */
static uint8_t *dma_ptr(struct fake_chip *f, uint32_t bus, uint32_t len, uint32_t align, const char *what)
{
    const struct mt_hal *h = &f->m->hal;
    uint64_t end = (uint64_t)bus + len;
    if (!h->dma_virt || len == 0 || bus < h->dma_bus || end > (uint64_t)h->dma_bus + h->dma_len || (bus % align) != 0) {
        violation(f, "%s: bus address 0x%08x len %u align %u outside region 0x%08x+0x%x", what, bus, len, align,
                  h->dma_bus, h->dma_len);
        return NULL;
    }
    return h->dma_virt + (bus - h->dma_bus);
}

static uint32_t glo(const struct fake_chip *f) { return mock_get_reg(f->m, R_GLO); }
uint32_t fake_glo_cfg(const struct fake_chip *f) { return glo(f) & ~0xau; }

static int wfsys_init_done(const struct fake_chip *f)
{
    if (f->wfsys_asserted || f->wfsys_never_done) return 0;
    if (!f->wfsys_released_once) return 1; /* idle chip as found */
    return now(f) >= f->wfsys_release_time + f->wfsys_done_delay_us;
}

uint32_t fake_misc(const struct fake_chip *f)
{
    if (!wfsys_init_done(f)) return 0;
    if (f->fw_started && !f->n9_never) return 3;
    if (f->fw_state_never_ready) return 0;
    return 1;
}

static int tx_ring_known(uint32_t n) { return n == 0 || n == 16 || n == 17; }
static int rx_ring_known(uint32_t n) { return n == 0 || n == 2 || n == 4; }
static uint32_t ring_reg(int rx, uint32_t n, uint32_t which) { return (rx ? R_RX_BASE : R_TX_BASE) + n * 0x10u + which; }
static uint32_t ring_size(const struct fake_chip *f, int rx, uint32_t n) { return mock_get_reg(f->m, ring_reg(rx, n, 4)); }
static uint32_t ring_base(const struct fake_chip *f, int rx, uint32_t n) { return mock_get_reg(f->m, ring_reg(rx, n, 0)); }

/* ---- events ---- */

static void emit(struct fake_chip *f, uint32_t ring, const uint8_t *ev, uint32_t len)
{
    uint32_t size = ring_size(f, 1, ring), slot, ctrl, buflen, bufbus;
    uint8_t *d, *buf;

    if (!(glo(f) & GLO_RX_EN) || !size || f->rx_dma[ring] == f->rx_cpu[ring]) {
        f->events_dropped++;
        return;
    }
    slot = f->rx_dma[ring];
    d = dma_ptr(f, ring_base(f, 1, ring) + slot * 16u, 16, 4, "RX descriptor");
    if (!d) return;
    ctrl = le32(d + 4);
    if (ctrl & 0x80000000u) {
        violation(f, "RX ring %u slot %u handed to the chip with DMA_DONE set (not armed)", ring, slot);
        f->events_dropped++;
        return;
    }
    buflen = (ctrl >> 16) & 0x3fffu;
    if (buflen < len || le32(d + 8) != 0 || le32(d + 12) != 0 || (ctrl & 0x4000ffffu)) {
        violation(f, "RX ring %u slot %u malformed: ctrl 0x%08x buf1 0x%08x info 0x%08x (event %u bytes)", ring, slot,
                  ctrl, le32(d + 8), le32(d + 12), len);
        f->events_dropped++;
        return;
    }
    bufbus = le32(d);
    buf = dma_ptr(f, bufbus, buflen, 4, "RX buffer");
    if (!buf) return;
    memcpy(buf, ev, len);
    ctrl = (len << 16) | 0x40000000u;
    if (glo(f) & GLO_RX_WB) ctrl |= 0x80000000u;
    put32(d + 4, ctrl);
    f->rx_dma[ring] = (slot + 1u) % size;
    f->events_sent[ring]++;
}

static void emit_response(struct fake_chip *f, uint32_t ring, uint8_t eid, uint8_t seq, uint8_t result, uint32_t total)
{
    uint8_t ev[64];
    memset(ev, 0, sizeof(ev));
    put32(ev, total | (7u << 27));          /* PKT_TYPE_RX_EVENT, flag 0 */
    ev[0x18] = (uint8_t)total;
    ev[0x1c] = eid;
    ev[0x1d] = seq;
    ev[0x20] = result;                      /* result byte for SEM/FINISH */
    emit(f, ring, ev, total);
}

static void respond(struct fake_chip *f, uint32_t ring, uint8_t eid, uint8_t seq, uint8_t result)
{
    if (f->drop_response_nth && f->cmd_count == f->drop_response_nth) {
        f->responses_dropped_by_knob++;
        mark_fault(f);
        return;
    }
    if (f->inject_unsolicited) emit_response(f, ring, 0x27, 0, 0, 40);         /* DBG_MSG, seq 0 */
    if (f->inject_stale) emit_response(f, ring, 0x01, (uint8_t)((seq + 13u) % 15u + 1u), 0x55, 40); /* previous seq */
    if (f->short_response_nth && f->cmd_count == f->short_response_nth) {
        mark_fault(f);
        emit_response(f, ring, eid, seq, result, 33);
        return;
    }
    emit_response(f, ring, eid, seq, result, 40);
}

/* ---- MCU ---- */

static uint8_t next_seq(struct fake_chip *f)
{
    uint8_t s = (uint8_t)(++f->seq_counter & 0xfu);
    if (!s) s = (uint8_t)(++f->seq_counter & 0xfu);
    return s;
}

static void log_cmd(struct fake_chip *f, uint8_t cid, uint8_t seq, uint32_t len, uint32_t ring)
{
    if (f->n_cmds < FAKE_MAX_CMDS) {
        f->cmds[f->n_cmds].cid = cid;
        f->cmds[f->n_cmds].seq = seq;
        f->cmds[f->n_cmds].total_len = len;
        f->cmds[f->n_cmds].ring = ring;
    }
    f->n_cmds++;
}

static struct fake_dl *cur_dl(struct fake_chip *f)
{
    return f->n_dl ? &f->dl[f->n_dl - 1u] : NULL;
}

static int dl_incomplete(struct fake_chip *f)
{
    struct fake_dl *d = cur_dl(f);
    return d && d->got != d->len;
}

static void mcu_command(struct fake_chip *f, const uint8_t *p, uint32_t len)
{
    uint32_t i, plen;
    uint8_t cid, seq, expect;
    const uint8_t *pl = p + 64;

    f->cmd_count++;
    if (len < 64) {
        violation(f, "MCU command of %u bytes is shorter than the 64-byte header", len);
        return;
    }
    plen = len - 64u;
    if ((le32(p) & 0xffffu) != len) violation(f, "TXD0 TX_BYTES %u != descriptor length %u", le32(p) & 0xffffu, len);
    if (((le32(p) >> 23) & 3u) != 2u) violation(f, "TXD0 PKT_FMT %u (want 2 = CMD)", (le32(p) >> 23) & 3u);
    if ((le32(p) >> 25) != 0x20u) violation(f, "TXD0 Q_IDX 0x%x (want 0x20)", le32(p) >> 25);
    if (le32(p) & 0x007f0000u) violation(f, "TXD0 reserved bits set: 0x%08x", le32(p));
    if (le32(p + 4) != 0x80010000u) violation(f, "TXD1 0x%08x (want 0x80010000)", le32(p + 4));
    for (i = 8; i < 32; i++)
        if (p[i]) violation(f, "TXD byte 0x%x non-zero", i);
    if ((uint32_t)(p[0x20] | (p[0x21] << 8)) != len - 32u) violation(f, "len field %u (want %u)", p[0x20] | (p[0x21] << 8), len - 32u);
    if ((p[0x22] | (p[0x23] << 8)) != 0x8000) violation(f, "pq_id 0x%04x (want 0x8000)", p[0x22] | (p[0x23] << 8));
    cid = p[0x24];
    if (p[0x25] != 0xa0) violation(f, "pkt_type 0x%02x (want 0xa0)", p[0x25]);
    if (p[0x26] != 3) violation(f, "cmd 0x%02x set_query %u (want 3 = MCU_Q_NA)", cid, p[0x26]);
    seq = p[0x27];
    expect = next_seq(f);
    if (seq != expect) {
        /* After an injected fault (e.g. a stalled FWDL ring) the chip never saw some chunks whose
         * sequence numbers the host consumed: resynchronise instead of reporting. */
        if (f->fault_idx) f->seq_counter = seq;
        else violation(f, "cmd 0x%02x seq %u, expected %u", cid, seq, expect);
    }
    if (p[0x28] || p[0x29] || p[0x2a] || p[0x2b]) violation(f, "cmd 0x%02x: rev/ext_cid/s2d/ext_cid_ack not 0", cid);
    for (i = 0x2c; i < 0x40; i++)
        if (p[i]) violation(f, "TXD rsv byte 0x%x non-zero", i);
    log_cmd(f, cid, seq, len, 17);

    switch (cid) {
    case 0x04: /* NIC_POWER_CTRL */
        if (plen != 4 || pl[0] != 1 || pl[1] || pl[2] || pl[3]) violation(f, "NIC_POWER_CTRL payload wrong");
        if (f->n_dl || f->sem_held) violation(f, "NIC_POWER_CTRL after download start");
        f->nic_powered = 1;
        if (f->nic_power_responds) respond(f, f->event_ring, 0x01, seq, 0);
        break;
    case 0x10: { /* PATCH_SEM_CONTROL */
        uint32_t op;
        int result;
        if (plen != 4) {
            violation(f, "PATCH_SEM_CONTROL payload %u bytes", plen);
            break;
        }
        op = le32(pl);
        if (!f->nic_powered) violation(f, "semaphore before NIC_POWER_CTRL");
        if (op == 1 && dl_incomplete(f)) violation(f, "semaphore get while a download is incomplete");
        if (op == 1) {
            if (f->sem_get_result >= 0) {
                result = f->sem_get_result;
                mark_fault(f);
            } else if (f->patch_loaded) {
                result = 1;
            } else {
                result = f->sem_held ? 0 : 2;
            }
            if (result == 2) f->sem_held = 1;
        } else if (op == 0) {
            if (f->sem_release_result >= 0) {
                result = f->sem_release_result;
                mark_fault(f);
            } else {
                result = f->sem_held ? 3 : 0;
                if (!f->sem_held) violation(f, "semaphore released but not held");
            }
            f->sem_held = 0;
        } else {
            violation(f, "semaphore op %u", op);
            result = 0;
        }
        respond(f, f->event_ring, 0x04, seq, (uint8_t)result);
        break;
    }
    case 0x05:
    case 0x01: { /* PATCH_START_REQ / TARGET_ADDRESS_LEN_REQ */
        struct fake_dl *d;
        uint32_t addr, dlen, mode;
        int is_patch_addr;
        if (plen != 12) {
            violation(f, "init download payload %u bytes", plen);
            break;
        }
        addr = le32(pl);
        dlen = le32(pl + 4);
        mode = le32(pl + 8);
        is_patch_addr = addr == 0x200000u || addr == 0x900000u;
        if ((cid == 0x05) != is_patch_addr) violation(f, "init download addr 0x%08x with cmd 0x%02x", addr, cid);
        if (dl_incomplete(f)) violation(f, "init download while the previous one is incomplete");
        if (!(mode & 0x80000000u)) violation(f, "init download mode 0x%08x without NEED_RSP", mode);
        if (dlen == 0 || dlen > 16u * 1024u * 1024u) violation(f, "init download len %u", dlen);
        if (f->n_dl >= FAKE_MAX_DL) {
            violation(f, "too many downloads");
            break;
        }
        d = &f->dl[f->n_dl++];
        memset(d, 0, sizeof(*d));
        if (f->sem_held && !f->patch_done) d->phase = 0;
        else if (f->patch_done || f->patch_loaded) d->phase = 1;
        else violation(f, "init download 0x%08x without semaphore or loaded patch", addr);
        if (d->phase == 1 && f->sem_held) violation(f, "RAM download while the semaphore is still held");
        d->cmd = cid;
        d->addr = addr;
        d->len = dlen;
        d->mode = mode;
        d->data = (uint8_t *)calloc(1, dlen ? dlen : 1u);
        respond(f, f->event_ring, 0x01, seq, 0);
        break;
    }
    case 0x07: /* PATCH_FINISH_REQ */
        if (plen != 4 || pl[0] || pl[1] || pl[2] || pl[3]) violation(f, "PATCH_FINISH_REQ payload wrong");
        if (!f->sem_held) violation(f, "PATCH_FINISH without semaphore");
        if (dl_incomplete(f)) violation(f, "PATCH_FINISH with an incomplete download");
        if (f->finish_result) mark_fault(f);
        else f->patch_done = 1;
        respond(f, f->event_ring, 0x01, seq, (uint8_t)f->finish_result);
        break;
    case 0x02: /* FW_START_REQ */
        if (plen != 8) {
            violation(f, "FW_START_REQ payload %u bytes", plen);
            break;
        }
        if (!(f->patch_done || f->patch_loaded)) violation(f, "FW_START before the patch is in place");
        if (f->sem_held) violation(f, "FW_START with the semaphore held");
        if (!f->n_dl || cur_dl(f)->phase != 1 || dl_incomplete(f)) violation(f, "FW_START without complete RAM download");
        f->fw_start_option = le32(pl);
        f->fw_start_addr = le32(pl + 4);
        f->fw_started = 1;
        respond(f, f->fw_start_ring, 0x01, seq, 0);
        break;
    default:
        violation(f, "unknown MCU command 0x%02x", cid);
        break;
    }
}

static void fw_chunk(struct fake_chip *f, const uint8_t *p, uint32_t len)
{
    struct fake_dl *d = cur_dl(f);
    uint32_t remaining, expect;
    (void)next_seq(f); /* the host consumes one sequence number per chunk */
    log_cmd(f, 0xee, 0, len, 16);
    if (!d || d->got >= d->len) {
        violation(f, "FW_SCATTER chunk of %u bytes without an open download", len);
        return;
    }
    remaining = d->len - d->got;
    expect = remaining < 4096u ? remaining : 4096u;
    if (len != expect) {
        violation(f, "FW_SCATTER chunk %u bytes, expected %u (remaining %u)", len, expect, remaining);
        if (len > remaining) return;
    }
    memcpy(d->data + d->got, p, len);
    d->got += len;
    d->chunks++;
}

/* ---- TX processing ---- */

static void tx_kick(struct fake_chip *f, uint32_t n, uint32_t cpu)
{
    uint32_t size = ring_size(f, 0, n), base = ring_base(f, 0, n);

    if (cpu == f->tx_dma[n]) return;
    mark_enabling(f);
    f->tx_kicks[n]++;
    if (!(glo(f) & GLO_TX_EN)) {
        violation(f, "TX ring %u kicked (cpu_idx %u) while TX DMA is disabled", n, cpu);
        return;
    }
    if (!size) {
        violation(f, "TX ring %u kicked before it was programmed", n);
        return;
    }
    if ((int)n == f->tx_stall_ring) {
        mark_fault(f);
        return;
    }
    while (f->tx_dma[n] != cpu) {
        uint32_t idx = f->tx_dma[n], ctrl, len, buf0;
        uint8_t *d = dma_ptr(f, base + idx * 16u, 16, 4, "TX descriptor");
        uint8_t *data;
        if (!d) return;
        buf0 = le32(d);
        ctrl = le32(d + 4);
        len = (ctrl >> 16) & 0x3fffu;
        if (ctrl & 0x80000000u) violation(f, "TX ring %u slot %u posted with DMA_DONE set", n, idx);
        if (!(ctrl & 0x40000000u)) violation(f, "TX ring %u slot %u without LAST_SEC0", n, idx);
        if (ctrl & 0xffffu) violation(f, "TX ring %u slot %u uses segment 1 (ctrl 0x%08x)", n, idx, ctrl);
        if (le32(d + 8) || le32(d + 12)) violation(f, "TX ring %u slot %u buf1/info not 0", n, idx);
        if (len == 0) violation(f, "TX ring %u slot %u zero length", n, idx);
        data = len ? dma_ptr(f, buf0, len, 4, "TX buffer") : NULL;
        if (data) {
            if (n == 17) mcu_command(f, data, len);
            else if (n == 16) fw_chunk(f, data, len);
            else violation(f, "data frame on TX ring %u during bring-up", n);
        }
        if (glo(f) & GLO_TX_WB) put32(d + 4, ctrl | 0x80000000u);
        f->tx_dma[n] = (idx + 1u) % size;
    }
}

/* At the moment DMA gets enabled every programmed ring must be fully initialised. */
static void check_rings_at_enable(struct fake_chip *f)
{
    static const uint32_t tx[3] = { 0, 16, 17 }, rx[3] = { 0, 2, 4 };
    int k, rxs;
    uint32_t i;
    for (rxs = 0; rxs < 2; rxs++) {
        for (k = 0; k < 3; k++) {
            uint32_t n = rxs ? rx[k] : tx[k], size = ring_size(f, rxs, n);
            uint8_t *descs;
            if (!size) {
                violation(f, "DMA enabled but %s ring %u not programmed", rxs ? "RX" : "TX", n);
                continue;
            }
            descs = dma_ptr(f, ring_base(f, rxs, n), size * 16u, 16, "descriptor array at enable");
            if (!descs) continue;
            for (i = 0; i < size; i++) {
                const uint8_t *d = descs + i * 16u;
                uint32_t ctrl = le32(d + 4);
                if (!dma_ptr(f, le32(d), 1, 4, "descriptor buf0 at enable")) break;
                if (!rxs && !(ctrl & 0x80000000u)) violation(f, "TX ring %u slot %u not idle at enable (ctrl 0x%08x)", n, i, ctrl);
                if (rxs) {
                    uint32_t c = f->rx_cpu[n], dma = f->rx_dma[n];
                    int armed = (c >= dma) ? (i >= dma && i < c) : (i >= dma || i < c);
                    if (armed && ((ctrl & 0x80000000u) || ((ctrl >> 16) & 0x3fffu) == 0))
                        violation(f, "RX ring %u slot %u not armed at enable (ctrl 0x%08x)", n, i, ctrl);
                    if (armed && !dma_ptr(f, le32(d), (ctrl >> 16) & 0x3fffu, 4, "RX buffer at enable")) break;
                }
            }
        }
    }
}

/* ---- L1 window ---- */

static uint32_t l1_read(struct fake_chip *f, uint32_t logical)
{
    switch (logical) {
    case L1_CHIPID: return f->chip_id;
    case L1_BOUND: return f->hw_bound;
    case L1_REV: return f->hw_rev;
    case L1_WFSYS: return (f->wfsys_asserted ? 0u : 1u) | (wfsys_init_done(f) ? 0x10u : 0u);
    case L1_TOP_LPCR: return f->top_lpcr_fw_own ? 1u : 0u;
    default:
        violation(f, "L1 read of unmodelled address 0x%08x", logical);
        return 0;
    }
}

static void l1_write(struct fake_chip *f, uint32_t logical, uint32_t v)
{
    switch (logical) {
    case L1_WFSYS:
        if (!(v & 1u) && !f->wfsys_asserted) {
            f->wfsys_asserted = 1;
            f->wfsys_assert_time = now(f);
            /* the Wi-Fi subsystem (MCU) is reset: firmware state is lost */
            f->sem_held = 0;
            f->patch_done = 0;
            f->fw_started = 0;
            f->nic_powered = 0;
            f->top_lpcr_fw_own = 1;
        } else if ((v & 1u) && f->wfsys_asserted) {
            if (now(f) - f->wfsys_assert_time < 50000u)
                violation(f, "WFSYS reset released after %u us (< 50 ms)", (unsigned)(now(f) - f->wfsys_assert_time));
            f->wfsys_asserted = 0;
            f->wfsys_released_once = 1;
            f->wfsys_release_time = now(f);
            f->wfsys_resets++;
        }
        break;
    case L1_TOP_LPCR:
        if (v & 2u) {
            if (!f->drv_own2_never) f->top_lpcr_fw_own = 0;
        } else {
            violation(f, "TOP_LPCR_HOST_BAND0 write 0x%08x", v);
        }
        break;
    default:
        violation(f, "L1 write of unmodelled address 0x%08x = 0x%08x", logical, v);
        break;
    }
}

static uint32_t l1_logical(const struct fake_chip *f, uint32_t off)
{
    return ((mock_get_reg(f->m, R_REMAP) & 0xffffu) << 16) | (off - L1_WIN);
}

/* ---- hooks ---- */

static int bus_tick(struct fake_chip *f)
{
    f->accesses++;
    if (f->bus_dead_after && f->accesses > f->bus_dead_after && !f->dead) {
        f->dead = 1;
        mark_fault(f);
    }
    return f->dead;
}

static void on_read(void *user, uint32_t off, uint32_t *value)
{
    struct fake_chip *f = (struct fake_chip *)user;
    uint32_t n;

    if (bus_tick(f)) {
        *value = 0xffffffffu;
        f->dead_read_seen = 1;
        return;
    }
    if (off == R_LPCTL) {
        *value = f->own_sync ? 4u : 0u;
        if ((f->fw_own_never && f->set_own_writes && !f->own_sync) || (f->drv_own_never && f->clr_own_requests))
            mark_fault(f);
    } else if (off == R_MISC) {
        *value = fake_misc(f);
        if (f->fw_state_never_ready && wfsys_init_done(f) && !f->fw_started) mark_fault(f);
        if (f->n9_never && f->fw_started) mark_fault(f);
    } else if (off >= L1_WIN && off < L1_WIN + 0x10000u) {
        uint32_t logical = l1_logical(f, off);
        *value = l1_read(f, logical);
        if ((logical == L1_CHIPID && f->chip_id != 0x7961u) || (logical == L1_BOUND && (f->hw_bound & 0x80u)) ||
            (logical == L1_WFSYS && f->wfsys_never_done && f->wfsys_resets) ||
            (logical == L1_TOP_LPCR && f->drv_own2_never))
            mark_fault(f);
    } else if (off == R_GLO) {
        *value = (*value & ~0xau) | (f->dma_busy_stuck ? 0x2u : 0u);
        if (f->dma_busy_stuck) mark_fault(f);
    } else if (off >= R_TX_BASE && off < R_TX_BASE + 18u * 0x10u && (off & 0xfu) == 0xcu) {
        n = (off - R_TX_BASE) / 0x10u;
        *value = f->tx_dma[n];
    } else if (off >= R_RX_BASE && off < R_RX_BASE + 8u * 0x10u && (off & 0xfu) == 0xcu) {
        n = (off - R_RX_BASE) / 0x10u;
        *value = f->rx_dma[n];
    }
}

static void on_write(void *user, uint32_t off, uint32_t v)
{
    struct fake_chip *f = (struct fake_chip *)user;
    uint32_t n, which;

    if (bus_tick(f)) {
        if (f->dead_read_seen) f->writes_after_dead_read++;
        return;
    }
    if (off == R_LPCTL) {
        if (v & 1u) {
            f->set_own_writes++;
            if (f->driver_owned_once) violation(f, "SET_OWN written after the driver took ownership");
            if (!f->fw_own_never) f->own_sync = 1;
        }
        if (v & 2u) {
            f->clr_own_requests++;
            if (!f->drv_own_never && f->clr_own_requests > f->drv_own_fail_count) {
                f->own_sync = 0;
                f->driver_owned_once = 1;
            }
        }
        if (v & ~3u) violation(f, "LPCTL write 0x%08x", v);
    } else if (off == R_REMAP) {
        if ((v & 0xffff0000u) != (f->remap_initial & 0xffff0000u)) violation(f, "remap high half changed: 0x%08x", v);
    } else if (off >= L1_WIN && off < L1_WIN + 0x10000u) {
        l1_write(f, l1_logical(f, off), v);
    } else if (off == R_INT_ENA) {
        if (v) violation(f, "HOST_INT_ENA written 0x%08x (non-zero)", v);
    } else if (off == R_GLO) {
        if (v & (GLO_TX_EN | GLO_RX_EN)) {
            mark_enabling(f);
            if ((v & (GLO_TX_EN | GLO_RX_EN)) != (GLO_TX_EN | GLO_RX_EN))
                violation(f, "GLO_CFG enables only one direction: 0x%08x", v);
            check_rings_at_enable(f);
        }
    } else if (off == R_RST) {
        if ((v & 0x30u) != 0x30u) {
            memset(f->tx_dma, 0, sizeof(f->tx_dma));
            memset(f->rx_dma, 0, sizeof(f->rx_dma));
        }
    } else if (off == R_DTX_PTR) {
        mark_enabling(f);
        for (n = 0; n < 18; n++)
            if (v & (1u << n)) f->tx_dma[n] = 0;
    } else if ((off >= 0xd4600u && off < 0xd4700u) || off == R_DUMMY || off == R_MCU2HOST) {
        mark_enabling(f);
    } else if (off >= R_TX_BASE && off < R_TX_BASE + 32u * 0x10u) {
        n = (off - R_TX_BASE) / 0x10u;
        which = off & 0xfu;
        if (!tx_ring_known(n)) {
            violation(f, "write to TX ring %u register 0x%05x", n, off);
            return;
        }
        if (which == 0 || which == 4) {
            mark_enabling(f);
            if (glo(f) & GLO_TX_EN) violation(f, "TX ring %u reprogrammed while DMA is enabled", n);
        } else if (which == 8) {
            if (ring_size(f, 0, n) && v >= ring_size(f, 0, n)) violation(f, "TX ring %u cpu_idx %u >= size", n, v);
            else tx_kick(f, n, v);
        } else {
            if (v) violation(f, "TX ring %u dma_idx written %u", n, v);
            f->tx_dma[n] = v;
        }
    } else if (off >= R_RX_BASE && off < R_RX_BASE + 8u * 0x10u) {
        n = (off - R_RX_BASE) / 0x10u;
        which = off & 0xfu;
        if (!rx_ring_known(n)) {
            violation(f, "write to RX ring %u register 0x%05x", n, off);
            return;
        }
        if (which == 0 || which == 4) {
            mark_enabling(f);
            if (glo(f) & GLO_RX_EN) violation(f, "RX ring %u reprogrammed while DMA is enabled", n);
        } else if (which == 8) {
            if (ring_size(f, 1, n) && v >= ring_size(f, 1, n)) violation(f, "RX ring %u cpu_idx %u >= size", n, v);
            else if (!f->rx_never_armed) f->rx_cpu[n] = v;
            else mark_fault(f);
        } else {
            if (v) violation(f, "RX ring %u dma_idx written %u", n, v);
            f->rx_dma[n] = v;
        }
    }
}

void fake_init(struct fake_chip *f, struct mock *m)
{
    memset(f, 0, sizeof(*f));
    f->m = m;
    f->chip_id = 0x7961u;
    f->hw_bound = 0;
    f->hw_rev = 0x8a10u;
    f->remap_initial = 0x18451800u;
    f->wfsys_done_delay_us = 12000u;
    f->sem_get_result = -1;
    f->sem_release_result = -1;
    f->fw_start_ring = 4;
    f->event_ring = 0;
    f->tx_stall_ring = -1;
    f->top_lpcr_fw_own = 1;

    mock_set_reg(m, R_REMAP, f->remap_initial);
    mock_set_reg(m, R_GLO, 0x1010b870u);
    mock_set_reg(m, R_INT_ENA, 0);
    mock_set_reg(m, R_PCIE_INT, 0xffu);
    mock_set_reg(m, R_RST, 0x30u);
    m->on_read = on_read;
    m->on_write = on_write;
    m->hook_user = f;
}

void fake_free(struct fake_chip *f)
{
    unsigned i;
    for (i = 0; i < f->n_dl && i < FAKE_MAX_DL; i++) free(f->dl[i].data);
    f->n_dl = 0;
}

const struct fake_dl *fake_find_dl(const struct fake_chip *f, uint32_t addr)
{
    unsigned i;
    for (i = 0; i < f->n_dl; i++)
        if (f->dl[i].addr == addr) return &f->dl[i];
    return NULL;
}

unsigned fake_enabling_after(const struct fake_chip *f, uint32_t idx)
{
    unsigned i, n = 0;
    for (i = 0; i < f->n_enabling; i++)
        if (f->enabling_idx[i] > idx) n++;
    return n;
}
