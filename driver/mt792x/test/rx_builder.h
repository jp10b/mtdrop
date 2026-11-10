/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rx_builder.h - synthetic RX packets as the MT7921 writes them into an RX buffer: base RXD,
 * optional groups 4/1/2/3/5, header padding, then the 802.11 frame (spec 9.5 / 11.7). Plus a few
 * 802.11 frames (beacon, data, AWDL action). Test code only.
 */
#ifndef MT792X_TEST_RX_BUILDER_H
#define MT792X_TEST_RX_BUILDER_H

#include <stddef.h>
#include <stdint.h>

struct rxb {
    uint32_t pkt_type;      /* RXD0 PKT_TYPE, default 2 (NORMAL) when 0 and !raw_type */
    int raw_type;           /* use pkt_type as given even if 0 */
    uint32_t pkt_flag;      /* RXD0 PKT_FLAG */
    uint8_t ch_freq;        /* RXD3 CH_FREQ field */
    int g1, g2, g3, g4, g5; /* groups to include */
    uint32_t timestamp;     /* group 2 word 0 */
    uint32_t prxv0;         /* group 3 word 0 */
    uint8_t rcpi[4];        /* group 3 word 1 */
    uint8_t g5_rcpi[4];     /* group 5 word 6 */
    uint16_t g4_fc;         /* group 4 frame control */
    unsigned hdr_offset;    /* RXD2 HDR_OFFSET (pad = 2 * value) */
    uint32_t rxd1_or, rxd2_or, rxd3_or, rxd4; /* extra bits */
    int fcs_err;
    int append_fcs;         /* append the CRC-32 of the frame (as a chip that keeps the FCS would) */
    int bad_length_field;   /* RXD0 LENGTH deliberately wrong */
    const uint8_t *frame;
    uint32_t frame_len;
};

/* Returns the total length written (0 if it does not fit). */
uint32_t rxb_build(const struct rxb *b, uint8_t *out, uint32_t cap);

/* 802.11 frames; return their length. */
uint32_t rxb_beacon(uint8_t *f, uint32_t cap, const uint8_t bssid[6], const char *ssid, uint8_t ds_channel);
uint32_t rxb_data(uint8_t *f, uint32_t cap, const uint8_t a1[6], const uint8_t a2[6], const uint8_t a3[6], uint16_t seq,
                  int qos, uint32_t payload_len);
/* AWDL action frame: category 127, OUI 00:17:f2, address 3 = 00:25:00:ff:94:73 */
uint32_t rxb_awdl_action(uint8_t *f, uint32_t cap, const uint8_t ta[6]);
/* 10-byte ACK */
uint32_t rxb_ack(uint8_t *f, uint32_t cap, const uint8_t ra[6]);

#endif /* MT792X_TEST_RX_BUILDER_H */
