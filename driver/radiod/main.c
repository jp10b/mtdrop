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
 *   radiod txtest <channel> <patch> <ram> [options]
 *                                              sniffer with TX allowed; at most 20 broadcast probe requests
 *                                              (per arm with --control),
 *                                              one per second; counts the probe responses addressed to us and
 *                                              whether the access points had to retransmit them (spec 12.8);
 *                                              --control adds a control arm (probes from a random address
 *                                              that nobody acknowledges) to judge the ACK behaviour
 *   radiod acktest <channel> <patch> <ram> [options]
 *                                              sniffer with TX allowed; directed probe requests with ACK and
 *                                              TX status requested, alternating between an access point and a
 *                                              ghost address nobody owns, one per second (at most 2 x 20);
 *                                              reports the TX status (ACKed / not) and transmissions per frame
 *
 *   radiod serve <channel> <patch> <ram> [options]
 *                                              radio server on a named pipe for awdld / radioctl (serve.c)
 *
 *   radiod --live-log <file> <command> ...      also append every line (except DEBUG messages) to <file>,
 *                                              written through to disk line by line, so the last line
 *                                              survives a system hang (docs/FINDINGS.md F-011)
 *
 * Privacy: MAC addresses are printed as OUI:xx:xx:xx only; SSIDs only with --show-ssid (and never
 * into the live log).
 */
#define _CRT_RAND_S /* rand_s (txtest --control): must come before <stdlib.h> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_win.h"
#include "mt792x/mt792x.h"
#include "out.h"
#include "radio_link.h"
#include "radiod.h"

/* Every line radiod prints also goes to the live log (out.h). */
#define printf out_printf

static int dma_probe(void) {
    char error[256];
    struct halwin *h = halwin_create(error, sizeof(error));
    if (!h) {
        printf("RESULT: FAIL - %s\n", error);
        return 1;
    }
    struct mt_hal *hal = halwin_hal(h);
    uint32_t pages = hal->dma_len / 4096u, lo = 0xFFFFFFFFu, hi = 0, runs = 1;
    int ok = hal->dma_page_bus != NULL && pages == halwin_low_pages(h);
    for (uint32_t i = 0; ok && i < pages; i++) {
        uint32_t p = hal->dma_page_bus[i];
        ok = p != 0 && (p & 0xFFFu) == 0 && (uint64_t)p + 4096u <= 0x100000000ull;
        if (p < lo) lo = p;
        if (p > hi) hi = p;
        if (i && p != hal->dma_page_bus[i - 1] + 4096u) runs++;
    }
    printf("DMA region: %u pages of 4 KiB below 4 GiB, bus addresses 0x%08x..0x%08x, %u contiguous run(s)\n", pages, lo, hi, runs);
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

struct mt792x g_dev; /* ~16 KiB: keep it off the stack */

/* struct session: radiod.h */
int session_open(struct session *s, const char *patch_path, const char *ram_path) {
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
        struct halwin_close_op close_ops[MT792X_STOP_OPS_COUNT];
        size_t n = mt792x_stop_ops(ops, MT792X_STOP_OPS_COUNT), m = 0;
        for (size_t i = 0; i < n; i++, m++) {
            /* same numbering: 1 write, 2 set, 3 clear, 4 wait (the library includes mt76's 50 ms WFSYS hold) */
            close_ops[m].offset = ops[i].bar_offset;
            close_ops[m].value = ops[i].value;
            close_ops[m].kind = ops[i].kind;
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
static void print_irq_guard(void) {
    const struct mt792x_irq_counters *c = mt792x_irq_counters(&g_dev);
    if (!c || !c->checks) return;
    printf("interrupt guard: %u checks; HOST_INT_ENA found on %u time(s) (last 0x%08x), PCIE_MAC_INT_ENABLE found on "
           "%u time(s) (last 0x%08x); HOST_INT_STA seen 0x%08x, MCU_CMD seen 0x%08x, %u status change(s)\n",
           c->checks, c->host_int_ena_fixed, c->host_int_ena_last, c->pcie_int_fixed, c->pcie_int_last,
           c->host_int_sta_seen, c->mcu_cmd_seen, c->status_changes);
}

void session_close(struct session *s) {
    print_irq_guard();
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
    uint32_t awdl_ok_on_channel;      /* AWDL BSSID frames with good FCS whose per-frame channel = tuned */
    uint8_t ssid[MAX_SSID][33];
    uint8_t ssid_len[MAX_SSID];
    unsigned n_ssid;
    uint32_t beacons_ok_on_channel;
    uint32_t beacons_other_channel;
    unsigned dumped;
    uint32_t lat[MAX_LAT];
    unsigned n_lat;
};

uint8_t band_of(uint8_t ch) {
    return ch > 14 ? MT792X_BAND_5GHZ : MT792X_BAND_2GHZ;
}

void print_masked_addr(const uint8_t *a) {
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
    if (len >= 22 && type != 1 && memcmp(d + 16, awdl_bssid, 6) == 0) {
        st->awdl_bssid++;
        if (fr->fcs_ok && fr->channel == tuned) st->awdl_ok_on_channel++;
    }
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
        } else if (st.beacons_ok_on_channel || st.awdl_ok_on_channel) {
            /* PASS if either kind of frame proves reception on the tuned channel (F-009: on channels 6 and 44
             * there was no access point, only the phone's AWDL frames) */
            printf("RESULT: PASS - %u beacon(s) and %u AWDL frame(s) with good FCS whose per-frame channel equals the tuned channel (%s)\n",
                   st.beacons_ok_on_channel, st.awdl_ok_on_channel,
                   st.beacons_ok_on_channel && st.awdl_ok_on_channel ? "beacons and AWDL"
                   : st.beacons_ok_on_channel                       ? "beacons"
                                                                    : "AWDL frames");
            rc = 0;
        } else {
            printf("RESULT: FAIL - neither a beacon nor an AWDL frame with good FCS on the tuned channel (frames %llu, FCS errors %u, malformed %u, events %u)\n",
                   (unsigned long long)st.frames, st.fcs_err, mt792x_rx_counters(&g_dev)->malformed,
                   mt792x_rx_counters(&g_dev)->events);
        }
    }

out:
    session_close(&s);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ */
/* txtest (spec 12.8)                                                                               */
/*
 * Why probe responses tell us about ACKs: a probe response is a unicast management frame addressed to
 * the station that sent the probe request (address 1 = our MAC). The access point waits for an 802.11
 * ACK from the addressee; without one it retransmits the same frame - same sequence number, Retry bit
 * (frame control bit 11) set - up to its retry limit. So:
 *   - any probe response addressed to our MAC proves that one of our frames was radiated and decoded
 *     by an AP (TX works);
 *   - responses that arrive once, without the Retry bit, mean the AP got its ACK - and since nobody
 *     else owns our MAC, the ACK came from our chip (ACK works);
 *   - responses whose sequence number comes again with the Retry bit mean no ACK arrived.
 * Caveats: an AP may also retransmit because our ACK was lost on the air (a few are normal), and our
 * own receiver may miss copies; hence the thresholds (>= 5 responses, >= 80 %) and INCONCLUSIVE.
 *
 * On a busy channel those absolute thresholds proved too weak (about half of the sequence numbers had
 * a retransmitted copy; collisions cause retries even when the ACK works). --control therefore adds a
 * control arm in the same run: probe requests from a random locally administered address that no
 * station owns, so nobody can ever acknowledge the responses. An access point that receives no ACK
 * retransmits a unicast probe response up to its retry limit, so the control arm shows how often the
 * same access points retransmit without any ACK; if our chip ACKs frames to its own MAC, the responses
 * to our MAC are retransmitted clearly less often than the responses to the control address. Only
 * access points that answered in both arms are compared (same distance, same retry policy).
 */

#define TXT_MAX_PROBES 20                    /* probes per arm */
#define TXT_MAX_FRAMES (2 * TXT_MAX_PROBES)  /* both arms */
#define TXT_MAX_RESP 1024

struct txt_resp {
    uint8_t ta[6];
    uint16_t sn;
    uint8_t retry;
    int8_t rssi;
    uint8_t probe;
    uint8_t arm;    /* 0 = addressed to our MAC, 1 = addressed to the control MAC */
    uint8_t common; /* the transmitter answered in both arms (set by txt_mark_common) */
    uint8_t ssid_len;
    uint8_t ssid[32];
};

struct txt_state {
    struct txt_resp r[TXT_MAX_RESP];
    unsigned n;
    unsigned overflow;
    uint32_t cookie[TXT_MAX_FRAMES];
    int done_seen[TXT_MAX_FRAMES];
    struct mt792x_tx_done done[TXT_MAX_FRAMES];
};

/* Statistics of one arm (optionally only the transmitters present in both arms). */
struct txt_arm {
    unsigned responses;
    unsigned pairs;     /* distinct (transmitter, sequence number) */
    unsigned hist[5];   /* pairs with 1, 2, 3, 4, 5+ copies */
    unsigned retried;   /* responses with the Retry bit */
    double mean;        /* copies per pair = responses / pairs */
};

static const struct { const char *name; uint8_t rate; } txt_rates[] = {
    {"1", MT792X_RATE_CCK_1M},    {"2", MT792X_RATE_CCK_2M},    {"5.5", MT792X_RATE_CCK_5_5M}, {"11", MT792X_RATE_CCK_11M},
    {"6", MT792X_RATE_OFDM_6M},   {"9", MT792X_RATE_OFDM_9M},   {"12", MT792X_RATE_OFDM_12M},  {"18", MT792X_RATE_OFDM_18M},
    {"24", MT792X_RATE_OFDM_24M}, {"36", MT792X_RATE_OFDM_36M}, {"48", MT792X_RATE_OFDM_48M},  {"54", MT792X_RATE_OFDM_54M},
};

int tx_channel_ok(uint8_t ch) {
    return (ch >= 1 && ch <= 13) || ch == 36 || ch == 40 || ch == 44 || ch == 48;
}

/* struct tx_cli: radiod.h */
void tx_cli_init(struct tx_cli *c) {
    memset(c, 0, sizeof(*c));
    c->rate = MT792X_RATE_OFDM_6M;
    /* [HOST] default cap 14 dBm (28 half-dB): below mt76's regulatory limits for PL (20 / 23 dBm), leaving
     * margin for the unknown antenna gain and the ACPI SAR tables mt76 would apply (spec 12.6) */
    c->extra.tx_power_max_half_db = 28;
}

/* Consumes argv[*i] (and its value) if it is a shared TX option: 1 = consumed, 0 = not ours, -1 = invalid
 * (already printed). */
int tx_cli_parse(struct tx_cli *c, int argc, char **argv, int *i) {
    const char *a = argv[*i];
    if (strcmp(a, "--rate") == 0 && *i + 1 < argc) {
        const char *v = argv[++*i];
        c->rate = 0;
        for (size_t k = 0; k < sizeof(txt_rates) / sizeof(txt_rates[0]); k++)
            if (strcmp(v, txt_rates[k].name) == 0) c->rate = txt_rates[k].rate;
        if (!c->rate) {
            printf("RESULT: FAIL - --rate must be one of 1 2 5.5 11 6 9 12 18 24 36 48 54\n");
            return -1;
        }
    } else if (strcmp(a, "--debug") == 0) c->debug = 1;
    else if (strcmp(a, "--pair") == 0) c->extra.vif_mode = MT792X_VIF_LINUX_PAIR;
    else if (strcmp(a, "--spe-v70") == 0) c->extra.tx_spe_mode = MT792X_SPE_LINUX_7_0;
    else if (strcmp(a, "--wcid-global") == 0) c->extra.tx_wcid_mode = MT792X_WCID_GLOBAL;
    else if (strcmp(a, "--no-power-limits") == 0) c->extra.tx_power_mode = MT792X_TXPWR_NONE;
    else if (strcmp(a, "--mt76-power") == 0) c->extra.tx_power_max_half_db = 0;
    else if (strcmp(a, "--max-dbm") == 0 && *i + 1 < argc) {
        int dbm = atoi(argv[++*i]);
        if (dbm < 1 || dbm > 23) {
            printf("RESULT: FAIL - --max-dbm must be 1..23 (it can only lower mt76's limits)\n");
            return -1;
        }
        c->extra.tx_power_max_half_db = (uint8_t)(2 * dbm);
    } else return 0;
    return 1;
}

/* Checks channel and rate, then: session, bring-up to post-boot, sniffer on 'ch' with TX allowed. Returns 0,
 * 1 if the arguments were refused before anything was touched (exit code 2), or -1 after a failure (printed;
 * the caller still closes the session). */
int tx_start(struct session *s, const char *patch_path, const char *ram_path, long chl, const struct tx_cli *c,
             uint8_t foreign_sa, const uint8_t *mac) {
    uint8_t ch, band;
    int err;
    if (chl < 1 || chl > 255 || !tx_channel_ok((uint8_t)chl)) {
        printf("RESULT: FAIL - transmitting is only allowed on 2.4 GHz channels 1-13 and 5 GHz channels 36, 40, 44, 48\n");
        return 1;
    }
    ch = (uint8_t)chl;
    band = band_of(ch);
    if (band == MT792X_BAND_5GHZ && c->rate <= MT792X_RATE_CCK_11M) {
        printf("RESULT: FAIL - CCK rates are not allowed on 5 GHz\n");
        return 1;
    }
    if (session_open(s, patch_path, ram_path) != 0) return -1;
    halwin_set_log_level(s->h, c->debug ? MT_LOG_DEBUG : MT_LOG_INFO);
    g_dev.opt.vif_mode = c->extra.vif_mode;
    g_dev.opt.tx_spe_mode = c->extra.tx_spe_mode;
    g_dev.opt.tx_wcid_mode = c->extra.tx_wcid_mode;
    g_dev.opt.tx_power_mode = c->extra.tx_power_mode;
    g_dev.opt.tx_power_max_half_db = c->extra.tx_power_max_half_db;
    g_dev.opt.tx_allow_foreign_sa = foreign_sa;
    err = mt792x_bringup(&g_dev, &s->fw, MT792X_STAGE_POSTBOOT);
    if (err) {
        printf("RESULT: FAIL - bring-up: %s (%d) at S%d\n", mt792x_strerror(err), err, mt792x_failed_step(&g_dev));
        return -1;
    }
    {
        struct mt792x_sniffer_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.channel = ch;
        cfg.band = band;
        cfg.flags = MT792X_SNIFF_CONTROL | MT792X_SNIFF_OTHER_BSS;
        cfg.allow_tx = 1;
        cfg.mac = mac; /* NULL: the chip's own address */
        err = mt792x_sniffer_start(&g_dev, &cfg);
        if (err) {
            printf("RESULT: FAIL - sniffer start: %s (%d) at SN%d\n", mt792x_strerror(err), err, g_dev.sniff_failed_step);
            return -1;
        }
    }
    return 0;
}

const char *tx_power_desc(const struct tx_cli *c) {
    return c->extra.tx_power_mode == MT792X_TXPWR_NONE ? "limits NOT sent (firmware defaults)"
           : c->extra.tx_power_max_half_db            ? "mt76 PL limits, capped by --max-dbm"
                                                       : "mt76 PL limits";
}

/* Probe request (802.11 probe request: wildcard SSID + supported rates), broadcast if 'da' is NULL, else
 * directed (address 1 = address 3 = 'da'). Rates per band, in 500 kb/s units with the basic bit: 2.4 GHz
 * 1, 2, 5.5, 11 (basic), 6, 9, 12, 18 + extended 24, 36, 48, 54; 5 GHz 6, 12, 24 (basic), 9, 18, 36, 48, 54. */
static uint32_t build_probe_req(uint8_t *f, const uint8_t *own, const uint8_t *da, uint16_t sn, uint8_t band) {
    static const uint8_t rates_2g[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    static const uint8_t ext_2g[4] = {0x30, 0x48, 0x60, 0x6c};
    static const uint8_t rates_5g[8] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    uint32_t n = 0;
    memset(f, 0, 24);
    f[0] = 0x40; /* management, subtype 4 */
    if (da) memcpy(f + 4, da, 6);
    else memset(f + 4, 0xff, 6);
    memcpy(f + 10, own, 6);
    if (da) memcpy(f + 16, da, 6);
    else memset(f + 16, 0xff, 6);
    f[22] = (uint8_t)(sn << 4);
    f[23] = (uint8_t)(sn >> 4);
    n = 24;
    f[n++] = 0; /* SSID element, length 0 = wildcard */
    f[n++] = 0;
    f[n++] = 1; /* supported rates */
    f[n++] = 8;
    memcpy(f + n, band == MT792X_BAND_2GHZ ? rates_2g : rates_5g, 8);
    n += 8;
    if (band == MT792X_BAND_2GHZ) {
        f[n++] = 50; /* extended supported rates */
        f[n++] = 4;
        memcpy(f + n, ext_2g, 4);
        n += 4;
    }
    return n;
}

static void txt_collect_done(struct txt_state *t, unsigned sent) {
    struct mt792x_tx_done d;
    while (mt792x_tx_done_get(&g_dev, &d) == 1)
        for (unsigned i = 0; i < sent; i++)
            if (t->cookie[i] == d.cookie && !t->done_seen[i]) {
                t->done[i] = d;
                t->done_seen[i] = 1;
            }
}

/* Polls RX until 'until_us'; records probe responses addressed to 'own' (arm 0) or, if 'ctl' is not NULL,
 * to 'ctl' (arm 1). Returns < 0 on a poll error. */
static int txt_listen(struct mt_hal *hal, struct txt_state *t, const uint8_t *own, const uint8_t *ctl, uint8_t probe,
                      uint8_t ch, uint64_t until_us, unsigned sent) {
    while (hal->now_us(hal->ctx) < until_us) {
        struct mt792x_rx_frame fr;
        int r = mt792x_rx_poll(&g_dev, &fr);
        if (r < 0) return r;
        txt_collect_done(t, sent);
        if (r == 0) {
            hal->sleep_us(hal->ctx, 500);
            continue;
        }
        if ((fr.flags & MT792X_RXF_HDR_TRANS) || !fr.fcs_ok || fr.len < 24 || fr.channel != ch) continue;
        if (fr.data[0] != 0x50) continue; /* not a probe response */
        uint8_t arm;
        if (memcmp(fr.data + 4, own, 6) == 0) arm = 0;
        else if (ctl && memcmp(fr.data + 4, ctl, 6) == 0) arm = 1;
        else continue;
        if (t->n >= TXT_MAX_RESP) {
            t->overflow++;
            continue;
        }
        struct txt_resp *x = &t->r[t->n++];
        memset(x, 0, sizeof(*x));
        x->arm = arm;
        memcpy(x->ta, fr.data + 10, 6);
        x->sn = (uint16_t)((fr.data[22] | (fr.data[23] << 8)) >> 4);
        x->retry = (fr.data[1] & 0x08) ? 1 : 0;
        x->rssi = fr.rssi_dbm;
        x->probe = probe;
        /* SSID: 24-byte header + 12 fixed bytes, first element */
        if (fr.len >= 38 && fr.data[36] == 0 && fr.data[37] <= 32 && 38u + fr.data[37] <= fr.len) {
            x->ssid_len = fr.data[37];
            memcpy(x->ssid, fr.data + 38, x->ssid_len);
        }
    }
    return 0;
}

static const char *txr_name(uint8_t r) {
    return r == MT792X_TXR_DONE ? "done" : r == MT792X_TXR_LOST ? "LOST" : r == MT792X_TXR_ABORTED ? "aborted" : "?";
}

/* One random control address per run: locally administered (bit 1 of the first octet set), unicast (bit 0
 * clear), different from our MAC. Returns 0, or -1 if the system random source fails. */
int random_control_mac(uint8_t *mac, const uint8_t *own) {
    for (int attempt = 0; attempt < 16; attempt++) {
        unsigned int v[2];
        if (rand_s(&v[0]) != 0 || rand_s(&v[1]) != 0) return -1;
        for (int k = 0; k < 6; k++) mac[k] = (uint8_t)(v[k / 3] >> (8 * (k % 3)));
        mac[0] = (uint8_t)((mac[0] | 0x02u) & 0xfeu);
        if (memcmp(mac, own, 6) != 0) return 0;
    }
    return -1;
}

/* Marks every response whose transmitter answered in both arms. */
static unsigned txt_mark_common(struct txt_state *t) {
    unsigned n_common = 0;
    for (unsigned k = 0; k < t->n; k++) {
        int in_arm[2] = {0, 0}, first = 1;
        for (unsigned m = 0; m < t->n; m++) {
            if (memcmp(t->r[m].ta, t->r[k].ta, 6) != 0) continue;
            in_arm[t->r[m].arm & 1u] = 1;
            if (m < k) first = 0;
        }
        t->r[k].common = (uint8_t)(in_arm[0] && in_arm[1]);
        if (t->r[k].common && first) n_common++;
    }
    return n_common;
}

/* Responses of one arm grouped by (transmitter, sequence number): an access point retransmits a response
 * without an ACK with the same sequence number, so "copies per pair" is the retransmission measure. */
static void txt_arm_stats(const struct txt_state *t, unsigned arm, int common_only, struct txt_arm *a) {
    memset(a, 0, sizeof(*a));
    for (unsigned k = 0; k < t->n; k++) {
        const struct txt_resp *x = &t->r[k];
        unsigned copies = 1;
        int first = 1;
        if (x->arm != arm || (common_only && !x->common)) continue;
        a->responses++;
        a->retried += x->retry;
        for (unsigned m = 0; m < t->n; m++) {
            const struct txt_resp *y = &t->r[m];
            if (m == k || y->arm != x->arm || y->sn != x->sn || memcmp(y->ta, x->ta, 6) != 0) continue;
            copies++;
            if (m < k) first = 0;
        }
        if (!first) continue;
        a->pairs++;
        a->hist[(copies > 5u ? 5u : copies) - 1u]++;
    }
    a->mean = a->pairs ? (double)a->responses / (double)a->pairs : 0.0;
}

static void txt_print_arm(const char *label, unsigned sent, const struct txt_arm *a) {
    printf("  %-11s probes %u, responses %u, (transmitter, SN) pairs %u, copies per pair 1:%u 2:%u 3:%u 4:%u 5+:%u, mean %.2f, Retry bit %u of %u (%.0f%%)\n",
           label, sent, a->responses, a->pairs, a->hist[0], a->hist[1], a->hist[2], a->hist[3], a->hist[4], a->mean,
           a->retried, a->responses, a->responses ? 100.0 * (double)a->retried / (double)a->responses : 0.0);
}

static int txtest(int argc, char **argv) {
    static struct txt_state t; /* large: keep it off the stack */
    struct session s;
    unsigned count = 5, show_ssid = 0, ssid_len_only = 0, sent = 0, control = 0;
    uint8_t rate, ch, band;
    struct tx_cli cli;
    int rc = 1, err, poll_err = 0, st;
    long chl = strtol(argv[2], NULL, 10);

    tx_cli_init(&cli);
    memset(&t, 0, sizeof(t));
    for (int i = 5; i < argc; i++) {
        int used = tx_cli_parse(&cli, argc, argv, &i);
        if (used < 0) return 2;
        if (used) continue;
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--ssid-len-only") == 0) ssid_len_only = 1;
        else if (strcmp(argv[i], "--show-ssid") == 0) show_ssid = 1;
        else if (strcmp(argv[i], "--control") == 0) control = 1;
        else {
            printf("RESULT: FAIL - unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (count < 1 || count > TXT_MAX_PROBES) {
        printf("RESULT: FAIL - --count must be 1..%d%s\n", TXT_MAX_PROBES, control ? " (per arm)" : "");
        return 2;
    }
    rate = cli.rate;
    st = tx_start(&s, argv[3], argv[4], chl, &cli, (uint8_t)control, NULL);
    if (st > 0) return 2;
    if (st < 0) goto out;
    ch = (uint8_t)chl;
    band = band_of(ch);
    {
        struct mt_hal *hal = halwin_hal(s.h);
        const uint8_t *own = mt792x_own_mac(&g_dev);
        uint8_t frame[64], ctl_mac[6] = {0, 0, 0, 0, 0, 0};
        const uint8_t *ctl = NULL;
        unsigned frames = control ? 2u * count : count; /* in control mode: own, control, own, control, ... */
        if (control) {
            if (random_control_mac(ctl_mac, own) != 0) {
                printf("RESULT: FAIL - no random control address available (rand_s failed)\n");
                goto out;
            }
            ctl = ctl_mac;
        }
        printf("txtest: channel %u, %u probe request(s)", ch, frames);
        if (control) printf(" (%u per arm: own MAC / control MAC)", count);
        printf(", rate code %u, own MAC ", rate);
        print_masked_addr(own);
        if (control) {
            printf(", control MAC ");
            print_masked_addr(ctl);
        }
        printf(", vifs %s, TX power %s\n", cli.extra.vif_mode ? "Linux pair" : "single", tx_power_desc(&cli));
        for (unsigned i = 0; i < frames && !poll_err; i++) {
            struct mt792x_tx_req q;
            uint64_t t0 = hal->now_us(hal->ctx);
            unsigned arm = control ? (i & 1u) : 0u;
            uint32_t len = build_probe_req(frame, arm ? ctl : own, NULL, (uint16_t)(0x100 + i), band);
            memset(&q, 0, sizeof(q));
            q.frame = frame;
            q.len = len;
            q.rate = rate;
            q.want_ack = 0; /* broadcast */
            err = mt792x_tx_frame(&g_dev, &q, &t.cookie[i]);
            if (err) {
                printf("probe %u: TX refused: %s (%d)\n", i + 1, mt792x_strerror(err), err);
                poll_err = err;
                break;
            }
            sent = i + 1;
            unsigned before = t.n;
            poll_err = txt_listen(hal, &t, own, ctl, (uint8_t)i, ch, t0 + 300000u, sent);
            unsigned n = 0, retries = 0;
            int lo = 0, hi = -128;
            for (unsigned k = before; k < t.n; k++) {
                if (t.r[k].arm != arm) continue; /* a late response of the other arm: counted in the summary only */
                retries += t.r[k].retry;
                if (n == 0 || t.r[k].rssi < lo) lo = t.r[k].rssi;
                if (t.r[k].rssi > hi) hi = t.r[k].rssi;
                n++;
            }
            if (control) printf("probe %u [%s]: TX %s", i + 1, arm ? "control MAC" : "own MAC", t.done_seen[i] ? txr_name(t.done[i].result) : "pending");
            else printf("probe %u: TX %s", i + 1, t.done_seen[i] ? txr_name(t.done[i].result) : "pending");
            if (t.done_seen[i])
                printf(" (TX-free status %u, transmissions %u, %u us)", t.done[i].free_status, t.done[i].tx_count,
                       t.done[i].elapsed_us);
            printf("; %u probe response(s) to %s, %u with Retry bit", n, arm ? "the control MAC" : "us", retries);
            if (n) printf(", RSSI %d..%d dBm, from", lo, hi);
            for (unsigned k = before; k < t.n; k++) {
                int dup = 0;
                if (t.r[k].arm != arm) continue;
                for (unsigned m = before; m < k; m++)
                    if (t.r[m].arm == arm && memcmp(t.r[m].ta, t.r[k].ta, 3) == 0) dup = 1;
                if (!dup) printf(" %02x:%02x:%02x:xx:xx:xx", t.r[k].ta[0], t.r[k].ta[1], t.r[k].ta[2]);
            }
            printf("\n");
            if (!poll_err && i + 1 < frames) poll_err = txt_listen(hal, &t, own, ctl, (uint8_t)i, ch, t0 + 1000000u, sent);
        }
        if (!poll_err)
            poll_err = txt_listen(hal, &t, own, ctl, (uint8_t)(sent ? sent - 1 : 0), ch, hal->now_us(hal->ctx) + 300000u, sent);
        if (poll_err) printf("receive/transmit loop stopped: %s (%d)\n", mt792x_strerror(poll_err), poll_err);

        /* ---- analysis ---- */
        /* 'total' feeds the single-arm analysis below and stays 0 in control mode (see txt_arm_stats) */
        unsigned total = control ? 0u : t.n, no_retry = 0, uniq = 0, uniq_retried = 0, uniq_repeated = 0, aps = 0;
        unsigned own_total = 0;
        for (unsigned k = 0; k < t.n; k++) own_total += t.r[k].arm == 0;
        for (unsigned k = 0; k < total; k++) {
            int first = 1, retried = t.r[k].retry, repeats = 0;
            no_retry += !t.r[k].retry;
            for (unsigned m = 0; m < total; m++) {
                if (m == k || memcmp(t.r[m].ta, t.r[k].ta, 6) != 0 || t.r[m].sn != t.r[k].sn) continue;
                if (m < k) first = 0;
                repeats++;
                retried |= t.r[m].retry;
            }
            if (!first) continue;
            uniq++;
            if (retried) uniq_retried++;
            if (repeats) uniq_repeated++;
            int new_ap = 1;
            for (unsigned m = 0; m < k; m++)
                if (memcmp(t.r[m].ta, t.r[k].ta, 6) == 0) new_ap = 0;
            aps += new_ap;
        }
        const struct mt792x_tx_counters *tc = mt792x_tx_counters(&g_dev);
        printf("\n==== txtest summary ====\n");
        printf("probe requests sent %u; TX records: done %u, lost %u; TX-free entries %u, unknown tokens %u, quarantined %u\n",
               sent, tc->done, tc->lost, tc->txfree_entries, tc->txfree_unknown, tc->quarantined);
        if (!control)
            printf("probe responses to our MAC: %u (%u without Retry bit) from %u transmitter(s); %u distinct sequence numbers, %u of them with a Retry copy, %u seen more than once%s\n",
                   total, no_retry, aps, uniq, uniq_retried, uniq_repeated, t.overflow ? " (table full)" : "");
        if (ssid_len_only || show_ssid) {
            for (unsigned k = 0; k < t.n; k++) {
                int dup = 0;
                for (unsigned m = 0; m < k; m++)
                    if (memcmp(t.r[m].ta, t.r[k].ta, 6) == 0) dup = 1;
                if (dup) continue;
                printf("  responder %02x:%02x:%02x:xx:xx:xx SSID length %u", t.r[k].ta[0], t.r[k].ta[1], t.r[k].ta[2],
                       t.r[k].ssid_len);
                if (show_ssid) {
                    printf(" \"");
                    for (unsigned c = 0; c < t.r[k].ssid_len; c++) {
                        uint8_t b = t.r[k].ssid[c];
                        if (b >= 0x20 && b < 0x7f && b != '"' && b != '\\') putchar(b);
                        else printf("\\x%02x", b);
                    }
                    printf("\"");
                }
                printf("\n");
            }
        }
        if (own_total >= 1) {
            printf("TX: PASS - %u probe response(s) addressed to our MAC: our probe requests were radiated and decoded\n", own_total);
            rc = 0;
        } else {
            printf("TX: FAIL - no probe response addressed to our MAC (is an access point on channel %u?)\n", ch);
        }
        if (control) {
            /* Control arm comparison (see the comment above): only transmitters that answered in both arms,
             * so that distance, load and retry policy are the same for both. The verdict needs >= 5
             * (transmitter, SN) pairs in each arm from those transmitters. An access point that gets no ACK
             * retransmits up to its retry limit, so the control arm's mean copies per pair is the "no ACK"
             * baseline; it only means something if it is clearly above 1 (>= 2.0). Own-MAC responses at
             * <= 60 % of that baseline: our chip acknowledges; >= 90 %: it does not. In between, or with
             * too little data: INCONCLUSIVE. */
            struct txt_arm own_all, ctl_all, own_com, ctl_com;
            unsigned n_common = txt_mark_common(&t), sent_own = (sent + 1u) / 2u, sent_ctl = sent / 2u;
            txt_arm_stats(&t, 0, 0, &own_all);
            txt_arm_stats(&t, 1, 0, &ctl_all);
            txt_arm_stats(&t, 0, 1, &own_com);
            txt_arm_stats(&t, 1, 1, &ctl_com);
            printf("control arm: probes from a random locally administered address that nobody acknowledges%s\n",
                   t.overflow ? " (response table full, some responses dropped)" : "");
            printf("all transmitters:\n");
            txt_print_arm("own MAC", sent_own, &own_all);
            txt_print_arm("control MAC", sent_ctl, &ctl_all);
            printf("transmitters that answered in both arms: %u\n", n_common);
            txt_print_arm("own MAC", sent_own, &own_com);
            txt_print_arm("control MAC", sent_ctl, &ctl_com);
            if (own_com.pairs >= 5 && ctl_com.pairs >= 5) {
                double mo = own_com.mean, mc = ctl_com.mean;
                if (mc >= 2.0 && mo <= 0.6 * mc)
                    printf("ACK: WORKING - mean copies per pair %.2f (own MAC) vs %.2f (control MAC) = %.0f%%; %u / %u pairs, %u common transmitter(s)\n",
                           mo, mc, 100.0 * mo / mc, own_com.pairs, ctl_com.pairs, n_common);
                else if (mc >= 2.0 && mo >= 0.9 * mc)
                    printf("ACK: NOT WORKING - mean copies per pair %.2f (own MAC) vs %.2f (control MAC) = %.0f%%; %u / %u pairs, %u common transmitter(s)\n",
                           mo, mc, 100.0 * mo / mc, own_com.pairs, ctl_com.pairs, n_common);
                else
                    printf("ACK: INCONCLUSIVE - mean copies per pair %.2f (own MAC) vs %.2f (control MAC)%s; %u / %u pairs, %u common transmitter(s)\n",
                           mo, mc, mc >= 2.0 ? "" : " (control baseline below 2.0: the access points did not retransmit enough)",
                           own_com.pairs, ctl_com.pairs, n_common);
            } else {
                printf("ACK: INCONCLUSIVE - too little data: %u pair(s) own MAC, %u pair(s) control MAC from %u common transmitter(s) (need >= 5 each); mean copies per pair %.2f vs %.2f\n",
                       own_com.pairs, ctl_com.pairs, n_common, own_com.mean, ctl_com.mean);
            }
        } else if (total >= 5 && no_retry * 100u >= 80u * total && uniq_repeated == 0)
            printf("ACK: LIKELY WORKING - %u of %u responses without Retry bit, no sequence number retransmitted\n", no_retry,
                   total);
        else if (uniq && uniq_retried * 100u >= 80u * uniq)
            printf("ACK: NOT WORKING - %u of %u response sequence numbers were retransmitted with the Retry bit\n",
                   uniq_retried, uniq);
        else
            printf("ACK: INCONCLUSIVE - responses %u, without Retry %u, sequence numbers %u, retransmitted %u, seen twice %u\n",
                   total, no_retry, uniq, uniq_retried, uniq_repeated);
    }

out:
    session_close(&s);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ */
/* acktest (F-010 / F-013 follow-up: unicast TX with ACK request and TX status report)              */
/*
 * Two arms alternate, one frame per second, each frame a directed probe request sent with an ACK request
 * (retries allowed) and a TX status request (TXS):
 *   - AP arm: address 1 = address 3 = the BSSID of an access point on the channel. Every 802.11 station
 *     acknowledges a unicast frame addressed to it that it received with a good FCS, associated or not,
 *     so the TXS should say "ACKed" and the TX-free COUNT (transmissions) should mostly be 1. The access
 *     point usually also answers with a probe response: independent proof that it received the frame.
 *   - ghost arm: the same frame to a random locally administered address that no station owns. Nobody can
 *     acknowledge it, so the TXS must say "not ACKed" and the chip should have used all its tries.
 * Only both arms together give a verdict: "ACKed" for the ghost, or "not ACKed" for frames the access point
 * demonstrably received, would mean the TXS ACK bits (mt76: TXS0 bits 18:16 clear = ACKed) do not mean what
 * we read them as.
 */

#define ACK_MAX_PER_ARM 20
#define ACK_MAX_BSS 64

struct ack_bss {
    uint8_t mac[6];
    unsigned beacons, with_rssi;
    int rssi_sum;
};

struct ack_arm {
    unsigned sent, done, lost, aborted, pending;
    unsigned txs, yes, no, unknown;
    unsigned answered;        /* frames the access point answered with a probe response to us */
    unsigned tries_hist[16];  /* TX-free COUNT 0..15 (0 = not reported) */
};

/* aa:bb:cc:dd:ee:ff or aa-bb-cc-dd-ee-ff (s3-bringup.ps1 turns ':' into spaces). */
static int parse_mac(const char *s, uint8_t *mac) {
    unsigned v[6];
    char tail, buf[32];
    size_t n = strlen(s);
    if (n != 17) return -1;
    memcpy(buf, s, n + 1);
    for (size_t k = 0; k < n; k++)
        if (buf[k] == '-') buf[k] = ':';
    if (sscanf(buf, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) return -1;
    for (int k = 0; k < 6; k++) {
        if (v[k] > 0xff) return -1;
        mac[k] = (uint8_t)v[k];
    }
    return (mac[0] & 1u) ? -1 : 0; /* unicast only */
}

/* Listens for 'ms' and picks the beaconing BSSID with the best mean RSSI (at least 3 beacons with good FCS
 * and an RSSI on the tuned channel). Returns 1 with 'target' set, 0 if none qualified, < 0 on a poll error. */
static int ack_scan(struct mt_hal *hal, uint8_t ch, uint32_t ms, uint8_t *target, unsigned *n_bss, int *rssi_mean,
                    unsigned *beacons) {
    static struct ack_bss b[ACK_MAX_BSS];
    unsigned n = 0, best = ACK_MAX_BSS;
    uint64_t until = hal->now_us(hal->ctx) + (uint64_t)ms * 1000u;
    memset(b, 0, sizeof(b));
    while (hal->now_us(hal->ctx) < until) {
        struct mt792x_rx_frame fr;
        int r = mt792x_rx_poll(&g_dev, &fr);
        if (r < 0) return r;
        if (r == 0) {
            hal->sleep_us(hal->ctx, 500);
            continue;
        }
        if ((fr.flags & MT792X_RXF_HDR_TRANS) || !fr.fcs_ok || fr.len < 24 || fr.channel != ch) continue;
        if ((fr.data[0] & 0xfc) != 0x80 || (fr.data[10] & 1u)) continue; /* beacons from a unicast address */
        unsigned k;
        for (k = 0; k < n; k++)
            if (memcmp(b[k].mac, fr.data + 10, 6) == 0) break;
        if (k == n) {
            if (n == ACK_MAX_BSS) continue;
            memcpy(b[n++].mac, fr.data + 10, 6);
        }
        b[k].beacons++;
        if (fr.rssi_dbm != -128) {
            b[k].with_rssi++;
            b[k].rssi_sum += fr.rssi_dbm;
        }
    }
    for (unsigned k = 0; k < n; k++) {
        if (b[k].beacons < 3 || !b[k].with_rssi) continue;
        if (best == ACK_MAX_BSS ||
            b[k].rssi_sum * (int)b[best].with_rssi > b[best].rssi_sum * (int)b[k].with_rssi)
            best = k;
    }
    *n_bss = n;
    if (best == ACK_MAX_BSS) return 0;
    memcpy(target, b[best].mac, 6);
    *rssi_mean = b[best].rssi_sum / (int)b[best].with_rssi;
    *beacons = b[best].beacons;
    return 1;
}

static const char *ack_name(uint8_t a) {
    return a == MT792X_ACK_YES ? "ACKed" : a == MT792X_ACK_NO ? "NOT ACKed" : a == MT792X_ACK_UNKNOWN ? "no TXS" : "?";
}

/* Probe responses to us from 'from' recorded for frame 'probe'. */
static unsigned ack_answers(const struct txt_state *t, const uint8_t *from, unsigned probe) {
    unsigned n = 0;
    for (unsigned k = 0; k < t->n; k++)
        if (t->r[k].probe == probe && memcmp(t->r[k].ta, from, 6) == 0) n++;
    return n;
}

static void ack_print_arm(const char *label, const struct ack_arm *a) {
    unsigned sum = 0, n = 0;
    printf("  %-6s frames %u: TX-free %u, lost %u, aborted %u, pending %u; TXS %u: ACKed %u, NOT ACKed %u, no TXS %u; "
           "transmissions",
           label, a->sent, a->done, a->lost, a->aborted, a->pending, a->txs, a->yes, a->no, a->unknown);
    for (unsigned c = 0; c < 16; c++) {
        if (!a->tries_hist[c]) continue;
        printf(" %u:%u", c, a->tries_hist[c]);
        if (c) {
            sum += c * a->tries_hist[c];
            n += a->tries_hist[c];
        }
    }
    printf(" (mean %.2f)", n ? (double)sum / (double)n : 0.0);
    printf("; answered by the access point %u\n", a->answered);
}

static int acktest(int argc, char **argv) {
    static struct txt_state t; /* large: keep it off the stack */
    struct session s;
    struct tx_cli cli;
    unsigned count = 5, tries = 15, scan_ms = 2000, sent = 0;
    uint8_t target[6], ghost[6], ch, band;
    int have_target = 0, rc = 1, err, poll_err = 0, st;
    long chl = strtol(argv[2], NULL, 10);

    tx_cli_init(&cli);
    memset(&t, 0, sizeof(t));
    for (int i = 5; i < argc; i++) {
        int used = tx_cli_parse(&cli, argc, argv, &i);
        if (used < 0) return 2;
        if (used) continue;
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--tries") == 0 && i + 1 < argc) tries = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--scan-ms") == 0 && i + 1 < argc) scan_ms = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            if (parse_mac(argv[++i], target) != 0) {
                printf("RESULT: FAIL - --target must be a unicast MAC address aa:bb:cc:dd:ee:ff\n");
                return 2;
            }
            have_target = 1;
        } else {
            printf("RESULT: FAIL - unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (count < 1 || count > ACK_MAX_PER_ARM) {
        printf("RESULT: FAIL - --count must be 1..%d (per arm)\n", ACK_MAX_PER_ARM);
        return 2;
    }
    if (tries < 1 || tries > 15) {
        printf("RESULT: FAIL - --tries must be 1..15 (transmissions per frame including retries)\n");
        return 2;
    }
    if (scan_ms < 500 || scan_ms > 10000) {
        printf("RESULT: FAIL - --scan-ms must be 500..10000\n");
        return 2;
    }
    st = tx_start(&s, argv[3], argv[4], chl, &cli, 0, NULL);
    if (st > 0) return 2;
    if (st < 0) goto out;
    ch = (uint8_t)chl;
    band = band_of(ch);
    {
        struct mt_hal *hal = halwin_hal(s.h);
        const uint8_t *own = mt792x_own_mac(&g_dev);
        uint8_t frame[64];
        unsigned frames = 2u * count;
        struct ack_arm arm[2];

        memset(arm, 0, sizeof(arm));
        if (random_control_mac(ghost, own) != 0) {
            printf("RESULT: FAIL - no random ghost address available (rand_s failed)\n");
            goto out;
        }
        if (!have_target) {
            unsigned n_bss = 0, beacons = 0;
            int rssi = 0;
            int r = ack_scan(hal, ch, scan_ms, target, &n_bss, &rssi, &beacons);
            if (r < 0) {
                printf("RESULT: FAIL - receive error while scanning: %s (%d)\n", mt792x_strerror(r), r);
                goto out;
            }
            if (r == 0) {
                printf("RESULT: FAIL - no access point with >= 3 beacons heard on channel %u in %u ms (%u BSSID(s) seen)\n",
                       ch, scan_ms, n_bss);
                goto out;
            }
            printf("scan: %u BSSID(s) beaconing on channel %u in %u ms; target ", n_bss, ch, scan_ms);
            print_masked_addr(target);
            printf(" (%u beacons, mean RSSI %d dBm)\n", beacons, rssi);
        }
        printf("acktest: channel %u, %u directed probe request(s) (%u per arm: access point / ghost), ACK + TX status "
               "requested, %u transmission(s) allowed per frame, rate code %u, own MAC ",
               ch, frames, count, tries, cli.rate);
        print_masked_addr(own);
        printf(", target ");
        print_masked_addr(target);
        printf(", ghost ");
        print_masked_addr(ghost);
        printf(", vifs %s, TX power %s\n", cli.extra.vif_mode ? "Linux pair" : "single", tx_power_desc(&cli));

        for (unsigned i = 0; i < frames && !poll_err; i++) {
            struct mt792x_tx_req q;
            uint64_t t0 = hal->now_us(hal->ctx);
            unsigned a = i & 1u; /* 0 = access point, 1 = ghost */
            uint32_t len = build_probe_req(frame, own, a ? ghost : target, (uint16_t)(0x200 + i), band);
            memset(&q, 0, sizeof(q));
            q.frame = frame;
            q.len = len;
            q.rate = cli.rate;
            q.want_ack = 1;
            q.want_status = 1;
            q.tx_count = (uint8_t)tries;
            err = mt792x_tx_frame(&g_dev, &q, &t.cookie[i]);
            if (err) {
                printf("frame %u: TX refused: %s (%d)\n", i + 1, mt792x_strerror(err), err);
                poll_err = err;
                break;
            }
            sent = i + 1;
            /* listen until the record is complete (TX-free + TXS, or the TXS wait ran out), at most 1 s */
            while (!poll_err && !t.done_seen[i] && hal->now_us(hal->ctx) < t0 + 1000000u)
                poll_err = txt_listen(hal, &t, own, NULL, (uint8_t)i, ch, hal->now_us(hal->ctx) + 20000u, sent);
            printf("frame %u [%s]: ", i + 1, a ? "ghost" : "AP");
            if (t.done_seen[i]) {
                const struct mt792x_tx_done *d = &t.done[i];
                printf("%s (TX-free status %u, transmissions %u, %u us); %s", txr_name(d->result), d->free_status,
                       d->tx_count, d->elapsed_us, ack_name(d->acked));
                if (d->has_txs) printf(" (TXS word0 0x%08x, rate 0x%03x)", d->txs[0], d->txs_rate);
            } else {
                printf("no TX record within 1 s");
            }
            /* the rest of the second: responses to this frame */
            if (!poll_err) poll_err = txt_listen(hal, &t, own, NULL, (uint8_t)i, ch, t0 + (i + 1 < frames ? 1000000u : 300000u), sent);
            printf("; probe responses from the target %u\n", ack_answers(&t, target, i));
        }
        /* late records: the library reports LOST after its TX timeout (2 s) at the latest */
        {
            uint64_t until = hal->now_us(hal->ctx) + 2500000u;
            int all = 0;
            while (!poll_err && !all && hal->now_us(hal->ctx) < until) {
                poll_err = txt_listen(hal, &t, own, NULL, (uint8_t)(sent ? sent - 1 : 0), ch, hal->now_us(hal->ctx) + 20000u,
                                      sent);
                all = 1;
                for (unsigned i = 0; i < sent; i++)
                    if (!t.done_seen[i]) all = 0;
            }
        }
        if (poll_err) printf("receive/transmit loop stopped: %s (%d)\n", mt792x_strerror(poll_err), poll_err);

        /* ---- analysis ---- */
        for (unsigned i = 0; i < sent; i++) {
            struct ack_arm *x = &arm[i & 1u];
            x->sent++;
            if (ack_answers(&t, target, i)) x->answered++;
            if (!t.done_seen[i]) {
                x->pending++;
                continue;
            }
            const struct mt792x_tx_done *d = &t.done[i];
            if (d->result == MT792X_TXR_DONE) x->done++;
            else if (d->result == MT792X_TXR_LOST) x->lost++;
            else x->aborted++;
            if (d->has_txs) x->txs++;
            if (d->acked == MT792X_ACK_YES) x->yes++;
            else if (d->acked == MT792X_ACK_NO) x->no++;
            else x->unknown++;
            if (d->result == MT792X_TXR_DONE) x->tries_hist[d->tx_count > 15u ? 15u : d->tx_count]++;
        }
        const struct mt792x_tx_counters *tc = mt792x_tx_counters(&g_dev);
        const struct mt792x_rx_counters *rxc = mt792x_rx_counters(&g_dev);
        const struct ack_arm *ap = &arm[0], *gh = &arm[1];
        printf("\n==== acktest summary ====\n");
        printf("frames sent %u; library: done %u, lost %u, ACKed %u, not ACKed %u; TXS records %u (unmatched %u); "
               "RX packets: TXS %u, TX_DONE events %u, TX-free MSDUs %u\n",
               sent, tc->done, tc->lost, tc->acked, tc->not_acked, tc->txs_records, tc->txs_unmatched, rxc->pkt_type[0],
               rxc->event_by_id[0x0f], rxc->tx_free_msdus);
        ack_print_arm("AP", ap);
        ack_print_arm("ghost", gh);

        /* Reception by the access point is proven by a probe response, or by an ACK as long as the ghost (whose
         * frames nobody can receive) was never reported ACKed. Some BSSIDs (hidden or virtual networks) never
         * answer a wildcard probe (F-014), so a missing probe response alone is not a TX failure. */
        if (ap->answered || (ap->yes && !gh->yes))
            printf("TX: PASS - the access point received our frames: %u of %u ACKed, %u answered with a probe response\n",
                   ap->yes, ap->sent, ap->answered);
        else printf("TX: FAIL - the access point neither ACKed nor answered any of the directed probes\n");

        if (ap->txs + gh->txs == 0) {
            printf("TXS: NOT DELIVERED - no TX status record for any of %u frames that asked for one\n", sent);
        } else {
            printf("TXS: DELIVERED - %u of %u frames got a TX status record\n", ap->txs + gh->txs, sent);
        }

        int cover = ap->txs * 5u >= ap->sent * 4u && gh->txs * 5u >= gh->sent * 4u && ap->sent && gh->sent;
        if (gh->yes)
            printf("UNICAST ACK: CONTRADICTION - the ghost address, which nobody owns, was reported ACKed %u time(s): the "
                   "TXS ACK bits do not mean what we read them as\n", gh->yes);
        else if (cover && ap->answered && ap->yes == 0)
            printf("UNICAST ACK: CONTRADICTION - the access point answered %u probe(s) but none of its frames was reported "
                   "ACKed\n", ap->answered);
        else if (cover && ap->yes * 5u >= ap->txs * 4u && gh->no == gh->txs) {
            printf("UNICAST ACK: WORKING - access point ACKed %u of %u, ghost NOT ACKed %u of %u\n", ap->yes, ap->txs, gh->no,
                   gh->txs);
            rc = 0;
        } else if (cover && ap->yes == 0)
            printf("UNICAST ACK: NOT WORKING - no frame to the access point was ACKed, and it never answered\n");
        else
            printf("UNICAST ACK: INCONCLUSIVE - TXS for %u/%u (AP) and %u/%u (ghost) frames, need >= 80%% each; AP ACKed "
                   "%u, ghost NOT ACKed %u\n", ap->txs, ap->sent, gh->txs, gh->sent, ap->yes, gh->no);

        /* Retries: a frame nobody ACKs should use every allowed transmission. */
        {
            unsigned full = gh->tries_hist[tries], n = 0;
            for (unsigned c = 1; c < 16; c++) n += gh->tries_hist[c];
            if (n == 0) printf("RETRIES: UNKNOWN - no TX-free COUNT reported for the ghost frames\n");
            else if (full == n) printf("RETRIES: AS REQUESTED - every ghost frame used all %u transmission(s)\n", tries);
            else printf("RETRIES: DIFFERENT - %u of %u ghost frames used all %u transmission(s); see the histogram\n", full, n,
                        tries);
        }
    }

out:
    session_close(&s);
    return rc;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--live-log") == 0) {
        if (out_open_live(argv[2]) != 0) {
            printf("RESULT: FAIL - cannot open the live log %s\n", argv[2]);
            return 2;
        }
        printf("=== radiod live log:");
        for (int i = 3; i < argc; i++) printf(" %s", argv[i]);
        printf("\n");
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
    }
    printf("radiod (mt792x library %s)\n", mt792x_version());
    if (argc >= 2 && strcmp(argv[1], "dma-probe") == 0) return dma_probe();
    if (argc == 5 && strcmp(argv[1], "bringup") == 0) return bringup(atoi(argv[2]), argv[3], argv[4]);
    if (argc >= 6 && strcmp(argv[1], "sniff") == 0) return sniff(argc, argv);
    if (argc >= 5 && strcmp(argv[1], "txtest") == 0) return txtest(argc, argv);
    if (argc >= 5 && strcmp(argv[1], "acktest") == 0) return acktest(argc, argv);
    if (argc >= 5 && strcmp(argv[1], "serve") == 0) return serve(argc, argv);
    printf("usage: radiod [--live-log <file>] <command> ...\n"
           "         --live-log <file>      also append each line (not DEBUG) to <file>, written through to disk\n"
           "       radiod dma-probe\n"
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
           "         --debug                print DEBUG library messages\n"
           "       radiod txtest <channel 1-13|36|40|44|48> <patch file> <ram file> [options]\n"
           "         --count N              probe requests, one per second (default 5, max 20; with --control per arm)\n"
           "         --control              ACK control arm: alternate probes from our MAC and from a random\n"
           "                                locally administered address nobody acknowledges (max 2 x 20 frames),\n"
           "                                compare how often the access points retransmit their responses\n"
           "         --rate R               1 2 5.5 11 (2.4 GHz only) 6 9 12 18 24 36 48 54 (default 6)\n"
           "         --ssid-len-only        list responders with their SSID length\n"
           "         --show-ssid            ... and the SSID\n"
           "         --pair                 Linux plain+active vif pair (OMAC 1 = our MAC)\n"
           "         --spe-v70              TXD7 SPE_IDX as Linux 7.0 (0)\n"
           "         --wcid-global          transmit with wcid 0 instead of the vif's\n"
           "         --max-dbm D            cap every TX power limit at D dBm (1..23, default 14)\n"
           "         --mt76-power           no extra cap: exactly mt76's limits for PL (20 / 23 dBm)\n"
           "         --no-power-limits      do not send SET_RATE_TX_POWER (firmware defaults)\n"
           "         --debug                print DEBUG library messages\n"
           "       radiod acktest <channel 1-13|36|40|44|48> <patch file> <ram file> [options]\n"
           "         directed probe requests with ACK + TX status requested, alternating: access point / ghost\n"
           "         (a random locally administered address nobody owns), one per second\n"
           "         --count N              frames per arm (default 5, max 20)\n"
           "         --tries T              transmissions allowed per frame incl. retries (1..15, default 15 = mt76)\n"
           "         --target MAC           access point to address, aa:bb:cc:dd:ee:ff or aa-bb-cc-dd-ee-ff\n"
           "                                (default: the beaconing BSSID with the best mean RSSI)\n"
           "         --scan-ms N            how long to listen for beacons first (500..10000, default 2000)\n"
           "         --rate, --pair, --spe-v70, --wcid-global, --max-dbm, --mt76-power, --no-power-limits,\n"
           "         --debug                as for txtest\n"
           "       radiod serve <channel 1-13|36|40|44|48> <patch file> <ram file> [options]\n"
           "         radio server for awdld / radioctl on " RL_PIPE_NAME " (ADR-0003); one client,\n"
           "         ends when it disconnects or is silent for 5 s\n"
           "         --max-seconds N        session limit (5..900, default 120)\n"
           "         --wait-client S        how long to wait for the client (1..300, default 60)\n"
           "         --mac chip|random      address to ACK and transmit from (default chip)\n"
           "         --no-filter            forward every frame, not only AWDL (BSSID 00:25:00:ff:94:73)\n"
           "         --loopback             synthetic radio, NO hardware access (patch/ram arguments ignored):\n"
           "                                for testing clients offline\n"
           "         --pair, --max-dbm, --mt76-power, --no-power-limits, --debug   as for txtest\n");
    return 2;
}
