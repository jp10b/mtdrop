/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rig_init_ex(struct rig *r, uint32_t dma_bus, int paged)
{
    memset(r, 0, sizeof(*r));
    if (mock_init(&r->m, RIG_DMA_LEN) != 0) return -1;
    if (dma_bus) r->m.hal.dma_bus = dma_bus;
    if (paged) {
        /* Page i lives at a pseudo-random distinct page frame: frame = 0x1000 + (i * 197) mod 512, spread
         * further by a stride of 3 pages, so neighbours in CPU order are never neighbours on the bus. */
        uint32_t i, n = RIG_DMA_LEN / 4096u;
        for (i = 0; i < n; i++) r->page_bus[i] = (0x1000u + ((i * 197u) % n) * 3u) * 4096u;
        r->m.hal.dma_bus = 0;
        r->m.hal.dma_page_bus = r->page_bus;
    }
    r->f = (struct fake_chip *)calloc(1, sizeof(*r->f));
    r->dev = (struct mt792x *)calloc(1, sizeof(*r->dev));
    if (!r->f || !r->dev) return -1;
    fake_init(r->f, &r->m);
    r->patch = fwb_patch(fwb_default_sections, FWB_DEFAULT_SECTIONS, &r->patch_len);
    r->ram = fwb_ram(fwb_default_regions, FWB_DEFAULT_REGIONS, &r->ram_len);
    if (!r->patch || !r->ram) return -1;
    r->fw.patch = r->patch;
    r->fw.patch_len = r->patch_len;
    r->fw.ram = r->ram;
    r->fw.ram_len = r->ram_len;
    r->init_err = mt792x_init(r->dev, mock_hal(&r->m));
    return r->init_err;
}

int rig_init(struct rig *r, uint32_t dma_bus) { return rig_init_ex(r, dma_bus, 0); }
int rig_init_paged(struct rig *r) { return rig_init_ex(r, 0, 1); }

void rig_set_fw(struct rig *r, uint8_t *patch, size_t patch_len, uint8_t *ram, size_t ram_len)
{
    free(r->patch);
    free(r->ram);
    r->patch = patch;
    r->ram = ram;
    r->patch_len = patch_len;
    r->ram_len = ram_len;
    r->fw.patch = patch;
    r->fw.patch_len = patch_len;
    r->fw.ram = ram;
    r->fw.ram_len = ram_len;
}

void rig_free(struct rig *r)
{
    if (r->f) fake_free(r->f);
    free(r->f);
    free(r->dev);
    free(r->patch);
    free(r->ram);
    mock_free(&r->m);
    memset(r, 0, sizeof(*r));
}

unsigned rig_check_no_violations(const struct rig *r)
{
    if (r->f->violations) {
        printf("    fake chip: %u violation(s), first: %s\n", r->f->violations, r->f->first_violation);
        return 1;
    }
    if (r->m.bad_access) {
        printf("    mock: %u out-of-range register access(es), last 0x%x\n", r->m.bad_access, r->m.bad_offset);
        return 1;
    }
    if (r->m.trace_overflow) {
        printf("    mock: trace overflow\n");
        return 1;
    }
    return 0;
}

unsigned rig_check_int_ena_never_nonzero(const struct rig *r)
{
    uint32_t i;
    for (i = 0; i < r->m.trace_len; i++) {
        if (r->m.trace[i].is_write && r->m.trace[i].offset == 0xd4204u && r->m.trace[i].value != 0) {
            printf("    HOST_INT_ENA written 0x%08x at trace %u\n", r->m.trace[i].value, i);
            return 1;
        }
    }
    return 0;
}

unsigned rig_check_set_own_only_in_s1(const struct rig *r)
{
    uint32_t i;
    unsigned set_own = 0;
    int clr_seen = 0;
    for (i = 0; i < r->m.trace_len; i++) {
        const struct mock_access *a = &r->m.trace[i];
        if (!a->is_write || a->offset != 0xe0010u) continue;
        if (a->value & 1u) {
            set_own++;
            if (clr_seen) {
                printf("    SET_OWN at trace %u after a CLR_OWN\n", i);
                return 1;
            }
        }
        if (a->value & 2u) clr_seen = 1;
    }
    if (set_own > 10u) {
        printf("    %u SET_OWN writes\n", set_own);
        return 1;
    }
    return 0;
}

unsigned rig_check_shutdown(const struct rig *r, uint32_t from, unsigned wfsys_resets_before, uint32_t remap)
{
    unsigned problems = 0;
    uint32_t i, glo;
    int first_write = -1, glo_clear = -1, rst_clear = -1, rst_set = -1;

    for (i = from; i < r->m.trace_len; i++) {
        const struct mock_access *a = &r->m.trace[i];
        if (!a->is_write) continue;
        if (first_write < 0) first_write = (int)i;
        if (a->offset == 0xd4208u && glo_clear < 0 && !(a->value & 0x18208005u)) glo_clear = (int)i;
        if (a->offset == 0xd4100u && (a->value & 0x30u) != 0x30u && rst_clear < 0) rst_clear = (int)i;
        if (a->offset == 0xd4100u && (a->value & 0x30u) == 0x30u && rst_clear >= 0 && rst_set < 0) rst_set = (int)i;
        if (a->offset == 0xe0010u && (a->value & 1u)) {
            printf("    SET_OWN during shutdown\n");
            problems++;
        }
    }
    if (first_write < 0 || r->m.trace[first_write].offset != 0xe0010u || r->m.trace[first_write].value != 2u) {
        printf("    shutdown does not start with the drv-own write\n");
        problems++;
    }
    if (glo_clear < 0 || rst_clear < glo_clear || rst_set < rst_clear) {
        printf("    shutdown order wrong: GLO_CFG clear %d, RST clear %d, RST set %d\n", glo_clear, rst_clear, rst_set);
        problems++;
    }
    glo = fake_glo_cfg(r->f);
    if (glo & 0x18208005u) {
        printf("    GLO_CFG 0x%08x after shutdown\n", glo);
        problems++;
    }
    if ((mock_get_reg(&r->m, 0xd4100u) & 0x30u) != 0x30u) {
        printf("    WFDMA reset not released after shutdown\n");
        problems++;
    }
    if (r->f->wfsys_resets != wfsys_resets_before + 1u || r->f->wfsys_asserted) {
        printf("    WFSYS resets %u (before %u), asserted %d\n", r->f->wfsys_resets, wfsys_resets_before, r->f->wfsys_asserted);
        problems++;
    }
    if (mock_get_reg(&r->m, 0xfe24cu) != remap) {
        printf("    remap 0x%08x, want 0x%08x\n", mock_get_reg(&r->m, 0xfe24cu), remap);
        problems++;
    }
    return problems;
}

void rig_replay_stop_ops(struct rig *r)
{
    struct mt792x_stop_op ops[MT792X_STOP_OPS_COUNT];
    struct mt_hal *h = mock_hal(&r->m);
    size_t i, n = mt792x_stop_ops(ops, MT792X_STOP_OPS_COUNT);
    for (i = 0; i < n; i++) {
        uint32_t v;
        switch (ops[i].kind) {
        case MT792X_STOP_WRITE:
            h->wr(h->ctx, ops[i].bar_offset, ops[i].value);
            break;
        case MT792X_STOP_SET:
            v = h->rr(h->ctx, ops[i].bar_offset);
            h->wr(h->ctx, ops[i].bar_offset, v | ops[i].value);
            break;
        default:
            v = h->rr(h->ctx, ops[i].bar_offset);
            h->wr(h->ctx, ops[i].bar_offset, v & ~ops[i].value);
            break;
        }
    }
}

int rig_postboot(struct rig *r)
{
    return mt792x_bringup(r->dev, &r->fw, MT792X_STAGE_POSTBOOT);
}

int rig_sniff(struct rig *r, uint8_t channel, uint32_t flags)
{
    struct mt792x_sniffer_cfg cfg;
    int err = rig_postboot(r);
    if (err || !channel) return err;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = channel;
    cfg.band = channel > 14 ? MT792X_BAND_5GHZ : MT792X_BAND_2GHZ;
    cfg.flags = flags;
    return mt792x_sniffer_start(r->dev, &cfg);
}

uint32_t rig_shutdown_regs_from(const struct rig *r, uint32_t from)
{
    int i = mock_find_write(&r->m, 0xe0010u, from);
    return i < 0 ? r->m.trace_len : (uint32_t)i;
}
