/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RX data path: the RXD parser (pure), its bounds behaviour under fuzzing, and mt792x_rx_poll
 * against packets injected into the simulated chip's RX rings. */
#include "test.h"
#include "rig.h"
#include "rx_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t ap[6] = { 0x00, 0x11, 0x32, 0xaa, 0xbb, 0xcc };
static const uint8_t sta[6] = { 0x3c, 0x22, 0xfb, 0x01, 0x02, 0x03 };
static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Parses a copy of exactly 'len' bytes from the heap. */
static int parse_copy(const uint8_t *pkt, uint32_t len, uint8_t fcs_mode, uint8_t mask, struct mt792x_rx_frame *out,
                      uint8_t *frame_copy, uint32_t *frame_len)
{
    uint8_t *b = (uint8_t *)malloc(len ? len : 1u);
    int rc;
    memcpy(b, pkt, len);
    rc = mt792x_rx_parse(b, len, fcs_mode, mask, out);
    if (rc == 0) {
        memcpy(frame_copy, out->data, out->len);
        *frame_len = out->len;
    }
    free(b);
    out->data = NULL;
    return rc;
}

TEST(rx_parse_every_group_combination)
{
    uint8_t frame[128], pkt[512], got[512];
    uint32_t flen, len, glen = 0;
    unsigned combo, pad;
    flen = rxb_beacon(frame, sizeof(frame), ap, "home", 6);
    for (combo = 0; combo < 32; combo++) {
        for (pad = 0; pad < 4; pad++) {
            struct rxb b;
            struct mt792x_rx_frame fr;
            memset(&b, 0, sizeof(b));
            b.g1 = combo & 1;
            b.g2 = (combo >> 1) & 1;
            b.g3 = (combo >> 2) & 1;
            b.g4 = (combo >> 3) & 1;
            b.g5 = (combo >> 4) & 1;
            b.hdr_offset = pad;
            b.ch_freq = 6;
            b.timestamp = 0x12345678u;
            b.prxv0 = (1u << 24) | 11u;               /* OFDM, idx 11 */
            b.rcpi[0] = 100;
            b.rcpi[1] = 110;
            b.g5_rcpi[0] = 120;
            b.g5_rcpi[1] = 120;
            b.g4_fc = 0x0080;
            b.frame = frame;
            b.frame_len = flen;
            len = rxb_build(&b, pkt, sizeof(pkt));
            CHECK(len > 0);
            CHECK(parse_copy(pkt, len, MT792X_FCS_MT76, 0x3, &fr, got, &glen) == 0);
            CHECK_EQ_U32(glen, flen);
            CHECK(memcmp(got, frame, flen) == 0);
            CHECK_EQ_U32(fr.channel, 6);
            CHECK_EQ_U32(fr.band, MT792X_BAND_2GHZ);
            CHECK_EQ_U32(fr.fcs_ok, 1);
            CHECK_EQ_U32(fr.raw_len, len);
            CHECK(!(fr.flags & MT792X_RXF_LEN_MISMATCH));
            CHECK_EQ_U32(!!(fr.flags & MT792X_RXF_TIMESTAMP), b.g2);
            if (b.g2) CHECK_EQ_U32(fr.timestamp, 0x12345678u);
            if (b.g4) CHECK_EQ_U32(fr.group4[0], 0x0080u);
            if (b.g3) {
                CHECK(fr.flags & MT792X_RXF_RATE);
                CHECK_EQ_U32(fr.rate_mode, 1);
                CHECK_EQ_U32(fr.rate_idx, 11);
                if (b.g5) {
                    CHECK(fr.flags & MT792X_RXF_GROUP5);
                    CHECK_EQ_U32((uint32_t)(int)fr.chain_rssi[0], (uint32_t)-50); /* (120 - 220) / 2 */
                    CHECK_EQ_U32((uint32_t)(int)fr.rssi_dbm, (uint32_t)-47);    /* equal chains: +3 */
                } else {
                    CHECK_EQ_U32((uint32_t)(int)fr.chain_rssi[0], (uint32_t)-60);
                    CHECK_EQ_U32((uint32_t)(int)fr.chain_rssi[1], (uint32_t)-55);
                    CHECK_EQ_U32((uint32_t)(int)fr.rssi_dbm, (uint32_t)-54);    /* -55, diff 5: +1 */
                }
            } else {
                CHECK_EQ_U32((uint32_t)(int)fr.rssi_dbm, (uint32_t)-128);
                CHECK_EQ_U32(!!(fr.flags & MT792X_RXF_G5_WITHOUT_G3), b.g5);
            }
        }
    }
}

TEST(rx_parse_channel_band_flags)
{
    uint8_t frame[128], pkt[512], got[512];
    uint32_t flen, len, glen = 0;
    struct rxb b;
    struct mt792x_rx_frame fr;
    flen = rxb_data(frame, sizeof(frame), sta, ap, ap, 7, 1, 40);
    memset(&b, 0, sizeof(b));
    b.frame = frame;
    b.frame_len = flen;
    b.ch_freq = 149;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(fr.channel, 149);
    CHECK_EQ_U32(fr.band, MT792X_BAND_5GHZ);
    b.ch_freq = 181 + 9;                               /* 6 GHz: (190 - 181) * 4 + 1 = 37 */
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(fr.channel, 37);
    CHECK_EQ_U32(fr.band, MT792X_BAND_6GHZ);
    b.ch_freq = 14;
    b.fcs_err = 1;
    b.rxd1_or = (1u << 25) | (1u << 26);               /* ICV and TKIP MIC error */
    b.rxd3_or = 1u << 16;                              /* ADDR_TYPE U2M */
    b.bad_length_field = 1;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(fr.channel, 14);
    CHECK_EQ_U32(fr.fcs_ok, 0);
    CHECK(fr.flags & MT792X_RXF_FCS_ERR);
    CHECK(fr.flags & MT792X_RXF_ICV_ERR);
    CHECK(fr.flags & MT792X_RXF_TKIP_MIC_ERR);
    CHECK(fr.flags & MT792X_RXF_UNICAST_TO_ME);
    CHECK(fr.flags & MT792X_RXF_LEN_MISMATCH);
    /* header translation: 802.3 payload, flagged, not touched */
    memset(&b, 0, sizeof(b));
    b.frame = frame;
    b.frame_len = 60;
    b.rxd2_or = 1u << 13;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, MT792X_FCS_AUTO, 3, &fr, got, &glen) == 0);
    CHECK(fr.flags & MT792X_RXF_HDR_TRANS);
    CHECK_EQ_U32(glen, 60);
    /* ... with CM set it is dropped as mt76 does */
    b.rxd1_or = 1u << 23;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_HDR_TRANS_CM);
    /* ... and a translated first fragment (more-fragments bit in the group 4 frame control) */
    b.rxd1_or = 0;
    b.g4 = 1;
    b.g4_fc = 0x0408;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_HDR_TRANS_FRAG);
}

TEST(rx_parse_rejects_like_mt76)
{
    uint8_t frame[64], pkt[512], got[512];
    uint32_t flen, len, glen = 0;
    struct rxb b;
    struct mt792x_rx_frame fr;
    flen = rxb_ack(frame, sizeof(frame), sta);
    memset(&b, 0, sizeof(b));
    b.frame = frame;
    b.frame_len = flen;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == 0);  /* a 10-byte ACK is a frame */
    CHECK_EQ_U32(glen, 10);
    CHECK(parse_copy(pkt, len - 1u, 0, 3, &fr, got, &glen) == -MT792X_RXE_FRAME_SHORT);
    CHECK(parse_copy(pkt, 23, 0, 3, &fr, got, &glen) == -MT792X_RXE_SHORT);
    b.rxd1_or = 1u << 28;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_BAND_IDX);
    b.rxd1_or = 0;
    b.rxd2_or = 1u << 23;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_AMSDU_ERR);
    b.rxd2_or = 1u << 24;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_MAX_LEN_ERR);
    /* groups announced but the packet ends inside them */
    b.rxd2_or = 0;
    b.g4 = b.g1 = b.g2 = b.g3 = b.g5 = 1;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == 0);
    CHECK(parse_copy(pkt, 24 + 16 + 16 + 8 + 8 + 72, 0, 3, &fr, got, &glen) == -MT792X_RXE_GROUP_OVERRUN);
    CHECK(parse_copy(pkt, 24 + 16, 0, 3, &fr, got, &glen) == -MT792X_RXE_GROUP_OVERRUN);
    /* padding reaching the end */
    memset(&b, 0, sizeof(b));
    b.hdr_offset = 3;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_PAD_OVERRUN);
}

TEST(rx_parse_amsdu_header_move)
{
    uint8_t frame[128], sub[130], pkt[512], got[512];
    uint32_t flen, len, glen = 0;
    struct rxb b;
    struct mt792x_rx_frame fr;
    flen = rxb_data(frame, sizeof(frame), sta, ap, ap, 3, 1, 30);  /* QoS data: 26-byte header */
    /* chip layout: 26-byte header, 2 bytes the host removes, payload */
    memcpy(sub, frame, 26);
    sub[26] = 0xee;
    sub[27] = 0xee;
    memcpy(sub + 28, frame + 26, flen - 26u);
    memset(&b, 0, sizeof(b));
    b.frame = sub;
    b.frame_len = flen + 2u;
    b.rxd4 = 3;                                        /* first A-MSDU subframe */
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == 0);
    CHECK(fr.flags & MT792X_RXF_AMSDU);
    CHECK_EQ_U32(glen, flen);
    CHECK(memcmp(got, frame, flen) == 0);
    /* A-MSDU flag on a management frame cannot be moved */
    flen = rxb_beacon(frame, sizeof(frame), ap, "x", 1);
    b.frame = frame;
    b.frame_len = flen;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 3, &fr, got, &glen) == -MT792X_RXE_AMSDU_HDR);
}

TEST(rx_parse_fcs_modes_and_rssi)
{
    uint8_t frame[128], pkt[512], got[512];
    uint32_t flen, len, glen = 0;
    struct rxb b;
    struct mt792x_rx_frame fr;
    static const uint8_t crc_check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK_EQ_U32(mt792x_crc32(crc_check, 9), 0xcbf43926u);     /* standard CRC-32 check value */
    flen = rxb_beacon(frame, sizeof(frame), ap, "fcs", 11);
    memset(&b, 0, sizeof(b));
    b.frame = frame;
    b.frame_len = flen;
    b.append_fcs = 1;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, MT792X_FCS_MT76, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(glen, flen + 4u);
    CHECK(fr.flags & MT792X_RXF_FCS_TRAILER);
    CHECK(!(fr.flags & MT792X_RXF_FCS_STRIPPED));
    CHECK(parse_copy(pkt, len, MT792X_FCS_AUTO, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(glen, flen);
    CHECK(fr.flags & MT792X_RXF_FCS_STRIPPED);
    b.append_fcs = 0;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, MT792X_FCS_AUTO, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(glen, flen);
    CHECK(!(fr.flags & (MT792X_RXF_FCS_TRAILER | MT792X_RXF_FCS_STRIPPED)));
    CHECK(parse_copy(pkt, len, MT792X_FCS_STRIP, 3, &fr, got, &glen) == 0);
    CHECK_EQ_U32(glen, flen - 4u);
    /* RSSI: unknown antenna mask -> chain 0 only, flagged; odd RCPI rounds down like mt76 */
    b.g3 = 1;
    b.rcpi[0] = 101;
    b.rcpi[1] = 0;
    len = rxb_build(&b, pkt, sizeof(pkt));
    CHECK(parse_copy(pkt, len, 0, 0, &fr, got, &glen) == 0);
    CHECK_EQ_U32((uint32_t)(int)fr.rssi_dbm, (uint32_t)-60);
    CHECK(fr.flags & MT792X_RXF_RSSI_CHAIN0);
    CHECK(parse_copy(pkt, len, 0, 1, &fr, got, &glen) == 0);
    CHECK_EQ_U32((uint32_t)(int)fr.rssi_dbm, (uint32_t)-60);
}

/* Deterministic fuzz: the same 'len' bytes followed by different tails must give identical
 * results; a parser reading past 'len' would sooner or later see the difference. */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static int same_result(int r1, const struct mt792x_rx_frame *a, const uint8_t *ba, int r2, const struct mt792x_rx_frame *b,
                       const uint8_t *bb)
{
    if (r1 != r2) return 0;
    if (r1) return 1;
    if (a->len != b->len || (a->data - ba) != (b->data - bb) || a->flags != b->flags || a->channel != b->channel ||
        a->rssi_dbm != b->rssi_dbm || a->timestamp != b->timestamp || a->hdr_gap != b->hdr_gap)
        return 0;
    return memcmp(a->data, b->data, a->len) == 0;
}

TEST(rx_parse_fuzz_never_reads_past_the_length)
{
    uint8_t frame[256], base[600], b1[700], b2[700];
    uint32_t seed = 12345u, it, flen, blen;
    unsigned mismatches = 0, accepted = 0, out_of_range = 0;
    flen = rxb_data(frame, sizeof(frame), sta, ap, ap, 1, 1, 200);
    for (it = 0; it < 30000u; it++) {
        struct rxb b;
        struct mt792x_rx_frame f1, f2;
        uint32_t len, k, nmut;
        int r1, r2;
        memset(&b, 0, sizeof(b));
        b.g1 = (int)(lcg(&seed) & 1u);
        b.g2 = (int)(lcg(&seed) & 1u);
        b.g3 = (int)(lcg(&seed) & 1u);
        b.g4 = (int)(lcg(&seed) & 1u);
        b.g5 = (int)(lcg(&seed) & 1u);
        b.hdr_offset = lcg(&seed) & 3u;
        b.rxd4 = lcg(&seed) & 3u;
        b.frame = frame;
        b.frame_len = lcg(&seed) % (flen + 1u);
        blen = rxb_build(&b, base, sizeof(base));
        if (!blen) continue;
        len = lcg(&seed) % (blen + 8u);
        if (len > blen) len = blen;
        memcpy(b1, base, blen);
        nmut = lcg(&seed) % 6u;
        for (k = 0; k < nmut; k++) b1[lcg(&seed) % (blen < 140u ? blen : 140u)] ^= (uint8_t)(1u << (lcg(&seed) & 7u));
        memcpy(b2, b1, len);
        memset(b1 + len, 0x00, sizeof(b1) - len);
        memset(b2 + len, 0xff, sizeof(b2) - len);
        r1 = mt792x_rx_parse(b1, len, (uint8_t)(it % 3u), 3, &f1);
        r2 = mt792x_rx_parse(b2, len, (uint8_t)(it % 3u), 3, &f2);
        if (!same_result(r1, &f1, b1, r2, &f2, b2)) mismatches++;
        if (r1 == 0) {
            accepted++;
            if (f1.data < b1 || f1.data + f1.len > b1 + len) out_of_range++;
        }
    }
    CHECK_EQ_U32(mismatches, 0);
    CHECK_EQ_U32(out_of_range, 0);
    CHECK(accepted > 1000u);
}

/* ---- mt792x_rx_poll against the simulated chip ---- */

static uint32_t build_pkt(uint8_t *pkt, uint32_t cap, const uint8_t *frame, uint32_t flen, uint8_t ch, int g3)
{
    struct rxb b;
    memset(&b, 0, sizeof(b));
    b.frame = frame;
    b.frame_len = flen;
    b.ch_freq = ch;
    b.g2 = 1;
    b.g3 = g3;
    b.rcpi[0] = 120;
    b.rcpi[1] = 120;
    return rxb_build(&b, pkt, cap);
}

static int poll_all(struct rig *r, struct mt792x_rx_frame *frames, uint8_t (*copies)[256], unsigned max, unsigned *n)
{
    struct mt792x_rx_frame fr;
    int rc;
    *n = 0;
    for (;;) {
        rc = mt792x_rx_poll(r->dev, &fr);
        if (rc <= 0) return rc;
        if (*n < max) {
            frames[*n] = fr;
            memcpy(copies[*n], fr.data, fr.len < 256u ? fr.len : 256u);
        }
        (*n)++;
    }
}

TEST(rx_poll_classifies_everything_on_the_rings)
{
    static struct mt792x_rx_frame frames[16];
    static uint8_t copies[16][256];
    struct rig r;
    uint8_t frame[256], pkt[600], ev[64];
    uint32_t flen, len;
    unsigned n;
    const struct mt792x_rx_counters *c;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, MT792X_SNIFF_FCSFAIL) == 0);
    c = mt792x_rx_counters(r.dev);

    flen = rxb_beacon(frame, sizeof(frame), ap, "home", 6);
    len = build_pkt(pkt, sizeof(pkt), frame, flen, 6, 1);
    CHECK(fake_inject(r.f, 2, pkt, len));                      /* 1: beacon on channel 6 */
    flen = rxb_data(frame, sizeof(frame), bc, sta, ap, 9, 0, 50);
    len = build_pkt(pkt, sizeof(pkt), frame, flen, 11, 1);
    CHECK(fake_inject(r.f, 2, pkt, len));                      /* 2: data on channel 11 */
    /* TX status: 2 header words + 2 records */
    memset(ev, 0, sizeof(ev));
    ev[0] = 72;                                                /* PKT_TYPE 0 = TXS */
    CHECK(fake_inject(r.f, 2, ev, 72));
    /* TX-free: 3 MSDUs announced, one wcid-pair entry in between */
    memset(ev, 0, sizeof(ev));
    ev[0] = 24;                                                /* rx_byte_cnt */
    ev[2] = 3;                                                 /* ctrl MSDU_CNT 3 (bits 9:0 of le16 at 2) */
    ev[3] = (uint8_t)(6u << 3);                                /* RXD0 PKT_TYPE 6 = TXRX_NOTIFY */
    ev[8 + 3] = 0x80;                                          /* entry 0: pair */
    CHECK(fake_inject(r.f, 2, ev, 24));
    /* MCU event (firmware log, unsolicited) on ring 2 and on ring 0 */
    memset(ev, 0, sizeof(ev));
    ev[0] = 56;
    ev[3] = 7 << 3;
    ev[0x1c] = 0x27;
    ev[36 + 2] = 3;
    ev[36 + 8] = 4;
    memcpy(ev + 46, "boot", 4);
    CHECK(fake_inject(r.f, 2, ev, 56));
    CHECK(fake_inject(r.f, 0, ev, 56));
    CHECK(fake_inject(r.f, 4, ev, 56));
    /* unknown packet type 0x0c */
    memset(ev, 0, sizeof(ev));
    ev[3] = 0x0c << 3;
    CHECK(fake_inject(r.f, 2, ev, 16));
    /* fragmented packet: 3 descriptors, dropped as a whole */
    flen = rxb_data(frame, sizeof(frame), bc, sta, ap, 10, 0, 100);
    len = build_pkt(pkt, sizeof(pkt), frame, flen, 6, 0);
    CHECK(fake_inject_frag(r.f, 2, pkt, len, 64));
    /* descriptor with an impossible length */
    CHECK(fake_inject_badlen(r.f, 2, 3000));
    CHECK(fake_inject_badlen(r.f, 2, 0));
    /* malformed frame (BAND_IDX) */
    {
        struct rxb b;
        memset(&b, 0, sizeof(b));
        b.frame = frame;
        b.frame_len = flen;
        b.rxd1_or = 1u << 28;
        len = rxb_build(&b, pkt, sizeof(pkt));
        CHECK(fake_inject(r.f, 2, pkt, len));
    }
    /* 3: FCS-error frame (delivered: FCSFAIL requested) */
    flen = rxb_awdl_action(frame, sizeof(frame), sta);
    {
        struct rxb b;
        memset(&b, 0, sizeof(b));
        b.frame = frame;
        b.frame_len = flen;
        b.ch_freq = 6;
        b.fcs_err = 1;
        len = rxb_build(&b, pkt, sizeof(pkt));
        CHECK(fake_inject(r.f, 2, pkt, len));
    }
    /* 4: type 7 with flag 1 is a frame (PKT_TYPE_NORMAL_MCU) */
    {
        struct rxb b;
        memset(&b, 0, sizeof(b));
        b.pkt_type = 7;
        b.pkt_flag = 1;
        b.frame = frame;
        b.frame_len = flen;
        b.ch_freq = 6;
        len = rxb_build(&b, pkt, sizeof(pkt));
        CHECK(fake_inject(r.f, 2, pkt, len));
    }

    CHECK(poll_all(&r, frames, copies, 16, &n) == 0);
    CHECK_EQ_U32(n, 4);
    CHECK_EQ_U32(frames[0].channel, 6);
    CHECK(copies[0][0] == 0x80);
    CHECK_EQ_U32(frames[0].ring, 2);
    CHECK_EQ_U32((uint32_t)(int)frames[0].rssi_dbm, (uint32_t)-47);
    CHECK_EQ_U32(frames[1].channel, 11);
    CHECK(copies[1][0] == 0x08);
    CHECK_EQ_U32(frames[2].fcs_ok, 0);
    CHECK(copies[2][24] == 127);
    CHECK_EQ_U32(frames[3].fcs_ok, 1);
    CHECK_EQ_U32(c->frames, 4);
    CHECK_EQ_U32(c->fcs_err, 1);
    CHECK_EQ_U32(c->txs_records, 2);
    CHECK_EQ_U32(c->tx_free_msdus, 3);
    CHECK_EQ_U32(c->frag_packets, 1);
    CHECK_EQ_U32(c->frag_descs, 3);
    CHECK_EQ_U32(c->bad_desc_len, 2);
    CHECK_EQ_U32(c->malformed, 1);
    CHECK_EQ_U32(c->malformed_by[MT792X_RXE_BAND_IDX], 1);
    CHECK(c->event_by_id[0x27] >= 3u);
    CHECK_EQ_U32(c->other_pkts, 1);
    CHECK_EQ_U32(c->pkt_type[0], 1);
    CHECK_EQ_U32(c->pkt_type[6], 1);
    CHECK(c->liveness_checks >= 1u);
    CHECK(rig_check_no_violations(&r) == 0);
    mt792x_shutdown(r.dev);
    rig_free(&r);
}

TEST(rx_poll_bursts_wrap_the_ring_in_order)
{
    struct rig r;
    struct mt792x_rx_frame fr;
    uint8_t frame[128], pkt[400];
    uint32_t flen, len, seq = 0, got = 0, writes_before;
    unsigned burst, i;
    int rc;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    writes_before = mock_count_writes(&r.m, 0xd4528u);
    for (burst = 0; burst < 8; burst++) {
        for (i = 0; i < 100; i++, seq++) {
            flen = rxb_data(frame, sizeof(frame), bc, sta, ap, (uint16_t)seq, 0, 20);
            len = build_pkt(pkt, sizeof(pkt), frame, flen, 6, 1);
            CHECK(fake_inject(r.f, 2, pkt, len));
        }
        while ((rc = mt792x_rx_poll(r.dev, &fr)) == 1) {
            uint16_t s = (uint16_t)((fr.data[22] >> 4) | (fr.data[23] << 4));
            if (s != (uint16_t)(got & 0xfffu)) {
                printf("  frame %u carries sequence %u\n", got, s);
                CHECK(0);
                break;
            }
            got++;
        }
        CHECK(rc == 0);
    }
    CHECK_EQ_U32(got, 800);
    /* cpu_idx is written in batches, not once per frame */
    CHECK(mock_count_writes(&r.m, 0xd4528u) - writes_before < 800u / 16u);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(rx_poll_recovers_from_a_full_ring)
{
    struct rig r;
    struct mt792x_rx_frame fr;
    uint8_t frame[128], pkt[400];
    uint32_t flen, len, i, delivered = 0, got = 0;
    int rc;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    flen = rxb_beacon(frame, sizeof(frame), ap, "full", 6);
    len = build_pkt(pkt, sizeof(pkt), frame, flen, 6, 1);
    for (i = 0; i < 300; i++) delivered += (uint32_t)fake_inject(r.f, 2, pkt, len);
    CHECK_EQ_U32(delivered, MT792X_RX2_NDESC - 1u);             /* the chip had nowhere to put the rest */
    CHECK_EQ_U32(r.f->rx_inject_dropped[2], 300u - delivered);
    while ((rc = mt792x_rx_poll(r.dev, &fr)) == 1) got++;
    CHECK(rc == 0);
    CHECK_EQ_U32(got, delivered);
    CHECK(mt792x_rx_counters(r.dev)->rx2_full >= 1u);
    for (i = 0; i < 10; i++) CHECK(fake_inject(r.f, 2, pkt, len));
    got = 0;
    while (mt792x_rx_poll(r.dev, &fr) == 1) got++;
    CHECK_EQ_U32(got, 10);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(rx_frames_during_a_command_are_drained_and_counted)
{
    struct rig r;
    struct mt792x_rx_frame fr;
    uint8_t frame[128], pkt[400];
    uint32_t flen, len, i;
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    flen = rxb_beacon(frame, sizeof(frame), ap, "x", 6);
    len = build_pkt(pkt, sizeof(pkt), frame, flen, 6, 0);
    for (i = 0; i < 20; i++) CHECK(fake_inject(r.f, 2, pkt, len));
    CHECK(mt792x_sniffer_set_channel(r.dev, 11, MT792X_BAND_2GHZ, NULL) == 0);
    CHECK_EQ_U32(mt792x_rx_counters(r.dev)->discarded_in_cmd, 20);
    CHECK(mt792x_rx_poll(r.dev, &fr) == 0);
    CHECK(rig_check_no_violations(&r) == 0);
    rig_free(&r);
}

TEST(rx_poll_liveness_and_bus_dead)
{
    struct rig r;
    struct mt792x_rx_frame fr;
    uint32_t sd;
    /* firmware dies */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    CHECK(mt792x_rx_poll(r.dev, &fr) == 0);
    r.f->n9_never = 1;
    CHECK(mt792x_rx_poll(r.dev, &fr) == 0);                     /* within the 250 ms period: not checked */
    mock_advance(&r.m, 300000u);
    CHECK(mt792x_rx_poll(r.dev, &fr) == MT792X_E_FW_DEAD);
    mt792x_shutdown(r.dev);
    rig_free(&r);
    /* WFDMA lost its configuration */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    mock_set_reg(&r.m, 0x02120u, 0);
    CHECK(mt792x_rx_poll(r.dev, &fr) == MT792X_E_DMA_REINIT);
    rig_free(&r);
    /* bus dies while sniffing */
    CHECK(rig_init(&r, 0) == 0);
    CHECK(rig_sniff(&r, 6, 0) == 0);
    r.f->bus_dead_after = r.f->accesses + 1u;
    mock_advance(&r.m, 300000u);
    CHECK(mt792x_rx_poll(r.dev, &fr) == MT792X_E_BUS_DEAD);
    CHECK(mt792x_rx_poll(r.dev, &fr) == MT792X_E_BUS_DEAD);
    CHECK(mt792x_sniffer_set_channel(r.dev, 1, 0, NULL) == MT792X_E_BUS_DEAD);
    CHECK_EQ_U32(r.f->writes_after_dead_read, 0);
    sd = r.m.trace_len;
    mt792x_shutdown(r.dev);
    CHECK(r.dev->shutdown_error != 0);
    CHECK(r.m.trace_len - sd < 200u);                         /* no MCU commands on a dead bus */
    CHECK(r.f->violations == 0);
    rig_free(&r);
}
