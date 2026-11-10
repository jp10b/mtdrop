/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radiod: user-mode host of the mt792x chip library on Windows.
 *
 *   radiod dma-probe                           allocate the DMA block, verify it is writable, free it
 *                                              (no device access)
 *   radiod bringup <stage 1-6> <patch> <ram>   staged bring-up (docs/MT7921-BRINGUP.md):
 *                                              1 ownership, 2 reset, 3 rings, 4 MCU, 5 firmware, 6 post-boot
 *   radiod sniff <channel> <seconds> <patch> <ram> [options]
 *                                              full bring-up, post-boot, sniffer on one channel, receive for
 *                                              <seconds> (max 120), print a summary, shut down (spec 11)
 *
 * Privacy: MAC addresses are printed as OUI:xx:xx:xx only; SSIDs only with --show-ssid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_win.h"
#include "mt792x/mt792x.h"

static int dma_probe(void) {
    char error[256];
    struct halwin *h = halwin_create(error, sizeof(error));
    if (!h) {
        printf("RESULT: FAIL - %s\n", error);
        return 1;
    }
    struct mt_hal *hal = halwin_hal(h);
    printf("DMA block: bus address 0x%08x, length 0x%x, low blocks seen %u\n", hal->dma_bus, hal->dma_len,
           halwin_low_blocks_seen(h));
    int ok = halwin_dma_phys(h) + hal->dma_len <= 0x100000000ull && (hal->dma_bus & 0x1FFFFFu) == 0;
    for (uint32_t i = 0; i < hal->dma_len; i += 4096) hal->dma_virt[i] = (uint8_t)(i >> 12);
    hal->dma_virt[hal->dma_len - 1] = 0xA5;
    for (uint32_t i = 0; i < hal->dma_len; i += 4096) ok = ok && hal->dma_virt[i] == (uint8_t)(i >> 12);
    ok = ok && hal->dma_virt[hal->dma_len - 1] == 0xA5;
    uint64_t t0 = hal->now_us(hal->ctx);
    hal->sleep_us(hal->ctx, 500);
    hal->sleep_us(hal->ctx, 5000);
    uint64_t waited = hal->now_us(hal->ctx) - t0;
    printf("clock: 5.5 ms of sleeps measured as %llu us\n", (unsigned long long)waited);
    ok = ok && waited >= 5500 && waited < 60000;
    halwin_destroy(h);
    printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL - block not usable\n");
    return ok ? 0 : 1;
}

/* Reads a whole file into a buffer of exactly its size. */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n > 0 && n <= 64L * 1024L * 1024L && fseek(f, 0, SEEK_SET) == 0) {
        b = malloc((size_t)n);
        if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

static struct mt792x g_dev; /* ~16 KiB: keep it off the stack */

/* Everything a run needs before the first chip access: validated firmware, DMA block, proxy with the
 * stop list armed, library initialised. */
struct session {
    struct mt792x_fw fw;
    uint8_t *patch, *ram;
    struct mt792x_patch_info *pinfo;
    struct mt792x_ram_info *rinfo;
    struct halwin *h;
};

static int session_open(struct session *s, const char *patch_path, const char *ram_path) {
    char error[256];
    int err;

    memset(s, 0, sizeof(*s));
    /* 1. Read and validate the firmware files before anything else. */
    s->patch = read_file(patch_path, &s->fw.patch_len);
    s->ram = read_file(ram_path, &s->fw.ram_len);
    s->fw.patch = s->patch;
    s->fw.ram = s->ram;
    if (!s->patch || !s->ram) {
        printf("RESULT: FAIL - cannot read %s\n", !s->patch ? patch_path : ram_path);
        return -1;
    }
    s->pinfo = calloc(1, sizeof(*s->pinfo));
    s->rinfo = calloc(1, sizeof(*s->rinfo));
    if (!s->pinfo || !s->rinfo) return -1;
    err = mt792x_patch_parse(s->patch, s->fw.patch_len, s->pinfo);
    if (err) {
        printf("RESULT: FAIL - patch file rejected: %s (%d)\n", mt792x_strerror(err), err);
        return -1;
    }
    err = mt792x_ram_parse(s->ram, s->fw.ram_len, s->rinfo);
    if (err) {
        printf("RESULT: FAIL - RAM file rejected: %s (%d)\n", mt792x_strerror(err), err);
        return -1;
    }
    printf("firmware: patch %s (%u section(s)), RAM %s %s (%u region(s)), feature byte 0x%02x (status %d, CNM %s)\n",
           s->pinfo->build_date, s->pinfo->n_sections, s->rinfo->fw_ver, s->rinfo->build_date, s->rinfo->n_regions,
           s->rinfo->fw_features, s->rinfo->fw_features_status,
           (s->rinfo->fw_features & MT792X_FW_CAP_CNM) ? "yes" : "no");

    /* 2. DMA block. */
    s->h = halwin_create(error, sizeof(error));
    if (!s->h) {
        printf("RESULT: FAIL - %s\n", error);
        return -1;
    }

    /* 3. Proxy. */
    if (halwin_open_proxy(s->h, error, sizeof(error)) != 0) {
        printf("RESULT: FAIL - %s\n", error);
        return -1;
    }

    /* 4. The stop list must be armed before the chip gets any chance to do DMA. */
    {
        struct mt792x_stop_op ops[MT792X_STOP_OPS_COUNT];
        struct halwin_close_op close_ops[MT792X_STOP_OPS_COUNT + 1];
        size_t n = mt792x_stop_ops(ops, MT792X_STOP_OPS_COUNT), m = 0;
        for (size_t i = 0; i < n; i++) {
            /* mt76 holds the WFSYS reset for 50 ms; the proxy can wait, so put the delay before the
             * last operation (the release of the reset asserted by the one before it). */
            if (i + 1 == n) {
                close_ops[m].offset = 0;
                close_ops[m].value = 50;
                close_ops[m].kind = 4;
                m++;
            }
            close_ops[m].offset = ops[i].bar_offset;
            close_ops[m].value = ops[i].value;
            close_ops[m].kind = ops[i].kind; /* same numbering: 1 write, 2 set, 3 clear */
            m++;
        }
        if (halwin_set_close_ops(s->h, close_ops, (uint32_t)m) != 0) {
            printf("RESULT: FAIL - the proxy did not accept the stop list; not starting\n");
            return -1;
        }
        printf("stop list armed in the proxy (%u operations)\n", (unsigned)m);
    }

    /* 5. Library. */
    err = mt792x_init(&g_dev, halwin_hal(s->h));
    if (err) {
        printf("RESULT: FAIL - init: %s (%d)\n", mt792x_strerror(err), err);
        return -1;
    }
    return 0;
}

/* Always: orderly shutdown (a no-op if nothing was touched), then close the proxy (which replays the
 * stop list) and only then free the DMA block. */
static void session_close(struct session *s) {
    mt792x_shutdown(&g_dev);
    if (g_dev.shutdown_error) printf("shutdown reported: %s\n", mt792x_strerror(g_dev.shutdown_error));
    halwin_destroy(s->h);
    free(s->pinfo);
    free(s->rinfo);
    free(s->patch);
    free(s->ram);
    memset(s, 0, sizeof(*s));
}

static int bringup(int stage, const char *patch_path, const char *ram_path) {
    struct session s;
    int err, rc = 1;

    if (stage < MT792X_STAGE_OWNERSHIP || stage > MT792X_STAGE_POSTBOOT) {
        printf("RESULT: FAIL - stage must be 1..6\n");
        return 2;
    }
    if (session_open(&s, patch_path, ram_path) == 0) {
        err = mt792x_bringup(&g_dev, &s.fw, (enum mt792x_stage)stage);
        printf("chip id 0x%04x, rev 0x%08x, last step S%d, failed step S%d, proxy I/O errors %u\n",
               mt792x_chip_id(&g_dev), mt792x_hw_rev(&g_dev), mt792x_last_step(&g_dev), mt792x_failed_step(&g_dev),
               halwin_io_errors(s.h));
        if (err) {
            printf("RESULT: FAIL - stage %d: %s (%d) at S%d\n", stage, mt792x_strerror(err), err,
                   mt792x_failed_step(&g_dev));
        } else {
            printf("RESULT: PASS - stage %d reached\n", stage);
            rc = 0;
        }
    }
    session_close(&s);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ */
/* sniff                                                                                            */

#define MAX_HOP 32
#define MAX_TA 1024
#define MAX_SSID 128
#define MAX_LAT 8192

struct sniff_opts {
    uint8_t channel;
    uint32_t seconds;
    int show_ssid;
    int debug;
    uint8_t hop[MAX_HOP];
    unsigned n_hop;
    uint32_t dwell_ms;
    int tune;          /* enum mt792x_tune_mode */
    int fcs;           /* enum mt792x_fcs_mode */
    int no_hdr_trans;
    int no_fwlog;
    int drop_fcs_err;  /* do not request FCS-error frames */
};

struct sniff_stats {
    uint64_t frames;
    uint32_t by_type[4][16];          /* [type][subtype] */
    uint32_t by_rx_channel[256];      /* per-frame channel field */
    uint32_t by_tuned_channel[256];   /* channel we were tuned to when the frame was taken */
    uint32_t rssi_hist[11];           /* -100..-91, ..., -10..-1, plus [10] = no RSSI */
    uint32_t hdr_trans;
    uint32_t fcs_err;
    uint8_t ta[MAX_TA][6];
    unsigned n_ta;
    int ta_overflow;
    uint32_t awdl_bssid, awdl_action;
    uint8_t ssid[MAX_SSID][33];
    uint8_t ssid_len[MAX_SSID];
    unsigned n_ssid;
    uint32_t beacons_ok_on_channel;
    uint32_t beacons_other_channel;
    unsigned dumped;
    uint32_t lat[MAX_LAT];
    unsigned n_lat;
};

static uint8_t band_of(uint8_t ch) {
    return ch > 14 ? MT792X_BAND_5GHZ : MT792X_BAND_2GHZ;
}

static void print_masked_addr(const uint8_t *a) {
    printf("%02x:%02x:%02x:xx:xx:xx", a[0], a[1], a[2]);
}

static void dump_frame(const struct mt792x_rx_frame *fr, unsigned n) {
    printf("frame %u: ring %u, %u bytes (raw %u, header gap %u), channel %u band %u, fcs_ok %u, rssi %d dBm, flags 0x%04x\n",
           n, fr->ring, fr->len, fr->raw_len, fr->hdr_gap, fr->channel, fr->band, fr->fcs_ok, fr->rssi_dbm, fr->flags);
    printf("  RXD:");
    for (int i = 0; i < 6; i++) printf(" %08x", fr->rxd[i]);
    printf("\n  bytes:");
    for (uint32_t i = 0; i < fr->len && i < 32; i++) {
        /* addresses 1-3 occupy bytes 4-9, 10-15, 16-21: keep the OUI, mask the rest */
        int masked = !(fr->flags & MT792X_RXF_HDR_TRANS) &&
                     ((i >= 7 && i <= 9) || (i >= 13 && i <= 15) || (i >= 19 && i <= 21));
        if (masked) printf(" xx");
        else printf(" %02x", fr->data[i]);
    }
    printf("\n");
}

static void note_ta(struct sniff_stats *st, const uint8_t *a) {
    for (unsigned i = 0; i < st->n_ta; i++)
        if (memcmp(st->ta[i], a, 6) == 0) return;
    if (st->n_ta < MAX_TA) memcpy(st->ta[st->n_ta++], a, 6);
    else st->ta_overflow = 1;
}

static void note_ssid(struct sniff_stats *st, const uint8_t *s, uint8_t len) {
    for (unsigned i = 0; i < st->n_ssid; i++)
        if (st->ssid_len[i] == len && memcmp(st->ssid[i], s, len) == 0) return;
    if (st->n_ssid < MAX_SSID) {
        memcpy(st->ssid[st->n_ssid], s, len);
        st->ssid_len[st->n_ssid++] = len;
    }
}

static void account(struct sniff_stats *st, const struct mt792x_rx_frame *fr, uint8_t tuned) {
    static const uint8_t awdl_bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
    const uint8_t *d = fr->data;
    uint32_t len = fr->len;

    st->frames++;
    st->by_rx_channel[fr->channel]++;
    st->by_tuned_channel[tuned]++;
    if (!fr->fcs_ok) st->fcs_err++;
    if (fr->flags & MT792X_RXF_RATE) {
        int b = (fr->rssi_dbm + 100) / 10;
        if (fr->rssi_dbm < -100) b = 0;
        if (b > 9) b = 9;
        st->rssi_hist[b]++;
    } else {
        st->rssi_hist[10]++;
    }
    if (fr->flags & MT792X_RXF_HDR_TRANS) {
        st->hdr_trans++;
        return;
    }
    if (len < 10) return;
    unsigned type = (d[0] >> 2) & 3u, sub = (d[0] >> 4) & 0xfu;
    st->by_type[type][sub]++;
    /* address 2 (transmitter) exists except in CTS (12) and ACK (13) control frames */
    if (len >= 16 && !(type == 1 && (sub == 12 || sub == 13))) note_ta(st, d + 10);
    if (len >= 22 && type != 1 && memcmp(d + 16, awdl_bssid, 6) == 0) st->awdl_bssid++;
    if (type == 0 && sub == 13 && len >= 28 && d[24] == 127 && d[25] == 0x00 && d[26] == 0x17 && d[27] == 0xf2)
        st->awdl_action++;
    if (type == 0 && sub == 8) { /* beacon: 24-byte header, 12 fixed bytes, then elements */
        if (fr->fcs_ok && fr->channel == tuned) st->beacons_ok_on_channel++;
        else if (fr->fcs_ok) st->beacons_other_channel++;
        uint32_t p = 36;
        while (fr->fcs_ok && p + 2 <= len) {
            uint8_t id = d[p], el = d[p + 1];
            if (p + 2u + el > len) break;
            if (id == 0) {
                if (el <= 32) note_ssid(st, d + p + 2, el);
                break;
            }
            p += 2u + el;
        }
    }
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void print_summary(const struct sniff_opts *o, struct sniff_stats *st, double secs) {
    static const char *const mgmt[16] = {"assoc-req", "assoc-resp", "reassoc-req", "reassoc-resp", "probe-req",
                                         "probe-resp", "timing-adv", "mgmt-7", "beacon", "atim", "disassoc",
                                         "auth", "deauth", "action", "action-noack", "mgmt-15"};
    const struct mt792x_rx_counters *c = mt792x_rx_counters(&g_dev);

    printf("\n==== sniff summary: %.1f s, %llu frames (%.1f frames/s) ====\n", secs, (unsigned long long)st->frames,
           secs > 0 ? (double)st->frames / secs : 0.0);
    printf("by type/subtype:\n");
    for (int s = 0; s < 16; s++)
        if (st->by_type[0][s]) printf("  mgmt %-13s %u\n", mgmt[s], st->by_type[0][s]);
    for (int s = 0; s < 16; s++)
        if (st->by_type[1][s]) printf("  ctrl subtype %-6d %u%s\n", s, st->by_type[1][s], s == 13 ? " (ACK)" : s == 12 ? " (CTS)" : s == 11 ? " (RTS)" : "");
    uint32_t data = 0;
    for (int s = 0; s < 16; s++) data += st->by_type[2][s];
    if (data) printf("  data (all subtypes)  %u\n", data);
    for (int s = 0; s < 16; s++)
        if (st->by_type[3][s]) printf("  extension sub %-5d %u\n", s, st->by_type[3][s]);
    if (st->hdr_trans) printf("  802.3 (header translated) %u\n", st->hdr_trans);
    printf("by per-frame channel field:\n");
    for (int ch = 0; ch < 256; ch++)
        if (st->by_rx_channel[ch]) printf("  channel %3d: %u\n", ch, st->by_rx_channel[ch]);
    if (o->n_hop) {
        printf("by tuned channel:\n");
        for (int ch = 0; ch < 256; ch++)
            if (st->by_tuned_channel[ch]) printf("  tuned %3d: %u\n", ch, st->by_tuned_channel[ch]);
    }
    printf("RSSI histogram (dBm):\n");
    for (int b = 0; b < 10; b++)
        if (st->rssi_hist[b]) printf("  %4d..%4d: %u\n", -100 + 10 * b, -91 + 10 * b, st->rssi_hist[b]);
    if (st->rssi_hist[10]) printf("  no RSSI (no group 3): %u\n", st->rssi_hist[10]);
    printf("distinct transmitter addresses: %u%s\n", st->n_ta, st->ta_overflow ? " (table full)" : "");
    for (unsigned i = 0; i < st->n_ta && i < 20; i++) {
        printf("  ");
        print_masked_addr(st->ta[i]);
        printf("\n");
    }
    printf("AWDL: %u frames with address 3 = AWDL BSSID, %u AWDL action frames\n", st->awdl_bssid, st->awdl_action);
    printf("distinct SSIDs in beacons: %u\n", st->n_ssid);
    for (unsigned i = 0; i < st->n_ssid; i++) {
        if (!o->show_ssid) {
            printf("  SSID #%u: length %u%s\n", i + 1, st->ssid_len[i], st->ssid_len[i] ? "" : " (hidden)");
            continue;
        }
        printf("  \"");
        for (unsigned k = 0; k < st->ssid_len[i]; k++) {
            uint8_t ch = st->ssid[i][k];
            if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\') putchar(ch);
            else printf("\\x%02x", ch);
        }
        printf("\"\n");
    }
    printf("beacons with good FCS on the tuned channel: %u (on another channel: %u)\n", st->beacons_ok_on_channel,
           st->beacons_other_channel);
    if (st->n_lat) {
        qsort(st->lat, st->n_lat, sizeof(st->lat[0]), cmp_u32);
        printf("channel switch latency (%u switches): min %u us, median %u us, max %u us\n", st->n_lat, st->lat[0],
               st->lat[st->n_lat / 2], st->lat[st->n_lat - 1]);
    }
    if (c) {
        printf("library counters: frames %llu, FCS errors %u, ICV errors %u, FCS trailer present %u, header-translated %u\n",
               (unsigned long long)c->frames, c->fcs_err, c->icv_err, c->fcs_trailer, c->hdr_trans);
        printf("  malformed %u (", c->malformed);
        for (int i = 1; i < MT792X_RXE_COUNT; i++) printf("%s%d:%u", i > 1 ? " " : "", i, c->malformed_by[i]);
        printf("), fragmented packets %u (%u descriptors), bad descriptor lengths %u\n", c->frag_packets, c->frag_descs,
               c->bad_desc_len);
        printf("  events %u (unsolicited %u), TX status records %u, TX-free MSDUs %u, other packets %u\n", c->events,
               c->unsolicited, c->txs_records, c->tx_free_msdus, c->other_pkts);
        printf("  frames consumed during commands %u, ring 2 seen full %u times, liveness checks %u\n",
               c->discarded_in_cmd, c->rx2_full, c->liveness_checks);
        printf("  packet types:");
        for (int t = 0; t < 32; t++)
            if (c->pkt_type[t]) printf(" %d:%u", t, c->pkt_type[t]);
        printf("\n  events by id:");
        for (int e = 0; e < 256; e++)
            if (c->event_by_id[e]) printf(" 0x%02x:%u", e, c->event_by_id[e]);
        printf("\n");
    }
}

static int parse_hop(struct sniff_opts *o, const char *list) {
    const char *p = list;
    while (*p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 1 || v > 177 || o->n_hop >= MAX_HOP) return -1;
        o->hop[o->n_hop++] = (uint8_t)v;
        p = end;
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return o->n_hop ? 0 : -1;
}

static int sniff(int argc, char **argv) {
    static struct sniff_stats st; /* large: keep it off the stack */
    struct sniff_opts o;
    struct session s;
    int rc = 1, err;

    memset(&o, 0, sizeof(o));
    o.channel = (uint8_t)atoi(argv[2]);
    o.seconds = (uint32_t)atoi(argv[3]);
    o.dwell_ms = 200;
    if (o.seconds > 120) o.seconds = 120;
    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "--show-ssid") == 0) o.show_ssid = 1;
        else if (strcmp(argv[i], "--debug") == 0) o.debug = 1;
        else if (strcmp(argv[i], "--no-hdr-trans") == 0) o.no_hdr_trans = 1;
        else if (strcmp(argv[i], "--no-fwlog") == 0) o.no_fwlog = 1;
        else if (strcmp(argv[i], "--drop-fcs-errors") == 0) o.drop_fcs_err = 1;
        else if (strcmp(argv[i], "--hop") == 0 && i + 1 < argc) {
            if (parse_hop(&o, argv[++i])) {
                printf("RESULT: FAIL - bad --hop list\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--dwell-ms") == 0 && i + 1 < argc) {
            o.dwell_ms = (uint32_t)atoi(argv[++i]);
            if (o.dwell_ms < 10) o.dwell_ms = 10;
        } else if (strcmp(argv[i], "--tune") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            o.tune = !strcmp(t, "switch") ? MT792X_TUNE_CHANNEL_SWITCH
                     : !strcmp(t, "sniffer") ? MT792X_TUNE_SNIFFER_CONFIG
                     : !strcmp(t, "both")    ? MT792X_TUNE_BOTH
                                             : MT792X_TUNE_AUTO;
        } else if (strcmp(argv[i], "--fcs") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            o.fcs = !strcmp(t, "strip") ? MT792X_FCS_STRIP : !strcmp(t, "auto") ? MT792X_FCS_AUTO : MT792X_FCS_MT76;
        } else {
            printf("RESULT: FAIL - unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (o.channel < 1 || o.channel > 177 || o.seconds == 0) {
        printf("RESULT: FAIL - channel 1..177 and seconds 1..120 required\n");
        return 2;
    }
    memset(&st, 0, sizeof(st));

    if (session_open(&s, argv[4], argv[5]) != 0) goto out;
    halwin_set_log_level(s.h, o.debug ? MT_LOG_DEBUG : MT_LOG_INFO);
    g_dev.opt.tune_mode = (uint8_t)o.tune;
    g_dev.opt.fcs_mode = (uint8_t)o.fcs;
    g_dev.opt.no_rx_hdr_trans = (uint8_t)o.no_hdr_trans;
    g_dev.opt.skip_fw_log = (uint8_t)o.no_fwlog;

    err = mt792x_bringup(&g_dev, &s.fw, MT792X_STAGE_POSTBOOT);
    if (err) {
        printf("RESULT: FAIL - bring-up: %s (%d) at S%d\n", mt792x_strerror(err), err, mt792x_failed_step(&g_dev));
        goto out;
    }
    {
        struct mt792x_sniffer_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.channel = o.channel;
        cfg.band = band_of(o.channel);
        cfg.flags = MT792X_SNIFF_CONTROL | MT792X_SNIFF_OTHER_BSS | (o.drop_fcs_err ? 0u : MT792X_SNIFF_FCSFAIL);
        err = mt792x_sniffer_start(&g_dev, &cfg);
        if (err) {
            printf("RESULT: FAIL - sniffer start: %s (%d) at SN%d\n", mt792x_strerror(err), err, g_dev.sniff_failed_step);
            goto out;
        }
    }
    printf("sniffing on channel %u for %u s (tuning by mode %d)%s\n", o.channel, o.seconds, mt792x_tune_mode_used(&g_dev),
           o.n_hop ? ", hopping" : "");

    {
        struct mt_hal *hal = halwin_hal(s.h);
        uint64_t t0 = hal->now_us(hal->ctx), now = t0, next_hop = t0 + (uint64_t)o.dwell_ms * 1000u;
        uint8_t tuned = o.channel;
        unsigned hop_i = 0;
        int poll_err = 0;
        while ((now = hal->now_us(hal->ctx)) - t0 < (uint64_t)o.seconds * 1000000u) {
            struct mt792x_rx_frame fr;
            if (o.n_hop && now >= next_hop) {
                uint32_t el = 0;
                uint8_t ch = o.hop[hop_i++ % o.n_hop];
                err = mt792x_sniffer_set_channel(&g_dev, ch, band_of(ch), &el);
                if (err) {
                    printf("channel switch to %u failed: %s\n", ch, mt792x_strerror(err));
                    poll_err = err;
                    break;
                }
                tuned = ch;
                if (st.n_lat < MAX_LAT) st.lat[st.n_lat++] = el;
                next_hop = hal->now_us(hal->ctx) + (uint64_t)o.dwell_ms * 1000u;
            }
            int r = mt792x_rx_poll(&g_dev, &fr);
            if (r < 0) {
                printf("rx_poll: %s (%d)\n", mt792x_strerror(r), r);
                poll_err = r;
                break;
            }
            if (r == 0) {
                hal->sleep_us(hal->ctx, 500);
                continue;
            }
            if (st.dumped < 3) dump_frame(&fr, st.dumped++);
            account(&st, &fr, tuned);
        }
        print_summary(&o, &st, (double)(hal->now_us(hal->ctx) - t0) / 1e6);
        if (poll_err) {
            printf("RESULT: FAIL - receive loop stopped: %s (%d); FCS errors %u, malformed %u\n", mt792x_strerror(poll_err),
                   poll_err, st.fcs_err, mt792x_rx_counters(&g_dev)->malformed);
        } else if (st.beacons_ok_on_channel) {
            printf("RESULT: PASS - %u beacon(s) with good FCS whose per-frame channel equals the tuned channel\n",
                   st.beacons_ok_on_channel);
            rc = 0;
        } else {
            printf("RESULT: FAIL - no beacon with good FCS on the tuned channel (frames %llu, FCS errors %u, malformed %u, events %u)\n",
                   (unsigned long long)st.frames, st.fcs_err, mt792x_rx_counters(&g_dev)->malformed,
                   mt792x_rx_counters(&g_dev)->events);
        }
    }

out:
    session_close(&s);
    return rc;
}

int main(int argc, char **argv) {
    printf("radiod (mt792x library %s)\n", mt792x_version());
    if (argc >= 2 && strcmp(argv[1], "dma-probe") == 0) return dma_probe();
    if (argc == 5 && strcmp(argv[1], "bringup") == 0) return bringup(atoi(argv[2]), argv[3], argv[4]);
    if (argc >= 6 && strcmp(argv[1], "sniff") == 0) return sniff(argc, argv);
    printf("usage: radiod dma-probe\n"
           "       radiod bringup <stage 1-6> <patch file> <ram file>\n"
           "         stages: 1 ownership, 2 reset, 3 rings, 4 MCU, 5 firmware, 6 post-boot\n"
           "       radiod sniff <channel> <seconds, max 120> <patch file> <ram file> [options]\n"
           "         --show-ssid            print SSIDs (otherwise only their count and lengths)\n"
           "         --hop <c1,c2,...>      hop over these channels, --dwell-ms N per channel (default 200)\n"
           "         --tune auto|switch|sniffer|both   how to tune (default auto: firmware CNM bit)\n"
           "         --fcs mt76|strip|auto  FCS treatment (default mt76: no FCS expected)\n"
           "         --no-hdr-trans         leave RX header translation off\n"
           "         --no-fwlog             do not ask the firmware for log events\n"
           "         --drop-fcs-errors      do not request FCS-error frames\n"
           "         --debug                print DEBUG library messages\n");
    return 2;
}
