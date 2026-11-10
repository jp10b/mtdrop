/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rx_builder.c - see rx_builder.h. Bit positions are written out independently of src/regs.h. */
#include "rx_builder.h"

#include <string.h>

#include "mt792x/mt792x.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t rxb_build(const struct rxb *b, uint8_t *out, uint32_t cap)
{
    uint32_t off = 24, rxd1 = b->rxd1_or, rxd2 = b->rxd2_or, rxd3 = b->rxd3_or, type, total, i;
    uint32_t fl = b->frame_len + (b->append_fcs ? 4u : 0u);

    total = 24u + (b->g4 ? 16u : 0u) + (b->g1 ? 16u : 0u) + (b->g2 ? 8u : 0u) + (b->g3 ? 8u : 0u) +
            (b->g3 && b->g5 ? 72u : 0u) + 2u * b->hdr_offset + fl;
    if (total > cap) return 0;
    memset(out, 0, total);
    if (b->g1) rxd1 |= 1u << 11;
    if (b->g2) rxd1 |= 1u << 12;
    if (b->g3) rxd1 |= 1u << 13;
    if (b->g4) rxd1 |= 1u << 14;
    if (b->g5) rxd1 |= 1u << 15;
    if (b->fcs_err) rxd1 |= 1u << 27;
    rxd2 |= (b->hdr_offset & 3u) << 14;
    rxd3 |= (uint32_t)b->ch_freq << 8;
    type = (b->pkt_type || b->raw_type) ? b->pkt_type : 2u;
    put32(out, ((b->bad_length_field ? total + 4u : total) & 0xffffu) | ((b->pkt_flag & 0xfu) << 16) | (type << 27));
    put32(out + 4, rxd1);
    put32(out + 8, rxd2);
    put32(out + 12, rxd3);
    put32(out + 16, b->rxd4);
    if (b->g4) {
        put32(out + off, b->g4_fc);
        off += 16;
    }
    if (b->g1) off += 16;
    if (b->g2) {
        put32(out + off, b->timestamp);
        off += 8;
    }
    if (b->g3) {
        put32(out + off, b->prxv0);
        for (i = 0; i < 4; i++) out[off + 4 + i] = b->rcpi[i];
        off += 8;
        if (b->g5) {
            for (i = 0; i < 4; i++) out[off + 24 + i] = b->g5_rcpi[i];
            off += 72;
        }
    }
    off += 2u * b->hdr_offset;
    if (b->frame_len) memcpy(out + off, b->frame, b->frame_len);
    if (b->append_fcs) put32(out + off + b->frame_len, mt792x_crc32(b->frame, b->frame_len));
    return total;
}

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

uint32_t rxb_beacon(uint8_t *f, uint32_t cap, const uint8_t bssid[6], const char *ssid, uint8_t ds_channel)
{
    uint32_t n = (uint32_t)strlen(ssid), len = 24 + 12 + 2 + n + 3;
    if (len > cap) return 0;
    memset(f, 0, len);
    f[0] = 0x80; /* management, subtype 8 = beacon */
    memcpy(f + 4, bcast, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[24 + 8] = 0x64; /* beacon interval 100 TU */
    f[24 + 10] = 0x01;
    f[36] = 0;        /* SSID element */
    f[37] = (uint8_t)n;
    memcpy(f + 38, ssid, n);
    f[38 + n] = 3;    /* DS parameter set */
    f[39 + n] = 1;
    f[40 + n] = ds_channel;
    return len;
}

uint32_t rxb_data(uint8_t *f, uint32_t cap, const uint8_t a1[6], const uint8_t a2[6], const uint8_t a3[6], uint16_t seq,
                  int qos, uint32_t payload_len)
{
    uint32_t hl = qos ? 26u : 24u, len = hl + payload_len, i;
    if (len > cap) return 0;
    memset(f, 0, len);
    f[0] = qos ? 0x88 : 0x08; /* data / QoS data */
    f[1] = 0x02;              /* FromDS */
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    f[22] = (uint8_t)(seq << 4);
    f[23] = (uint8_t)(seq >> 4);
    for (i = 0; i < payload_len; i++) f[hl + i] = (uint8_t)(i * 7u + seq);
    return len;
}

uint32_t rxb_awdl_action(uint8_t *f, uint32_t cap, const uint8_t ta[6])
{
    static const uint8_t awdl_bssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
    uint32_t len = 24 + 1 + 3 + 1 + 1 + 1 + 1 + 8;
    if (len > cap) return 0;
    memset(f, 0, len);
    f[0] = 0xd0;  /* management, subtype 13 = action */
    memcpy(f + 4, bcast, 6);
    memcpy(f + 10, ta, 6);
    memcpy(f + 16, awdl_bssid, 6);
    f[24] = 127;  /* vendor specific */
    f[25] = 0x00;
    f[26] = 0x17;
    f[27] = 0xf2;
    f[28] = 0x08; /* AWDL type */
    f[29] = 0x10; /* version */
    f[30] = 0x00; /* subtype PSF */
    return len;
}

uint32_t rxb_ack(uint8_t *f, uint32_t cap, const uint8_t ra[6])
{
    if (cap < 10) return 0;
    memset(f, 0, 10);
    f[0] = 0xd4; /* control, subtype 13 = ACK */
    memcpy(f + 4, ra, 6);
    return 10;
}
