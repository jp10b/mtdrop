/* SPDX-License-Identifier: MIT */
/* Unit tests of radio_link.c. Builds with MSVC (CMake target radio_link_test) and with GCC:
 *   gcc -std=c11 -Wall -Wextra -Werror radio_link_test.c radio_link.c -o radio_link_test */
#include <stdio.h>
#include <string.h>

#include "radio_link.h"

static int failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_header(void) {
    uint8_t m[RL_HEADER_SIZE + 3];
    struct rl_header h;
    CHECK(rl_put_header(m, sizeof(m), RL_TX, 3) == RL_HEADER_SIZE);
    CHECK(m[0] == 3 && m[1] == 0 && m[4] == 3 && m[5] == 0);
    CHECK(rl_get_header(m, sizeof(m), &h) == 0 && h.type == RL_TX && h.length == 3);
    CHECK(rl_get_header(m, sizeof(m) - 1, &h) == -1);          /* length mismatch */
    CHECK(rl_get_header(m, 4, &h) == -1);                      /* short */
    m[0] = 0;
    CHECK(rl_get_header(m, sizeof(m), &h) == -1);              /* type 0 */
    m[0] = RL_BYE + 1;
    CHECK(rl_get_header(m, sizeof(m), &h) == -1);              /* unknown type */
    CHECK(rl_put_header(m, sizeof(m), RL_RX, RL_MAX_PAYLOAD + 1) == 0);
    CHECK(rl_put_header(m, 7, RL_RX, 0) == 0);
}

static void test_structs(void) {
    uint8_t b[64];
    struct rl_hello h = {RL_VERSION, 44, 1, 20, 1, {0x02, 1, 2, 3, 4, 5}, 4032, RL_LEASE_MS}, h2;
    struct rl_tx_status s = {0xdeadbeef, RL_TXR_DONE, RL_ACK_NO, 15, 44, 12345, -46}, s2;
    struct rl_channel c = {6, 1, 1, 0, 10100}, c2;
    struct rl_stats st, st2;
    CHECK(rl_put_hello(b, sizeof(b), &h) == RL_HELLO_SIZE);
    CHECK(rl_get_hello(b, RL_HELLO_SIZE, &h2) == 0 && memcmp(&h, &h2, sizeof(h)) == 0);
    CHECK(rl_get_hello(b, RL_HELLO_SIZE - 1, &h2) == -1);
    CHECK(rl_put_tx_status(b, sizeof(b), &s) == RL_TX_STATUS_SIZE);
    CHECK(b[0] == 0xef && b[3] == 0xde && b[6] == 15);
    CHECK(rl_get_tx_status(b, RL_TX_STATUS_SIZE, &s2) == 0 && s2.cookie == s.cookie && s2.error == -46 &&
          s2.elapsed_us == 12345 && s2.acked == RL_ACK_NO);
    CHECK(rl_put_channel(b, sizeof(b), &c) == RL_CHANNEL_SIZE);
    CHECK(rl_get_channel(b, RL_CHANNEL_SIZE, &c2) == 0 && c2.channel == 6 && c2.ok == 1 && c2.elapsed_us == 10100);
    for (unsigned i = 0; i < RL_STATS_COUNT; i++) st.v[i] = i * 1000u + 7u;
    CHECK(rl_put_stats(b, sizeof(b), &st) == RL_STATS_SIZE);
    CHECK(rl_get_stats(b, RL_STATS_SIZE, &st2) == 0 && memcmp(&st, &st2, sizeof(st)) == 0);
    CHECK(rl_put_stats(b, RL_STATS_SIZE - 1, &st) == 0);
}

static void test_radiotap(void) {
    uint8_t rt[RL_RADIOTAP_MAX];
    struct rl_rx_meta m;
    size_t n;
    /* OFDM 6 Mb/s on channel 44, -55 dBm: header 8 + TSFT 8 + FLAGS 1 + RATE 1 + CHANNEL 4 + SIGNAL 1 = 23 */
    memset(&m, 0, sizeof(m));
    m.tsft_us = 0x0102030405060708ull;
    m.channel = 44;
    m.band = 1;
    m.has_signal = 1;
    m.signal_dbm = -55;
    m.rate_500k = 12;
    n = rl_radiotap_build(rt, sizeof(rt), &m);
    CHECK(n == 23);
    CHECK(rt[0] == 0 && rt[2] == 23 && rt[3] == 0);
    CHECK(rt[4] == 0x2f && rt[5] == 0 && rt[6] == 0 && rt[7] == 0); /* TSFT FLAGS RATE CHANNEL DBM_ANTSIGNAL */
    CHECK(rt[8] == 0x08 && rt[15] == 0x01);                         /* TSFT little-endian */
    CHECK(rt[16] == 0 && rt[17] == 12);                            /* FLAGS, RATE */
    CHECK(rt[18] == (5220 & 0xff) && rt[19] == (5220 >> 8));       /* 5220 MHz, aligned to 2 */
    CHECK(rt[20] == 0x40 && rt[21] == 0x01);                       /* 5 GHz | OFDM */
    CHECK((int8_t)rt[22] == -55);
    /* HT on channel 6, bad FCS, no signal: 8 + 8 + 1 + (pad 1) + 4 + MCS 3 = 25 */
    memset(&m, 0, sizeof(m));
    m.channel = 6;
    m.fcs_bad = 1;
    m.has_mcs = 1;
    m.mcs_index = 7;
    n = rl_radiotap_build(rt, sizeof(rt), &m);
    CHECK(n == 25);
    CHECK(rt[4] == 0x0b && rt[6] == 0x08);                         /* TSFT FLAGS CHANNEL, MCS (bit 19) */
    CHECK(rt[16] == 0x40);                                         /* BADFCS */
    CHECK(rt[18] == (2437 & 0xff) && rt[19] == (2437 >> 8));
    CHECK(rt[20] == 0xc0 && rt[21] == 0x00);                       /* 2 GHz | OFDM */
    CHECK(rt[22] == 0x03 && rt[23] == 0 && rt[24] == 7);
    CHECK(rl_radiotap_build(rt, 10, &m) == 0);
    CHECK(rl_channel_freq(1, 0) == 2412 && rl_channel_freq(14, 0) == 2484 && rl_channel_freq(149, 1) == 5745);
    CHECK(rl_channel_freq(0, 0) == 0 && rl_channel_freq(20, 0) == 0);
}

static void test_tx(void) {
    uint8_t frame[30], pl[128];
    const uint8_t *f;
    size_t flen, n;
    uint32_t cookie;
    uint8_t rate;
    for (unsigned i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)i;
    n = rl_build_tx(pl, sizeof(pl), 77, frame, sizeof(frame), 24);
    CHECK(n == 4 + 9 + sizeof(frame));
    CHECK(rl_parse_tx(pl, n, &cookie, &f, &flen, &rate) == 0 && cookie == 77 && rate == 24 && flen == sizeof(frame) &&
          memcmp(f, frame, flen) == 0);
    n = rl_build_tx(pl, sizeof(pl), 5, frame, sizeof(frame), 0);
    CHECK(n == 4 + 8 + sizeof(frame));
    CHECK(rl_parse_tx(pl, n, &cookie, &f, &flen, &rate) == 0 && rate == 0 && flen == sizeof(frame));
    /* OWL's TX radiotap (src/tx.c ieee80211_init_radiotap_header): RATE only, it_len 9 */
    {
        uint8_t owl[4 + 9 + 24];
        memset(owl, 0, sizeof(owl));
        owl[6] = 9;
        owl[8] = 0x04;
        owl[12] = 24; /* 12 Mb/s */
        CHECK(rl_parse_tx(owl, sizeof(owl), &cookie, &f, &flen, &rate) == 0 && rate == 24 && flen == 24 && f == owl + 13);
    }
    /* TSFT + FLAGS + RATE: rate found after the aligned TSFT */
    {
        uint8_t t[4 + 18 + 24];
        memset(t, 0, sizeof(t));
        t[6] = 18;
        t[8] = 0x07;
        t[4 + 17] = 108;
        CHECK(rl_parse_tx(t, sizeof(t), &cookie, &f, &flen, &rate) == 0 && rate == 108 && flen == 24);
    }
    /* malformed */
    n = rl_build_tx(pl, sizeof(pl), 1, frame, 24, 0);
    CHECK(rl_parse_tx(pl, n - 1, &cookie, &f, &flen, &rate) == -1); /* frame 23 bytes */
    pl[4] = 1;
    CHECK(rl_parse_tx(pl, n, &cookie, &f, &flen, &rate) == -1);     /* radiotap version */
    pl[4] = 0;
    pl[6] = 200;
    CHECK(rl_parse_tx(pl, n, &cookie, &f, &flen, &rate) == -1);     /* it_len past the end */
    pl[6] = 4;
    CHECK(rl_parse_tx(pl, n, &cookie, &f, &flen, &rate) == -1);     /* it_len < 8 */
    CHECK(rl_parse_tx(pl, 11, &cookie, &f, &flen, &rate) == -1);
    CHECK(rl_build_tx(pl, 20, 1, frame, sizeof(frame), 0) == 0);    /* does not fit */
}

int main(void) {
    test_header();
    test_structs();
    test_radiotap();
    test_tx();
    if (failures) {
        printf("radio_link_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("radio_link_test: all passed\n");
    return 0;
}
