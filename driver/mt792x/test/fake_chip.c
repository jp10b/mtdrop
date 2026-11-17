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
#define R_INT_STA      0xd4200u
#define R_TX_BASE      0xd4300u
#define R_RX_BASE      0xd4500u
#define L1_WIN         0x40000u
#define L1_CHIPID      0x70010200u
#define L1_BOUND       0x70010020u
#define L1_REV         0x70010204u
#define L1_WFSYS       0x18000140u
#define L1_TOP_LPCR    0x18060010u

#define R_WTBL_UPDATE  0x34230u

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
    if (h->dma_page_bus && h->dma_virt && len != 0 && (bus % align) == 0) {
        /* Paged region: the object must lie inside one of the host's pages. */
        uint32_t i, pages = h->dma_len / 4096u, in_page = bus % 4096u;
        if (in_page + len <= 4096u)
            for (i = 0; i < pages; i++)
                if (h->dma_page_bus[i] == bus - in_page) return h->dma_virt + (size_t)i * 4096u + in_page;
        violation(f, "%s: bus address 0x%08x len %u is not inside one page of the paged region", what, bus, len);
        return NULL;
    }
    if (!h->dma_virt || len == 0 || bus < h->dma_bus || end > (uint64_t)h->dma_bus + h->dma_len || (bus % align) != 0) {
        violation(f, "%s: bus address 0x%08x len %u align %u outside region 0x%08x+0x%x", what, bus, len, align,
                  h->dma_bus, h->dma_len);
        return NULL;
    }
    return h->dma_virt + (bus - h->dma_bus);
}

/* registers of mt7921_mac_init / mt792x_mac_init_band after fixed-map translation */
static int is_mac_reg(uint32_t off)
{
    static const uint32_t regs[] = { 0x0f000u, 0x0f004u, R_WTBL_UPDATE, 0x210f4u, 0xa10f4u, 0x217c4u, 0xa17c4u, 0x21780u,
                                     0xa1780u, 0x24804u, 0xa4804u, 0x21e00u, 0xa1e00u, 0x23408u, 0xa3408u,
                                     0x21090u, 0x21094u, 0x210a4u, 0x20884u, 0x20c80u };
    unsigned i;
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        if (regs[i] == off) return 1;
    return 0;
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

/* Writes one descriptor completion into an armed RX slot, like the chip's DMA. 'claim' != 0 writes
 * that length into ctrl without data (bad-length injection). Returns 1 if written. */
static int emit_desc(struct fake_chip *f, uint32_t ring, const uint8_t *ev, uint32_t len, int last, uint32_t claim)
{
    uint32_t size = ring_size(f, 1, ring), slot, ctrl, buflen, bufbus;
    uint8_t *d, *buf;

    if (!(glo(f) & GLO_RX_EN) || !size || f->rx_dma[ring] == f->rx_cpu[ring]) {
        f->events_dropped++;
        return 0;
    }
    slot = f->rx_dma[ring];
    d = dma_ptr(f, ring_base(f, 1, ring) + slot * 16u, 16, 4, "RX descriptor");
    if (!d) return 0;
    ctrl = le32(d + 4);
    if (ctrl & 0x80000000u) {
        violation(f, "RX ring %u slot %u handed to the chip with DMA_DONE set (not armed)", ring, slot);
        f->events_dropped++;
        return 0;
    }
    buflen = (ctrl >> 16) & 0x3fffu;
    if ((!claim && buflen < len) || le32(d + 8) != 0 || le32(d + 12) != 0 || (ctrl & 0x4000ffffu)) {
        violation(f, "RX ring %u slot %u malformed: ctrl 0x%08x buf1 0x%08x info 0x%08x (event %u bytes)", ring, slot,
                  ctrl, le32(d + 8), le32(d + 12), len);
        f->events_dropped++;
        return 0;
    }
    bufbus = le32(d);
    buf = dma_ptr(f, bufbus, buflen, 4, "RX buffer");
    if (!buf) return 0;
    if (!claim) memcpy(buf, ev, len);
    ctrl = ((claim ? claim : len) & 0x3fffu) << 16;
    if (last) ctrl |= 0x40000000u;
    if (glo(f) & GLO_RX_WB) ctrl |= 0x80000000u;
    put32(d + 4, ctrl);
    f->rx_dma[ring] = (slot + 1u) % size;
    f->events_sent[ring]++;
    return 1;
}

static void emit(struct fake_chip *f, uint32_t ring, const uint8_t *ev, uint32_t len)
{
    (void)emit_desc(f, ring, ev, len, 1, 0);
}

int fake_inject(struct fake_chip *f, unsigned ring, const uint8_t *pkt, uint32_t len)
{
    int ok = ring < 8 ? emit_desc(f, ring, pkt, len, 1, 0) : 0;
    if (ring < 8) {
        if (ok) f->rx_injected[ring]++;
        else f->rx_inject_dropped[ring]++;
    }
    return ok;
}

int fake_inject_frag(struct fake_chip *f, unsigned ring, const uint8_t *pkt, uint32_t len, uint32_t chunk)
{
    uint32_t off = 0;
    int ok = 1;
    if (ring >= 8 || !chunk) return 0;
    while (off < len && ok) {
        uint32_t n = len - off < chunk ? len - off : chunk;
        ok = emit_desc(f, ring, pkt + off, n, off + n == len, 0);
        off += n;
    }
    return ok;
}

int fake_inject_badlen(struct fake_chip *f, unsigned ring, uint32_t claim_len)
{
    if (ring >= 8) return 0;
    return emit_desc(f, ring, NULL, 0, 1, claim_len ? claim_len : 0x3fffu + 1u);
}

int fake_find_cmd(const struct fake_chip *f, uint32_t key, unsigned from, unsigned nth)
{
    unsigned i;
    for (i = from; i < f->n_cmds && i < FAKE_MAX_CMDS; i++) {
        const struct fake_cmd *c = &f->cmds[i];
        if (c->ring == 17 && FAKE_KEY(c->fmt, c->cid, c->ext) == key) {
            if (nth == 0) return (int)i;
            nth--;
        }
    }
    return -1;
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
        f->cmds[f->n_cmds].fmt = FAKE_FMT_CMD;
        f->cmds[f->n_cmds].ext = 0;
        f->cmds[f->n_cmds].raw = NULL;
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

static void postboot_command(struct fake_chip *f, unsigned fmt, uint8_t id, uint8_t ext, uint8_t seq,
                             const uint8_t *pl, uint32_t plen);

static void mcu_command(struct fake_chip *f, const uint8_t *p, uint32_t len)
{
    uint32_t i, plen, hdr;
    uint8_t cid, seq, expect, ext = 0;
    unsigned fmt;
    const uint8_t *pl;

    f->cmd_count++;
    if (len < 48) {
        violation(f, "MCU command of %u bytes is shorter than any header", len);
        return;
    }
    /* struct mt76_connac2_mcu_uni_txd has the cid where the 64-byte header has pq_id 0x8000 and
     * option 0x07 at 0x2b */
    fmt = ((p[0x22] | (p[0x23] << 8)) != 0x8000 && p[0x2b] == 0x07) ? FAKE_FMT_UNI : FAKE_FMT_CMD;
    hdr = fmt == FAKE_FMT_UNI ? 48u : 64u;
    if (len < hdr) {
        violation(f, "MCU command of %u bytes is shorter than its %u-byte header", len, hdr);
        return;
    }
    pl = p + hdr;
    plen = len - hdr;
    if ((le32(p) & 0xffffu) != len) violation(f, "TXD0 TX_BYTES %u != descriptor length %u", le32(p) & 0xffffu, len);
    if (((le32(p) >> 23) & 3u) != 2u) violation(f, "TXD0 PKT_FMT %u (want 2 = CMD)", (le32(p) >> 23) & 3u);
    if ((le32(p) >> 25) != 0x20u) violation(f, "TXD0 Q_IDX 0x%x (want 0x20)", le32(p) >> 25);
    if (le32(p) & 0x007f0000u) violation(f, "TXD0 reserved bits set: 0x%08x", le32(p));
    if (le32(p + 4) != 0x80010000u) violation(f, "TXD1 0x%08x (want 0x80010000)", le32(p + 4));
    for (i = 8; i < 32; i++)
        if (p[i]) violation(f, "TXD byte 0x%x non-zero", i);
    if ((uint32_t)(p[0x20] | (p[0x21] << 8)) != len - 32u) violation(f, "len field %u (want %u)", p[0x20] | (p[0x21] << 8), len - 32u);
    seq = p[0x27];
    if (fmt == FAKE_FMT_UNI) {
        if (p[0x23] != 0) violation(f, "UNI cid 0x%04x above 0xff", p[0x22] | (p[0x23] << 8));
        cid = p[0x22];
        if (p[0x24] || p[0x26] || p[0x28] || p[0x29] || p[0x2a]) violation(f, "UNI cmd 0x%02x: rsv/frag/checksum/s2d not 0", cid);
        if (p[0x25] != 0xa0) violation(f, "pkt_type 0x%02x (want 0xa0)", p[0x25]);
        for (i = 0x2c; i < 0x30; i++)
            if (p[i]) violation(f, "UNI TXD rsv1 byte 0x%x non-zero", i);
    } else {
        cid = p[0x24];
        ext = p[0x29];
        if (p[0x25] != 0xa0) violation(f, "pkt_type 0x%02x (want 0xa0)", p[0x25]);
        if (p[0x26] == 3) {
            fmt = FAKE_FMT_CMD;
            if (p[0x28] || p[0x29] || p[0x2a] || p[0x2b]) violation(f, "cmd 0x%02x: rev/ext_cid/s2d/ext_cid_ack not 0", cid);
        } else if (cid == 0xed) {
            fmt = FAKE_FMT_EXT;
            if (!ext || p[0x2b] != 1 || p[0x26] > 1 || p[0x28] || p[0x2a])
                violation(f, "EXT cmd 0x%02x: ext_cid_ack %u set_query %u", ext, p[0x2b], p[0x26]);
        } else {
            fmt = FAKE_FMT_CE;
            if (ext || p[0x2b] || p[0x26] > 1 || p[0x28] || p[0x2a])
                violation(f, "CE cmd 0x%02x: ext %u ack %u set_query %u", cid, ext, p[0x2b], p[0x26]);
        }
        for (i = 0x2c; i < 0x40; i++)
            if (p[i]) violation(f, "TXD rsv byte 0x%x non-zero", i);
    }
    expect = next_seq(f);
    if (seq != expect) {
        /* After an injected fault (e.g. a stalled FWDL ring) the chip never saw some chunks whose
         * sequence numbers the host consumed: resynchronise instead of reporting. */
        if (f->fault_idx) f->seq_counter = seq;
        else violation(f, "cmd 0x%02x seq %u, expected %u", cid, seq, expect);
    }
    log_cmd(f, cid, seq, len, 17);
    if (f->n_cmds <= FAKE_MAX_CMDS) {
        struct fake_cmd *c = &f->cmds[f->n_cmds - 1u];
        c->fmt = (uint8_t)fmt;
        c->ext = ext;
        c->set_query = fmt == FAKE_FMT_UNI ? 0 : p[0x26];
        c->raw = (uint8_t *)malloc(len);
        if (c->raw) memcpy(c->raw, p, len);
        c->pl_first = plen ? pl[0] : 0;
    }
    if (fmt != FAKE_FMT_CMD) {
        postboot_command(f, fmt, cid, ext, seq, pl, plen);
        return;
    }

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
        if (f->fw_int_ena) {
            mock_set_reg(f->m, R_INT_ENA, mock_get_reg(f->m, R_INT_ENA) | f->fw_int_ena);
            mock_set_reg(f->m, R_INT_STA, mock_get_reg(f->m, R_INT_STA) | f->fw_int_ena);
            f->next_int_rearm_us = f->m->clock_us + f->fw_int_rearm_us;
        }
        respond(f, f->fw_start_ring, 0x01, seq, 0);
        break;
    default:
        violation(f, "unknown MCU command 0x%02x", cid);
        break;
    }
}

/* ---- post-boot / sniffer commands (spec 11) ---- */

static const uint8_t default_nic_reply[] = {
    6, 0, 0, 0,                                                   /* n_element 6, rsv */
    5, 0, 0, 0, 8, 0, 0, 0, 0x10, 0, 0x61, 0x79, 0, 0, 0, 0,      /* HW_VER: 8 raw bytes */
    6, 0, 0, 0, 8, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8,               /* SW_VER: 8 raw bytes */
    7, 0, 0, 0, 6, 0, 0, 0, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,   /* MAC (locally administered, fake) */
    8, 0, 0, 0, 12, 0, 0, 0, 1, 1, 1, 2, 2, 0, 1, 1, 1, 1, 3, 1,  /* PHY: nss 2, hw_path 3, he 1 */
    0x20, 0, 0, 0, 8, 0, 0, 0, 0x09, 0, 0, 0, 0, 0, 0, 0,         /* CHIP_CAP: CLC_EVT_EN | WF_RF_PIN_CTRL_EVT_EN */
    0x18, 0, 0, 0, 1, 0, 0, 0, 0,                                 /* 6G: none */
};

static void pb_reply(struct fake_chip *f, uint32_t key, uint8_t seq, const uint8_t *payload, uint32_t plen,
                     uint32_t delay_us)
{
    uint8_t ev[1024];
    uint32_t total = 36u + (plen < 4u ? 4u : plen);
    if ((f->drop_response_nth && f->cmd_count == f->drop_response_nth) || (f->drop_key && f->drop_key == key) ||
        (f->drop_all_from_cmd && f->cmd_count >= (unsigned)f->drop_all_from_cmd)) {
        f->responses_dropped_by_knob++;
        mark_fault(f);
        return;
    }
    if (total > sizeof(ev)) {
        violation(f, "fake: reply too large");
        return;
    }
    if (delay_us) mock_advance(f->m, delay_us);
    if (f->inject_unsolicited) emit_response(f, f->event_ring, 0x27, 0, 0, 40);
    if (f->inject_stale) emit_response(f, f->event_ring, 0x01, (uint8_t)((seq + 13u) % 15u + 1u), 0x55, 40);
    memset(ev, 0, total);
    put32(ev, total | (7u << 27));
    ev[0x18] = (uint8_t)total;
    ev[0x19] = (uint8_t)(total >> 8);
    ev[0x1c] = 0x01;
    ev[0x1d] = seq;
    if (plen) memcpy(ev + 36, payload, plen);
    emit(f, f->event_ring, ev, total);
}

static void uni_status_reply(struct fake_chip *f, uint32_t key, uint8_t cid, uint8_t seq, uint32_t status)
{
    uint8_t pl[8];
    memset(pl, 0, sizeof(pl));
    pl[0] = cid;
    put32(pl + 4, status);
    pb_reply(f, key, seq, pl, sizeof(pl), 0);
}

static int all_zero(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

static void retune(struct fake_chip *f, int by_switch, uint8_t ch, uint8_t band)
{
    if (f->tune_by == 0 || (f->tune_by == 1 && by_switch) || (f->tune_by == 2 && !by_switch)) {
        f->channel = ch;
        f->band = band;
        f->tunes++;
    }
}

static void postboot_command(struct fake_chip *f, unsigned fmt, uint8_t id, uint8_t ext, uint8_t seq,
                             const uint8_t *pl, uint32_t plen)
{
    uint32_t key = FAKE_KEY(fmt, id, ext);
    const struct fake_cmd *c = &f->cmds[(f->n_cmds - 1u) % FAKE_MAX_CMDS];

    if (!f->fw_started) violation(f, "post-boot command fmt %u id 0x%02x/0x%02x before FW_START", fmt, id, ext);
    if (fmt == FAKE_FMT_CE && id == 0xc0) {
        if (c->set_query != 0) violation(f, "REG_READ with set_query %u (want 0 = QUERY)", c->set_query);
    } else if (fmt != FAKE_FMT_UNI && c->set_query != 1) {
        violation(f, "fmt %u id 0x%02x/0x%02x: set_query %u (want 1 = SET)", fmt, id, ext, c->set_query);
    }

    if (fmt == FAKE_FMT_CE) {
        switch (id) {
        case 0x8a: /* GET_NIC_CAPAB */
            if (plen) violation(f, "GET_NIC_CAPAB with %u payload bytes", plen);
            f->nic_queried = 1;
            pb_reply(f, key, seq, f->nic_reply ? f->nic_reply : default_nic_reply,
                     f->nic_reply ? f->nic_reply_len : (uint32_t)sizeof(default_nic_reply), 0);
            return;
        case 0xca: { /* CHIP_CONFIG */
            char text[64];
            size_t n;
            if (plen != 328 || !all_zero(pl, 8) || pl[327] != 0) {
                violation(f, "CHIP_CONFIG payload %u bytes / header not zero", plen);
                return;
            }
            memcpy(text, pl + 8, sizeof(text) - 1);
            text[sizeof(text) - 1] = '\0';
            n = strlen(text);
            if (!all_zero(pl + 8 + n, 320u - (uint32_t)n)) violation(f, "CHIP_CONFIG data not NUL-padded");
            if (strcmp(text, "DowngradeTxStreamTemp 105 115") == 0) f->thermal_set = 1;
            else if (strcmp(text, "KeepFullPwr 1") == 0) f->keep_full_pwr = 1;
            else if (strcmp(text, "KeepFullPwr 0") == 0) f->keep_full_pwr = 0;
            else violation(f, "CHIP_CONFIG text not expected: %s", text);
            return;
        }
        case 0xc5: /* FWLOG_2_HOST */
            if (plen != 4 || pl[0] != 1 || !all_zero(pl + 1, 3)) violation(f, "FWLOG_2_HOST payload");
            f->fwlog_on = 1;
            return;
        case 0x0f: { /* SET_CHAN_DOMAIN */
            uint32_t n, i;
            if (plen < 12) {
                violation(f, "SET_CHAN_DOMAIN payload %u", plen);
                return;
            }
            n = (uint32_t)pl[8] + pl[9] + pl[10];
            if (plen != 12u + 8u * n || pl[0] != '0' || pl[1] != '0' || pl[2] || pl[3] || pl[4] != 0 || pl[5] != 3 ||
                pl[6] != 3 || pl[7] || pl[11])
                violation(f, "SET_CHAN_DOMAIN header/length wrong (%u bytes, %u channels)", plen, n);
            for (i = 0; i < n && 12u + 8u * i + 8u <= plen; i++)
                if (!all_zero(pl + 12 + 8 * i + 1, 7) || pl[12 + 8 * i] == 0) violation(f, "SET_CHAN_DOMAIN channel %u entry", i);
            if (!f->mac_enabled) violation(f, "SET_CHAN_DOMAIN before MAC enable");
            f->chan_domain_n2 = pl[8];
            f->chan_domain_n5 = pl[9];
            return;
        }
        case 0x0a: /* SET_RX_FILTER */
            if (plen != 68 || !all_zero(pl, 4) || !all_zero(pl + 5, 3) || !all_zero(pl + 17, 51)) {
                violation(f, "SET_RX_FILTER payload");
                return;
            }
            if (pl[4] == 1) {
                if (!(le32(pl + 8) & 0x80000000u) || le32(pl + 12) || pl[16]) violation(f, "SET_RX_FILTER mode 1 fields");
                f->rx_filter_fif = le32(pl + 8);
            } else if (pl[4] == 2) {
                if (le32(pl + 8) || pl[16] != 2 || le32(pl + 12) != 0x800u) violation(f, "SET_RX_FILTER mode 2 fields");
                else f->other_beacon_drop_cleared = 1;
            } else {
                violation(f, "SET_RX_FILTER mode %u", pl[4]);
            }
            return;
        case 0x5d: { /* SET_RATE_TX_POWER (mt76_connac_mcu_rate_txpower_band) */
            static const uint8_t l2[14] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
            static const uint8_t l5[48] = { 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 100, 102, 104,
                                            106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 132, 134, 136,
                                            138, 140, 142, 144, 149, 151, 153, 155, 157, 159, 161, 165, 169, 173, 177 };
            uint32_t n, j, k, nlist;
            int b;
            const uint8_t *list;
            if (plen < 44) {
                violation(f, "SET_RATE_TX_POWER payload %u", plen);
                return;
            }
            n = pl[4];
            b = pl[5] == 1 ? 0 : pl[5] == 2 ? 1 : -1;
            if (b < 0 || n == 0 || n > 8 || plen != 44u + n * 162u || !all_zero(pl, 4) || pl[6] > 1 || pl[7] ||
                pl[8] != '0' || pl[9] != '0' || pl[10] || pl[11] || !all_zero(pl + 12, 32)) {
                violation(f, "SET_RATE_TX_POWER header (%u bytes, n %u, band %u)", plen, n, pl[5]);
                return;
            }
            if (f->pwr_pending_read) violation(f, "SET_RATE_TX_POWER without a REG_READ after the previous one");
            list = b ? l5 : l2;
            nlist = b ? 48u : 14u;
            for (j = 0; j < n; j++) {
                const uint8_t *e = pl + 44 + j * 162u;
                uint32_t pos = f->pwr_channels[b] + j;
                int cap = b == 0 ? 40 : (e[0] >= 36 && e[0] <= 48) ? 46 : 28;
                int8_t v = (int8_t)e[1 + 4];
                if (pos >= nlist || e[0] != list[pos]) violation(f, "SET_RATE_TX_POWER channel %u at position %u", e[0], pos);
                if (v <= 0 || v > cap) violation(f, "SET_RATE_TX_POWER channel %u: limit %d above the PL value %d", e[0], v, cap);
                for (k = 0; k < 161u; k++) {
                    int is_val = k < 4u ? b == 0 : k < 29u ? 1 : k < 77u ? ((k - 29u) % 12u) < 10u : 1;
                    if ((int8_t)e[1 + k] != (is_val ? v : 127)) {
                        violation(f, "SET_RATE_TX_POWER channel %u entry %u = %d", e[0], k, (int8_t)e[1 + k]);
                        break;
                    }
                }
                f->pwr_value[b][e[0]] = v;
                if (j + 1 == n && pl[6] != (list[pos] == 177 ? 1 : 0)) violation(f, "SET_RATE_TX_POWER last_msg %u", pl[6]);
            }
            f->pwr_channels[b] += n;
            f->pwr_msgs[b]++;
            f->pwr_pending_read = 1;
            return;
        }
        case 0xc0: { /* MCU_CE_QUERY(REG_READ) {le32 addr; le32 val} */
            uint8_t rr[8];
            if (plen != 8 || le32(pl) != 0x820c8000u || le32(pl + 4)) violation(f, "REG_READ payload");
            if (!f->pwr_pending_read) violation(f, "REG_READ without a preceding SET_RATE_TX_POWER");
            f->pwr_pending_read = 0;
            f->reg_reads++;
            put32(rr, 0x820c8000u);
            put32(rr + 4, 0x00000001u);
            pb_reply(f, key, seq, rr, sizeof(rr), 0);
            return;
        }
        case 0x17: /* SET_BSS_ABORT */
            if (plen != 4 || !all_zero(pl, 4)) violation(f, "SET_BSS_ABORT payload");
            f->bss_aborts++;
            return;
        default:
            break;
        }
    } else if (fmt == FAKE_FMT_EXT) {
        switch (ext) {
        case 0x21: /* EFUSE_BUFFER_MODE */
            if (plen != 4 || pl[0] != 0 || pl[1] != 1 || pl[2] || pl[3]) violation(f, "EFUSE_BUFFER_MODE payload");
            if (!f->nic_queried) violation(f, "EFUSE_BUFFER_MODE before GET_NIC_CAPAB");
            f->efuse_mode_set = 1;
            pb_reply(f, key, seq, NULL, 0, 0);
            return;
        case 0x3e: /* PROTECT_CTRL */
            if (plen != 12 || pl[0] != 1 || pl[1] || pl[2] || pl[3] || le32(pl + 4) != 0x92bu || le32(pl + 8) != 2u)
                violation(f, "PROTECT_CTRL payload");
            if (!f->efuse_mode_set || f->wtbl_updates < 20) violation(f, "PROTECT_CTRL before EFUSE mode / MAC init");
            f->rts_set = 1;
            pb_reply(f, key, seq, NULL, 0, 0);
            return;
        case 0x46: /* MAC_INIT_CTRL */
            if (plen != 4 || pl[0] > 1 || pl[1] || pl[2] || pl[3]) violation(f, "MAC_INIT_CTRL payload");
            if (pl[0] && (!f->efuse_mode_set || !f->rts_set)) violation(f, "MAC enable before EFUSE mode / RTS");
            if (!pl[0] && (f->sniffer_on || f->bss_active || f->dev_active || f->bss1_active || f->dev1_active))
                violation(f, "MAC disable with the sniffer/BSS/DEV still active");
            f->mac_enabled = pl[0];
            pb_reply(f, key, seq, NULL, 0, 0);
            return;
        case 0x4e:   /* SET_RX_PATH */
        case 0x08: { /* CHANNEL_SWITCH */
            uint8_t want_rx = ext == 0x4e ? 3u : 2u;
            if (plen != 76 || pl[1] != pl[0] || pl[2] != 0 || pl[3] != 2 || pl[4] != want_rx || pl[5] != 0 || pl[6] ||
                pl[7] || pl[8] || pl[9] || pl[10] > 1 || !all_zero(pl + 11, 65))
                violation(f, "%s payload", ext == 0x4e ? "SET_RX_PATH" : "CHANNEL_SWITCH");
            if (!f->mac_enabled) violation(f, "channel command before MAC enable");
            retune(f, 1, pl[0], pl[10]);
            pb_reply(f, key, seq, NULL, 0, f->tune_delay_us);
            return;
        }
        case 0x05: /* ID_RADIO_ON_OFF_CTRL */
            if (plen != 4 || pl[0] < 1 || pl[0] > 3 || pl[1] || pl[2] || pl[3]) violation(f, "ID_RADIO_ON_OFF_CTRL payload");
            f->radio_led = pl[0];
            return;
        case 0xbd: /* WF_RF_PIN_CTRL */
            if (plen != 2 || pl[0] || pl[1]) violation(f, "WF_RF_PIN_CTRL payload");
            f->rf_pin_inits++;
            return;
        default:
            break;
        }
    } else if (fmt == FAKE_FMT_UNI) {
        switch (id) {
        case 0x01: { /* DEV_INFO_UPDATE; omac_idx 0 or 1 (pair) */
            uint8_t idx = pl[0];
            if (plen != 16 || idx > 1 || pl[1] || pl[2] || pl[3] || pl[4] || pl[5] || pl[6] != 12 || pl[7] || pl[8] > 1 ||
                pl[9]) {
                violation(f, "DEV_INFO_UPDATE payload");
                return;
            }
            if (pl[8]) {
                if (!f->mac_enabled) violation(f, "DEV_INFO active before MAC enable");
                /* 00:00:00:00:00:00 is what Linux gives the virtual monitor vif (OMAC 0 of the pair) */
                if ((pl[10] & 1u) || (idx == 1 && all_zero(pl + 10, 6))) violation(f, "DEV_INFO with an invalid MAC");
                if (idx == 1 && !f->dev_active) violation(f, "DEV_INFO 1 before DEV_INFO 0");
                memcpy(idx ? f->dev1_mac : f->dev_mac, pl + 10, 6);
            } else if (idx ? f->bss1_active : f->bss_active) {
                violation(f, "DEV_INFO %u inactive while its BSS is active", idx);
            } else if (idx == 0 && f->dev1_active) {
                violation(f, "DEV_INFO 0 inactive while vif 1 is active");
            }
            if (f->uni_wrong_cid && !f->uni_wrong_cid_sent) {
                f->uni_wrong_cid_sent = 1;
                uni_status_reply(f, 0, 0x02, seq, 0); /* same seq, wrong cid: must be skipped */
            }
            if (f->uni_status_dev == 0) {
                if (idx) f->dev1_active = pl[8];
                else f->dev_active = pl[8];
            }
            uni_status_reply(f, key, 0x01, seq, f->uni_status_dev);
            return;
        }
        case 0x02: { /* BSS_INFO_UPDATE; bss_idx = omac_idx = hw_bss_idx = wmm_idx = vif index */
            uint8_t idx = pl[0];
            if (plen != 36 || idx > 1 || !all_zero(pl + 1, 3) || pl[4] || pl[5] || pl[6] != 32 || pl[7] || pl[8] > 1 ||
                pl[9] != idx || pl[10] != idx || pl[11] || le32(pl + 12) != 0x00010002u || pl[16] != 1 || pl[17] != idx ||
                !all_zero(pl + 18, 18)) {
                violation(f, "BSS_INFO_UPDATE payload");
                return;
            }
            if (pl[8] && !(idx ? f->dev1_active : f->dev_active)) violation(f, "BSS_INFO active before DEV_INFO");
            if (f->uni_status_bss == 0) {
                if (idx) f->bss1_active = pl[8];
                else f->bss_active = pl[8];
            }
            uni_status_reply(f, key, 0x02, seq, f->uni_status_bss);
            return;
        }
        case 0x24: /* SNIFFER */
            if (plen == 12 && all_zero(pl, 4) && pl[4] == 0 && pl[5] == 0 && pl[6] == 8 && pl[7] == 0 && pl[8] <= 1 &&
                all_zero(pl + 9, 3)) {
                if (!f->mac_enabled) violation(f, "SNIFFER enable before MAC enable");
                f->sniffer_on = pl[8];
                pb_reply(f, key, seq, NULL, 0, 0);
                return;
            }
            if (plen == 20 && all_zero(pl, 4) && pl[4] == 1 && pl[5] == 0 && pl[6] == 16 && pl[7] == 0 && pl[8] == 0 &&
                pl[9] == 0 && (pl[10] == 1 || pl[10] == 2) && pl[11] == 0 && pl[13] == 0 && pl[14] == pl[12] && pl[15] == 0 &&
                pl[16] <= 1 && all_zero(pl + 17, 3)) {
                if (!f->sniffer_on) violation(f, "SNIFFER config while the sniffer is off");
                f->drop_err = pl[16];
                retune(f, 0, pl[12], (uint8_t)(pl[10] - 1u));
                pb_reply(f, key, seq, NULL, 0, f->tune_delay_us);
                return;
            }
            violation(f, "SNIFFER payload (%u bytes)", plen);
            return;
        default:
            break;
        }
    }
    violation(f, "unknown MCU command fmt %u id 0x%02x ext 0x%02x (%u bytes payload)", fmt, id, ext, plen);
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

/* ---- raw TX (TX ring 0), spec 12 ---- */

static int tok_test(const struct fake_chip *f, uint16_t t) { return (f->tok_inflight[t >> 3] >> (t & 7u)) & 1u; }
static void tok_set(struct fake_chip *f, uint16_t t, int on)
{
    if (on) f->tok_inflight[t >> 3] = (uint8_t)(f->tok_inflight[t >> 3] | (1u << (t & 7u)));
    else f->tok_inflight[t >> 3] = (uint8_t)(f->tok_inflight[t >> 3] & ~(1u << (t & 7u)));
}

static void emit_txfree(struct fake_chip *f, uint16_t token, uint8_t wcid, uint32_t status, uint32_t count)
{
    uint8_t p[16];
    /* {le16 rx_byte_cnt; le16 ctrl (MSDU_CNT 9:0, PKT_TYPE in the dword's 31:27); le32 txd} + a wcid pair
     * entry (BIT(31), does not count) + one MSDU entry */
    put32(p, 16u | (1u << 16) | (6u << 27));
    put32(p + 4, 0);
    put32(p + 8, 0x80000000u | ((uint32_t)wcid << 14));
    put32(p + 12, ((uint32_t)token << 16) | ((status & 3u) << 13) | (count & 0x1fffu));
    if (tok_test(f, token) && f->tx_inflight) f->tx_inflight--;
    tok_set(f, token, 0);
    if (emit_desc(f, f->txc_ring, p, sizeof(p), 1, 0)) f->txfree_sent++;
}

static void emit_txs(struct fake_chip *f, uint8_t pid, uint8_t wcid, int ack, uint16_t rate, int fix, uint16_t sn)
{
    uint32_t w[8];
    uint8_t buf[160];
    unsigned i;
    memset(w, 0, sizeof(w));
    w[0] = (rate & 0x3fffu) | (ack ? 0u : 0x10000u) | (fix ? 0x80000000u : 0u); /* format 0 (MPDU) */
    w[1] = (uint32_t)sn << 20;
    w[2] = (uint32_t)wcid << 16;
    w[3] = (uint32_t)pid << 24;
    memset(buf, 0, sizeof(buf));
    if (f->txs_via_event) {
        uint32_t total = 36u + 28u + 28u + 32u;
        put32(buf, total | (7u << 27));
        buf[0x18] = (uint8_t)total;
        buf[0x1c] = 0x0f;                     /* MCU_EVENT_TX_DONE, seq 0 */
        buf[36] = pid;
        for (i = 0; i < 7; i++) put32(buf + 36 + 28 + 4 * i, w[i]);
        if (emit_desc(f, f->txc_ring, buf, total, 1, 0)) f->txs_sent++;
    } else {
        put32(buf, 40u | (0u << 27));         /* PKT_TYPE_TXS, 2 header words + one 8-word record */
        for (i = 0; i < 8; i++) put32(buf + 8 + 4 * i, w[i]);
        if (emit_desc(f, f->txc_ring, buf, 40, 1, 0)) f->txs_sent++;
    }
}

static void tx_complete(struct fake_chip *f, uint16_t token, uint8_t pid, uint8_t wcid, int ack, uint8_t rem,
                        uint16_t rate, uint16_t sn)
{
    int txs = pid >= 3 && !f->txs_never;
    if (txs && f->txs_first) emit_txs(f, pid, wcid, ack, rate, 1, sn);
    emit_txfree(f, token, wcid, 0, ack ? 1u : rem);
    if (txs && !f->txs_first) emit_txs(f, pid, wcid, ack, rate, 1, sn);
}

unsigned fake_tx_release(struct fake_chip *f)
{
    unsigned i, n = f->n_deferred;
    for (i = 0; i < n; i++)
        tx_complete(f, f->deferred[i].token, f->deferred[i].pid, f->deferred[i].wcid, f->deferred[i].ack,
                    f->deferred[i].rem, f->deferred[i].rate, f->deferred[i].sn);
    f->n_deferred = 0;
    return n;
}

int fake_tx_free_token(struct fake_chip *f, uint16_t token, uint8_t wcid, uint32_t status, uint32_t count)
{
    unsigned before = f->txfree_sent;
    emit_txfree(f, token, wcid, status, count);
    return f->txfree_sent != before;
}

static int tx_chan_ok(uint8_t ch, uint8_t band)
{
    if (band == 0) return ch >= 1 && ch <= 13;
    return ch == 36 || ch == 40 || ch == 44 || ch == 48;
}

/* One TX ring 0 descriptor's TXWI (TXD + hardware TXP), validated against mt76's encoding. */
static void tx0_frame(struct fake_chip *f, const uint8_t *w, uint32_t len)
{
    uint32_t t[8], i, flen, hdrlen, buf0, rate = 0;
    const uint8_t *txp = w + 32, *fr;
    uint16_t fc, token, len0, sn;
    uint8_t type, sub, group, omac, wcid, pid, fix, no_ack, q, rem;
    const uint8_t *mac;
    struct fake_air *a;

    if (len != 64) {
        violation(f, "TX ring 0 segment of %u bytes (want the 64-byte TXWI)", len);
        return;
    }
    if (!f->fw_started || !f->mac_enabled || !f->sniffer_on) violation(f, "frame on TX ring 0 before firmware/MAC/sniffer");
    for (i = 0; i < 8; i++) t[i] = le32(w + 4 * i);
    if ((t[0] & 0xffffu) < 32u + 24u) {
        violation(f, "TXD0 TX_BYTES %u", t[0] & 0xffffu);
        return;
    }
    flen = (t[0] & 0xffffu) - 32u;
    q = (uint8_t)(t[0] >> 25);
    if ((t[0] >> 23) & 3u) violation(f, "TXD0 PKT_FMT %u (want 0 = CT)", (t[0] >> 23) & 3u);
    if (q != 0x10u && q > 7u) violation(f, "TXD0 Q_IDX 0x%x", q);
    if (t[0] & 0x007f0000u) violation(f, "TXD0 ETH_TYPE_OFFSET set");
    if (!(t[1] & 0x80000000u) || (t[1] & 0x408c0400u) || ((t[1] >> 16) & 3u) != 2u)
        violation(f, "TXD1 0x%08x: LONG_FORMAT / HDR_FORMAT 802.11 / reserved bits", t[1]);
    omac = (uint8_t)((t[1] >> 24) & 0x3fu);
    wcid = (uint8_t)(t[1] & 0x3ffu);
    /* hardware TXP */
    token = (uint16_t)(txp[0] | (txp[1] << 8));
    if (!(token & 0x8000u)) violation(f, "TXP msdu_id[0] 0x%04x without MT_MSDU_ID_VALID", token);
    token &= 0x7fffu;
    len0 = (uint16_t)(txp[12] | (txp[13] << 8));
    buf0 = le32(txp + 8);
    if (!all_zero(txp + 2, 6) || !(len0 & 0x8000u) || (len0 & 0x7000u) || (uint32_t)(len0 & 0xfffu) != flen ||
        !all_zero(txp + 14, 18))
        violation(f, "TXP malformed: len0 0x%04x for %u bytes", len0, flen);
    fr = dma_ptr(f, buf0, flen, 1, "TX frame (TXP buf0)");
    if (!fr) return;
    fc = (uint16_t)(fr[0] | (fr[1] << 8));
    type = (uint8_t)((fc >> 2) & 3u);
    sub = (uint8_t)((fc >> 4) & 0xfu);
    group = (uint8_t)(fr[4] & 1u);
    sn = (uint16_t)((fr[22] | (fr[23] << 8)) >> 4);
    hdrlen = type == 2 ? 24u + ((fc & 0x0300u) == 0x0300u ? 6u : 0u) + ((sub & 8u) ? 2u : 0u) : 24u;
    if (((t[1] >> 11) & 0x1fu) * 2u != hdrlen) violation(f, "TXD1 HDR_INFO %u for a %u-byte header", (t[1] >> 11) & 0x1fu, hdrlen);
    if ((t[2] & 0x3fu) != (uint32_t)((type << 4) | sub) || ((t[7] >> 20) & 3u) != type || ((t[7] >> 16) & 0xfu) != sub)
        violation(f, "TXD2/TXD7 type/subtype do not match frame control 0x%04x", fc);
    if (((t[2] >> 10) & 1u) != group) violation(f, "TXD2 MULTICAST %u for address 1 group bit %u", (t[2] >> 10) & 1u, group);
    if (t[2] & ~(0x80000000u | 0x2000u | 0x400u | 0x3fu)) violation(f, "TXD2 unexpected bits 0x%08x", t[2]);
    fix = (uint8_t)(t[2] >> 31);
    if (fix) {
        uint32_t mode, idx;
        rate = (t[6] >> 16) & 0x3fffu;
        mode = rate >> 6;
        idx = rate & 0x3fu;
        if (!(t[2] & 0x2000u) || !(t[3] & 0x10000000u) || (t[6] & ~(0x3fff0000u | 0x4u)) != 0 || !(t[6] & 0x4u))
            violation(f, "fixed-rate frame: HTC_VLD / BA_DISABLE / FIXED_BW / TXD6 0x%08x", t[6]);
        if (!((mode == 0 && idx <= 3 && f->band == 0) || (mode == 1 && idx >= 8 && idx <= 15)))
            violation(f, "TXD6 TX_RATE 0x%x not a legacy rate for band %u", rate, f->band);
    } else {
        if (t[6] || (t[2] & 0x2000u) || (t[3] & 0x10000000u) || (t[7] & 0xf800u))
            violation(f, "rate-controlled frame with fixed-rate fields set");
        if (type != 2 || group) violation(f, "management or group frame without FIX_RATE");
    }
    no_ack = (uint8_t)(t[3] & 1u);
    rem = (uint8_t)((t[3] >> 11) & 0x1fu);
    if (group && !no_ack) violation(f, "group-addressed frame without NO_ACK");
    if (rem < 1 || rem > 15) violation(f, "TXD3 REM_TX_COUNT %u", rem);
    if (!(t[3] & 0x80000000u) || ((t[3] >> 16) & 0xfffu) != sn) violation(f, "TXD3 SN_VALID/SEQ 0x%08x for SN %u", t[3], sn);
    if (t[3] & ~(0x80000000u | 0x10000000u | 0x0fff0000u | 0xf800u | 1u)) violation(f, "TXD3 unexpected bits 0x%08x", t[3]);
    if (t[4]) violation(f, "TXD4 0x%08x", t[4]);
    pid = (uint8_t)(t[5] & 0xffu);
    if (((t[5] >> 10) & 1u) != (pid >= 3 ? 1u : 0u) || (t[5] & ~0x4ffu) || (no_ack && pid))
        violation(f, "TXD5 0x%08x (PID / TX_STATUS_HOST)", t[5]);
    if (t[7] & ~(0x00300000u | 0x000f0000u | 0xf800u)) violation(f, "TXD7 unexpected bits 0x%08x", t[7]);
    if (omac > 1 || !(omac ? f->dev1_active : f->dev_active)) violation(f, "TXD1 OWN_MAC %u has no active DEV_INFO", omac);
    mac = omac ? f->dev1_mac : f->dev_mac;
    if (!f->allow_foreign_sa && memcmp(fr + 10, mac, 6) != 0) violation(f, "address 2 is not the OMAC %u address", omac);
    if (wcid != 0 && wcid != 19u - omac) violation(f, "TXD1 WLAN_IDX %u", wcid);
    if (!tx_chan_ok(f->channel, f->band)) violation(f, "frame transmitted on channel %u band %u (not allowed)", f->channel, f->band);
    if (tok_test(f, token)) violation(f, "token %u reused while in flight", token);
    tok_set(f, token, 1);
    f->tx_inflight++;
    if (f->n_air < FAKE_MAX_AIR) {
        a = &f->air[f->n_air++];
        memset(a, 0, sizeof(*a));
        a->bytes = (uint8_t *)malloc(flen);
        if (a->bytes) memcpy(a->bytes, fr, flen);
        a->len = flen;
        memcpy(a->txd, t, sizeof(t));
        memcpy(a->txp, txp, 32);
        a->token = token;
        a->fix_rate = fix;
        a->rate = (uint16_t)rate;
        a->no_ack = no_ack;
        a->q_idx = q;
        a->wcid = wcid;
        a->omac = omac;
        a->pid = pid;
        a->rem_tx = rem;
        a->channel = f->channel;
        a->band = f->band;
        a->time_us = now(f);
    }
    switch (f->tx_mode) {
    case FAKE_TX_SILENT:
        break;
    case FAKE_TX_DEFER:
        if (f->n_deferred < FAKE_MAX_DEFER) {
            f->deferred[f->n_deferred].token = token;
            f->deferred[f->n_deferred].pid = pid;
            f->deferred[f->n_deferred].wcid = wcid;
            f->deferred[f->n_deferred].ack = 1;
            f->deferred[f->n_deferred].rem = rem;
            f->deferred[f->n_deferred].rate = (uint16_t)rate;
            f->deferred[f->n_deferred].sn = sn;
            f->n_deferred++;
        } else {
            violation(f, "fake: too many deferred TX completions");
        }
        break;
    default:
        tx_complete(f, token, pid, wcid, no_ack || f->tx_mode == FAKE_TX_ACK, rem, (uint16_t)rate, sn);
        break;
    }
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
    if ((int)n == f->tx_stall_ring || (n == 0 && f->tx_no_consume)) {
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
            else tx0_frame(f, data, len);
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
    f->keep_full_pwr = -1;
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
    if (f->asleep) {
        if (f->wake_at_us && f->m->clock_us >= f->wake_at_us) {
            f->asleep = 0;                     /* the CLR_OWN that woke it takes effect */
            f->own_sync = 0;
            f->driver_owned_once = 1;
        } else {
            f->reads_while_asleep++;
            *value = 0xffffffffu;
            return;
        }
    }
    if (f->fw_started && f->fw_int_ena && f->fw_int_rearm_us && f->m->clock_us >= f->next_int_rearm_us) {
        mock_set_reg(f->m, R_INT_ENA, mock_get_reg(f->m, R_INT_ENA) | f->fw_int_ena);
        f->next_int_rearm_us = f->m->clock_us + f->fw_int_rearm_us;
        if (off == R_INT_ENA) *value = mock_get_reg(f->m, R_INT_ENA);
    }
    if (off == R_LPCTL) {
        *value = f->own_sync ? 4u : 0u;
        if (f->sleep_pending && f->own_sync) { /* OWN_SYNC is seen once, then the chip goes to sleep */
            f->sleep_pending = 0;
            f->asleep = 1;
        }
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
    } else if (off == R_WTBL_UPDATE) {
        *value = f->wtbl_busy_never ? (*value | 0x80000000u) : (*value & ~0x80000000u);
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
        if (f->dead_read_seen) {
            if (off == R_LPCTL && v == 2u) f->clr_own_after_dead_read++;
            else f->writes_after_dead_read++;
        }
        return;
    }
    if (f->asleep) {
        if (off == R_LPCTL && (v & 2u)) {
            f->clr_own_requests++;
            if (!f->wake_at_us && f->wake_us) f->wake_at_us = f->m->clock_us + f->wake_us;
        } else {
            f->writes_while_asleep++;
        }
        return;
    }
    if (off == R_LPCTL) {
        if (v & 1u) {
            f->set_own_writes++;
            if (f->driver_owned_once) violation(f, "SET_OWN written after the driver took ownership");
            if (!f->fw_own_never) f->own_sync = 1;
            if (f->sleep_after_set_own && !f->driver_owned_once) f->sleep_pending = 1;
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
    } else if (is_mac_reg(off)) {
        if (!f->fw_started) violation(f, "MAC register 0x%05x written before the firmware runs", off);
        if (off == R_WTBL_UPDATE) {
            if ((v & 0x3ffu) >= 20u || !(v & 0x1000u)) violation(f, "WTBL_UPDATE 0x%08x", v);
            f->wtbl_updates++;
        }
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
    f->txc_ring = 4;
    f->fw_int_ena = 0x20000000u;

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
    for (i = 0; i < f->n_cmds && i < FAKE_MAX_CMDS; i++) {
        free(f->cmds[i].raw);
        f->cmds[i].raw = NULL;
    }
    for (i = 0; i < f->n_dl && i < FAKE_MAX_DL; i++) free(f->dl[i].data);
    for (i = 0; i < f->n_air && i < FAKE_MAX_AIR; i++) {
        free(f->air[i].bytes);
        f->air[i].bytes = NULL;
    }
    f->n_air = 0;
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
