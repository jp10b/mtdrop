/* SPDX-License-Identifier: MIT */
/* Radio link encoding and decoding (radio_link.h). No OS headers. */
#include "radio_link.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

size_t rl_put_header(uint8_t *out, size_t cap, uint16_t type, uint32_t length) {
    if (cap < RL_HEADER_SIZE || length > RL_MAX_PAYLOAD) return 0;
    put16(out, type);
    put16(out + 2, 0);
    put32(out + 4, length);
    return RL_HEADER_SIZE;
}

size_t rl_put_hello(uint8_t *out, size_t cap, const struct rl_hello *h) {
    if (cap < RL_HELLO_SIZE) return 0;
    put32(out, h->version);
    out[4] = h->channel;
    out[5] = h->band;
    out[6] = h->width_mhz;
    out[7] = h->tx_allowed;
    memcpy(out + 8, h->addr, 6);
    put16(out + 14, h->max_frame);
    put32(out + 16, h->lease_ms);
    return RL_HELLO_SIZE;
}

size_t rl_put_tx_status(uint8_t *out, size_t cap, const struct rl_tx_status *s) {
    if (cap < RL_TX_STATUS_SIZE) return 0;
    put32(out, s->cookie);
    out[4] = s->result;
    out[5] = s->acked;
    out[6] = s->tx_count;
    out[7] = s->channel;
    put32(out + 8, s->elapsed_us);
    put32(out + 12, (uint32_t)s->error);
    return RL_TX_STATUS_SIZE;
}

size_t rl_put_channel(uint8_t *out, size_t cap, const struct rl_channel *c) {
    if (cap < RL_CHANNEL_SIZE) return 0;
    out[0] = c->channel;
    out[1] = c->ok;
    out[2] = c->tx_allowed;
    out[3] = 0;
    put32(out + 4, c->elapsed_us);
    return RL_CHANNEL_SIZE;
}

size_t rl_put_stats(uint8_t *out, size_t cap, const struct rl_stats *s) {
    if (cap < RL_STATS_SIZE) return 0;
    for (unsigned i = 0; i < RL_STATS_COUNT; i++) put32(out + 4u * i, s->v[i]);
    return RL_STATS_SIZE;
}

int rl_get_header(const uint8_t *msg, size_t len, struct rl_header *h) {
    if (!msg || len < RL_HEADER_SIZE) return -1;
    h->type = get16(msg);
    h->flags = get16(msg + 2);
    h->length = get32(msg + 4);
    if (h->type < RL_HELLO || h->type > RL_BYE) return -1;
    if (h->length > RL_MAX_PAYLOAD || (size_t)h->length != len - RL_HEADER_SIZE) return -1;
    return 0;
}

int rl_get_hello(const uint8_t *p, size_t len, struct rl_hello *h) {
    if (len != RL_HELLO_SIZE) return -1;
    h->version = get32(p);
    h->channel = p[4];
    h->band = p[5];
    h->width_mhz = p[6];
    h->tx_allowed = p[7];
    memcpy(h->addr, p + 8, 6);
    h->max_frame = get16(p + 14);
    h->lease_ms = get32(p + 16);
    return 0;
}

int rl_get_tx_status(const uint8_t *p, size_t len, struct rl_tx_status *s) {
    if (len != RL_TX_STATUS_SIZE) return -1;
    s->cookie = get32(p);
    s->result = p[4];
    s->acked = p[5];
    s->tx_count = p[6];
    s->channel = p[7];
    s->elapsed_us = get32(p + 8);
    s->error = (int32_t)get32(p + 12);
    return 0;
}

int rl_get_channel(const uint8_t *p, size_t len, struct rl_channel *c) {
    if (len != RL_CHANNEL_SIZE) return -1;
    c->channel = p[0];
    c->ok = p[1];
    c->tx_allowed = p[2];
    c->reserved = p[3];
    c->elapsed_us = get32(p + 4);
    return 0;
}

int rl_get_stats(const uint8_t *p, size_t len, struct rl_stats *s) {
    if (len != RL_STATS_SIZE) return -1;
    for (unsigned i = 0; i < RL_STATS_COUNT; i++) s->v[i] = get32(p + 4u * i);
    return 0;
}

/* ---- radiotap (https://www.radiotap.org) ---- */

#define RT_TSFT 0
#define RT_FLAGS 1
#define RT_RATE 2
#define RT_CHANNEL 3
#define RT_DBM_ANTSIGNAL 5
#define RT_MCS 19
#define RT_EXT 31

#define RT_F_BADFCS 0x40u
#define RT_CHAN_CCK 0x0020u
#define RT_CHAN_OFDM 0x0040u
#define RT_CHAN_2GHZ 0x0080u
#define RT_CHAN_5GHZ 0x0100u
#define RT_MCS_HAVE_BW 0x01u
#define RT_MCS_HAVE_MCS 0x02u
#define RT_MCS_BW_40 1u

uint16_t rl_channel_freq(uint8_t channel, uint8_t band) {
    if (band == 0) {
        if (channel >= 1 && channel <= 13) return (uint16_t)(2407u + 5u * channel);
        if (channel == 14) return 2484;
        return 0;
    }
    if (channel >= 32 && channel <= 177) return (uint16_t)(5000u + 5u * channel);
    return 0;
}

static int is_cck(uint8_t rate_500k) {
    return rate_500k == 2 || rate_500k == 4 || rate_500k == 11 || rate_500k == 22;
}

size_t rl_radiotap_build(uint8_t *out, size_t cap, const struct rl_rx_meta *m) {
    uint8_t buf[RL_RADIOTAP_MAX];
    uint32_t present = 0;
    size_t off = 8;
    uint16_t chan_flags;

    memset(buf, 0, sizeof(buf));
    /* TSFT: u64, aligned to 8 (the 8-byte header keeps it aligned) */
    present |= 1u << RT_TSFT;
    put64(buf + off, m->tsft_us);
    off += 8;
    present |= 1u << RT_FLAGS;
    buf[off++] = m->fcs_bad ? (uint8_t)RT_F_BADFCS : 0u;
    if (m->rate_500k) {
        present |= 1u << RT_RATE;
        buf[off++] = m->rate_500k;
    }
    /* CHANNEL: u16 frequency, u16 flags, aligned to 2 */
    off = (off + 1u) & ~(size_t)1u;
    present |= 1u << RT_CHANNEL;
    chan_flags = m->band ? (uint16_t)(RT_CHAN_5GHZ | RT_CHAN_OFDM)
                         : (uint16_t)(RT_CHAN_2GHZ | (is_cck(m->rate_500k) ? RT_CHAN_CCK : RT_CHAN_OFDM));
    put16(buf + off, rl_channel_freq(m->channel, m->band));
    put16(buf + off + 2, chan_flags);
    off += 4;
    if (m->has_signal) {
        present |= 1u << RT_DBM_ANTSIGNAL;
        buf[off++] = (uint8_t)m->signal_dbm;
    }
    if (m->has_mcs) {
        present |= 1u << RT_MCS;
        buf[off++] = RT_MCS_HAVE_BW | RT_MCS_HAVE_MCS;
        buf[off++] = m->mcs_bw40 ? (uint8_t)RT_MCS_BW_40 : 0u;
        buf[off++] = m->mcs_index;
    }
    buf[0] = 0; /* version */
    buf[1] = 0;
    put16(buf + 2, (uint16_t)off);
    put32(buf + 4, present);
    if (cap < off) return 0;
    memcpy(out, buf, off);
    return off;
}

int rl_parse_tx(const uint8_t *p, size_t len, uint32_t *cookie, const uint8_t **frame, size_t *frame_len,
                uint8_t *rate_500k) {
    const uint8_t *rt;
    size_t rt_len, off = 8;
    uint32_t present;
    if (!p || len < 4u + 8u) return -1;
    *cookie = get32(p);
    rt = p + 4;
    if (rt[0] != 0) return -1; /* radiotap version */
    rt_len = get16(rt + 2);
    if (rt_len < 8u || 4u + rt_len > len) return -1;
    present = get32(rt + 4);
    *rate_500k = 0;
    if (!(present & (1u << RT_EXT))) {
        if (present & (1u << RT_TSFT)) off = ((off + 7u) & ~(size_t)7u) + 8u;
        if (present & (1u << RT_FLAGS)) off += 1u;
        if ((present & (1u << RT_RATE)) && off < rt_len) *rate_500k = rt[off];
    }
    *frame = rt + rt_len;
    *frame_len = len - 4u - rt_len;
    if (*frame_len < 24u) return -1;
    return 0;
}

size_t rl_build_tx(uint8_t *out, size_t cap, uint32_t cookie, const uint8_t *frame, size_t frame_len,
                   uint8_t rate_500k) {
    size_t rt_len = rate_500k ? 9u : 8u, n = 4u + rt_len + frame_len;
    if (cap < n || n > RL_MAX_PAYLOAD) return 0;
    put32(out, cookie);
    out[4] = 0;
    out[5] = 0;
    put16(out + 6, (uint16_t)rt_len);
    put32(out + 8, rate_500k ? 1u << RT_RATE : 0u);
    if (rate_500k) out[12] = rate_500k;
    memcpy(out + 4 + rt_len, frame, frame_len);
    return n;
}
