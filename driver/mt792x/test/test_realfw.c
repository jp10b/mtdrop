/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Optional local-only test: if MT792X_FW_DIR names a directory with the real MT7961 firmware
 * files, parse them and print their metadata. Skipped silently otherwise. The files are read
 * only; nothing is copied anywhere.
 */
#include "test.h"
#include "mt792x/mt792x.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    long n;
    uint8_t *b;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    b = (uint8_t *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

TEST(real_firmware_metadata)
{
    const char *dir = getenv("MT792X_FW_DIR");
    struct mt792x_patch_info *p;
    struct mt792x_ram_info *r;
    uint8_t *pb, *rb;
    size_t pl = 0, rl = 0;
    uint32_t i;
    int err;

    if (!dir || !*dir) return;
    pb = read_file(dir, "WIFI_MT7961_patch_mcu_1_2_hdr.bin", &pl);
    rb = read_file(dir, "WIFI_RAM_CODE_MT7961_1.bin", &rl);
    if (!pb || !rb) {
        printf("  MT792X_FW_DIR set but files not readable; skipped\n");
        free(pb);
        free(rb);
        return;
    }
    p = (struct mt792x_patch_info *)calloc(1, sizeof(*p));
    r = (struct mt792x_ram_info *)calloc(1, sizeof(*r));

    err = mt792x_patch_parse(pb, pl, p);
    printf("  patch: %lu bytes, parse %d (%s)\n", (unsigned long)pl, err, mt792x_strerror(err));
    CHECK(err == 0);
    printf("    build_date '%s' platform '%s' hw_sw_ver 0x%08lx patch_ver 0x%08lx checksum 0x%04x\n", p->build_date,
           p->platform, (unsigned long)p->hw_sw_ver, (unsigned long)p->patch_ver, (unsigned)p->checksum);
    printf("    desc: patch_ver 0x%08lx subsys 0x%08lx feature 0x%08lx crc 0x%08lx sections %lu\n",
           (unsigned long)p->desc_patch_ver, (unsigned long)p->subsys, (unsigned long)p->feature,
           (unsigned long)p->crc, (unsigned long)p->n_sections);
    printf("    #  type       offs     size     addr       len      sec_key    align  mode       cmd\n");
    for (i = 0; i < p->n_sections; i++) {
        const struct mt792x_patch_section *s = &p->sec[i];
        printf("    %u  0x%08lx 0x%06lx 0x%06lx 0x%08lx 0x%06lx 0x%08lx 0x%04lx 0x%08lx 0x%02x\n", (unsigned)i,
               (unsigned long)s->type, (unsigned long)s->offs, (unsigned long)s->size, (unsigned long)s->addr,
               (unsigned long)s->len, (unsigned long)s->sec_key_idx, (unsigned long)s->align_len,
               (unsigned long)s->mode, s->cmd_id);
    }

    err = mt792x_ram_parse(rb, rl, r);
    printf("  RAM: %lu bytes, parse %d (%s)\n", (unsigned long)rl, err, mt792x_strerror(err));
    CHECK(err == 0);
    printf("    fw_ver '%s' build_date '%s' chip_id 0x%02x eco %u format_ver %u format_flag %u crc 0x%08lx\n",
           r->fw_ver, r->build_date, r->chip_id, r->eco_code, r->format_ver, r->format_flag, (unsigned long)r->crc);
    printf("    regions %lu, data %lu bytes, download %lu bytes, FW_START option %lu addr 0x%08lx\n",
           (unsigned long)r->n_regions, (unsigned long)r->data_len, (unsigned long)r->total_dl_bytes,
           (unsigned long)r->fw_start_option, (unsigned long)r->override_addr);
    printf("    #  file_off  addr       len      feature type dl mode       cmd decomp_len decomp_blk\n");
    for (i = 0; i < r->n_regions; i++) {
        const struct mt792x_ram_region *g = &r->reg[i];
        printf("    %u  0x%06lx  0x%08lx 0x%06lx 0x%02x    %u    %u  0x%08lx 0x%02x 0x%06lx   0x%06lx\n", (unsigned)i,
               (unsigned long)g->file_offset, (unsigned long)g->addr, (unsigned long)g->len, g->feature_set, g->type,
               g->download, (unsigned long)g->mode, g->cmd_id, (unsigned long)g->decomp_len,
               (unsigned long)g->decomp_blk_sz);
    }
    free(p);
    free(r);
    free(pb);
    free(rb);
}
