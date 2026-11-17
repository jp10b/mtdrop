/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radioctl: test client of `radiod serve` (docs/adr/0003-awdld-on-windows.md, step M5.1).
 *
 *   radioctl [--seconds N] [--channel C] [--tx-test] [--connect-timeout S] [--no-filter]
 *
 * Connects to RL_PIPE_NAME (retrying until --connect-timeout, default 60 s, so it can be started before the
 * server), prints the HELLO, optionally switches channel, then listens for N seconds (default 30) and counts
 * AWDL frames: action frames by subtype (PSF, MIF, ...) and data frames, transmitters (OUI only), RSSI and the
 * per-frame channel from the radiotap CHANNEL field. --tx-test sends, one per second, 3 broadcast probe
 * requests (no ACK expected) and 3 probe requests to a random locally administered address nobody owns (ACK
 * requested, never given: NOT ACKed after all retries) and checks the TX status of each. Sends a heartbeat
 * every second and asks for the server's counters at the end. Disconnecting ends the server's session.
 *
 * Privacy: addresses are printed as OUI:xx:xx:xx.
 */
#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio_link.h"

#define MAX_TA 64

static HANDLE pipe_h = INVALID_HANDLE_VALUE;
static OVERLAPPED rov, wov;
static int reading;
static uint8_t rbuf[RL_MAX_MESSAGE];

static uint64_t now_ms(void) {
    return GetTickCount64();
}

static int connect_pipe(unsigned timeout_s) {
    uint64_t until = now_ms() + (uint64_t)timeout_s * 1000u;
    for (;;) {
        pipe_h = CreateFileA(RL_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                             NULL);
        if (pipe_h != INVALID_HANDLE_VALUE) break;
        if (now_ms() >= until) return -1;
        Sleep(250);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe_h, &mode, NULL, NULL)) return -1;
    rov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    wov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    return rov.hEvent && wov.hEvent ? 0 : -1;
}

static int send_msg(uint16_t type, const uint8_t *p, size_t len) {
    uint8_t m[RL_MAX_MESSAGE];
    DWORD n = 0;
    if (!rl_put_header(m, sizeof(m), type, (uint32_t)len)) return -1;
    if (len) memcpy(m + RL_HEADER_SIZE, p, len);
    ResetEvent(wov.hEvent);
    if (!WriteFile(pipe_h, m, (DWORD)(RL_HEADER_SIZE + len), &n, &wov)) {
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        if (!GetOverlappedResult(pipe_h, &wov, &n, TRUE)) return -1;
    }
    return 0;
}

/* Waits up to 'ms' for one message. Returns its length, 0 on timeout, -1 if the server is gone. */
static int recv_msg(DWORD ms) {
    DWORD n = 0;
    if (!reading) {
        ResetEvent(rov.hEvent);
        if (ReadFile(pipe_h, rbuf, sizeof(rbuf), &n, &rov)) {
            return (int)n;
        }
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        reading = 1;
    }
    if (WaitForSingleObject(rov.hEvent, ms) != WAIT_OBJECT_0) return 0;
    reading = 0;
    if (!GetOverlappedResult(pipe_h, &rov, &n, FALSE)) return -1;
    return (int)n;
}

static void masked(const uint8_t *a) {
    printf("%02x:%02x:%02x:xx:xx:xx", a[0], a[1], a[2]);
}

/* Fields of our radiotap header (rl_radiotap_build layout, walked generically for the fields it can carry). */
struct rt_info {
    size_t len;
    uint16_t freq;
    int has_signal;
    int8_t signal;
};

static int parse_radiotap(const uint8_t *p, size_t len, struct rt_info *ri) {
    uint32_t present;
    size_t off = 8;
    memset(ri, 0, sizeof(*ri));
    if (len < 8 || p[0] != 0) return -1;
    ri->len = (size_t)(p[2] | (p[3] << 8));
    if (ri->len < 8 || ri->len > len) return -1;
    present = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    if (present & (1u << 31)) return 0;
    if (present & 1u) off = ((off + 7u) & ~(size_t)7u) + 8u; /* TSFT */
    if (present & 2u) off += 1u;                            /* FLAGS */
    if (present & 4u) off += 1u;                            /* RATE */
    if (present & 8u) {                                     /* CHANNEL */
        off = (off + 1u) & ~(size_t)1u;
        if (off + 4u <= ri->len) ri->freq = (uint16_t)(p[off] | (p[off + 1] << 8));
        off += 4u;
    }
    if (present & 0x10u) off += 2u;                         /* FHSS */
    if ((present & 0x20u) && off < ri->len) {               /* DBM_ANTSIGNAL */
        ri->has_signal = 1;
        ri->signal = (int8_t)p[off];
    }
    return 0;
}

static uint32_t build_probe(uint8_t *f, const uint8_t *own, const uint8_t *da, uint16_t sn) {
    static const uint8_t rates_5g[8] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    uint32_t n = 24;
    memset(f, 0, 24);
    f[0] = 0x40;
    if (da) memcpy(f + 4, da, 6);
    else memset(f + 4, 0xff, 6);
    memcpy(f + 10, own, 6);
    if (da) memcpy(f + 16, da, 6);
    else memset(f + 16, 0xff, 6);
    f[22] = (uint8_t)(sn << 4);
    f[23] = (uint8_t)(sn >> 4);
    f[n++] = 0; /* wildcard SSID */
    f[n++] = 0;
    f[n++] = 1; /* rates (5 GHz set; on 2.4 GHz the OFDM ones are still valid) */
    f[n++] = 8;
    memcpy(f + n, rates_5g, 8);
    return n + 8;
}

static int random_laa(uint8_t *mac) {
    unsigned int v[2];
    if (rand_s(&v[0]) != 0 || rand_s(&v[1]) != 0) return -1;
    for (int k = 0; k < 6; k++) mac[k] = (uint8_t)(v[k / 3] >> (8 * (k % 3)));
    mac[0] = (uint8_t)((mac[0] | 0x02u) & 0xfeu);
    return 0;
}

static const char *subtype_name(uint8_t s) {
    switch (s) {
    case 0: return "PSF";
    case 3: return "MIF";
    default: return "other";
    }
}

int main(int argc, char **argv) {
    unsigned seconds = 30, connect_timeout = 60, tx_test = 0, no_filter = 0, channel = 0;
    struct rl_hello hello;
    unsigned actions = 0, act_by_sub[256], data = 0, other = 0, on_channel = 0, off_channel = 0, n_ta = 0, msgs = 0;
    unsigned tx_sent = 0, tx_status = 0, tx_ok = 0, tx_bad = 0;
    uint8_t ta[MAX_TA][6], ghost[6];
    int sig_min = 0, sig_max = -128, have_sig = 0, rc = 1;
    struct rl_stats stats;
    int have_stats = 0;

    memset(act_by_sub, 0, sizeof(act_by_sub));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--connect-timeout") == 0 && i + 1 < argc) connect_timeout = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) channel = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--tx-test") == 0) tx_test = 1;
        else if (strcmp(argv[i], "--no-filter") == 0) no_filter = 1;
        else {
            printf("usage: radioctl [--seconds N (1..600)] [--channel C] [--tx-test] [--connect-timeout S] [--no-filter]\n");
            return 2;
        }
    }
    if (seconds < 1 || seconds > 600 || channel > 177) {
        printf("RESULT: FAIL - bad arguments\n");
        return 2;
    }
    if (tx_test && seconds < 10) {
        printf("RESULT: FAIL - --tx-test needs --seconds >= 10 (6 frames, one per second, from second 2)\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (connect_pipe(connect_timeout) != 0) {
        printf("RESULT: FAIL - cannot connect to %s within %u s (error %lu)\n", RL_PIPE_NAME, connect_timeout,
               GetLastError());
        return 1;
    }
    {
        int n = recv_msg(10000);
        struct rl_header h;
        if (n <= 0 || rl_get_header(rbuf, (size_t)n, &h) != 0 || h.type != RL_HELLO ||
            rl_get_hello(rbuf + RL_HEADER_SIZE, h.length, &hello) != 0) {
            printf("RESULT: FAIL - no valid HELLO from the server\n");
            return 1;
        }
    }
    printf("radioctl: connected; protocol %u, channel %u (%s), TX %s, address ", hello.version, hello.channel,
           hello.band ? "5 GHz" : "2.4 GHz", hello.tx_allowed ? "allowed" : "not allowed");
    masked(hello.addr);
    printf(", max frame %u, lease %u ms\n", hello.max_frame, hello.lease_ms);
    if (hello.version != RL_VERSION) {
        printf("RESULT: FAIL - protocol version %u, expected %u\n", hello.version, RL_VERSION);
        return 1;
    }
    if (no_filter) {
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        send_msg(RL_SET_BSSID_FILTER, zero, 6);
    }
    if (channel && channel != hello.channel) {
        uint8_t c = (uint8_t)channel;
        send_msg(RL_SET_CHANNEL, &c, 1);
    }
    if (tx_test && random_laa(ghost) != 0) {
        printf("RESULT: FAIL - rand_s failed\n");
        return 1;
    }

    uint64_t t0 = now_ms(), end = t0 + (uint64_t)seconds * 1000u, next_beat = t0 + 1000u, next_tx = t0 + 2000u;
    uint8_t cur_channel = hello.channel;
    int gone = 0;
    while (now_ms() < end) {
        uint64_t now = now_ms();
        if (now >= next_beat) {
            if (send_msg(RL_HEARTBEAT, NULL, 0) != 0) {
                gone = 1;
                break;
            }
            next_beat += 1000u;
        }
        if (tx_test && tx_sent < 6 && now >= next_tx) {
            uint8_t f[64], pl[128];
            int to_ghost = tx_sent >= 3;
            uint32_t flen = build_probe(f, hello.addr, to_ghost ? ghost : NULL, (uint16_t)(0x300 + tx_sent));
            size_t n = rl_build_tx(pl, sizeof(pl), 0x1000u + tx_sent, f, flen, 12 /* 6 Mb/s */);
            if (!n || send_msg(RL_TX, pl, n) != 0) {
                gone = 1;
                break;
            }
            printf("tx %u: %s probe request sent (cookie 0x%x)\n", tx_sent + 1, to_ghost ? "directed (ghost)" : "broadcast",
                   0x1000u + tx_sent);
            tx_sent++;
            next_tx += 1000u;
        }
        int n = recv_msg(50);
        if (n < 0) {
            gone = 1;
            break;
        }
        if (n == 0) continue;
        struct rl_header h;
        if (rl_get_header(rbuf, (size_t)n, &h) != 0) {
            printf("malformed message from the server (%d bytes)\n", n);
            continue;
        }
        msgs++;
        const uint8_t *p = rbuf + RL_HEADER_SIZE;
        if (h.type == RL_RX) {
            struct rt_info ri;
            if (parse_radiotap(p, h.length, &ri) != 0 || h.length - ri.len < 24) {
                printf("RX with a bad radiotap header\n");
                continue;
            }
            const uint8_t *f = p + ri.len;
            size_t flen = h.length - ri.len;
            uint16_t want = rl_channel_freq(cur_channel, cur_channel > 14 ? 1 : 0);
            if (ri.freq == want) on_channel++;
            else off_channel++;
            if (ri.has_signal) {
                if (!have_sig || ri.signal < sig_min) sig_min = ri.signal;
                if (ri.signal > sig_max) sig_max = ri.signal;
                have_sig = 1;
            }
            int known = 0;
            for (unsigned k = 0; k < n_ta; k++)
                if (memcmp(ta[k], f + 10, 6) == 0) known = 1;
            if (!known && n_ta < MAX_TA) memcpy(ta[n_ta++], f + 10, 6);
            if (f[0] == 0xd0 && flen >= 31 && f[24] == 127 && f[25] == 0x00 && f[26] == 0x17 && f[27] == 0xf2 && f[28] == 8) {
                if (actions == 0) {
                    printf("first AWDL action frame: %s from ", subtype_name(f[30]));
                    masked(f + 10);
                    printf(", %u MHz, %d dBm\n", ri.freq, ri.has_signal ? ri.signal : 0);
                }
                actions++;
                act_by_sub[f[30]]++;
            } else if ((f[0] & 0x0c) == 0x08) {
                data++;
            } else {
                other++;
            }
        } else if (h.type == RL_TX_STATUS) {
            struct rl_tx_status s;
            if (rl_get_tx_status(p, h.length, &s) != 0) continue;
            unsigned idx = s.cookie - 0x1000u;
            int ghost_frame = idx >= 3;
            int ok = ghost_frame ? (s.result == RL_TXR_DONE && s.acked == RL_ACK_NO)
                                 : (s.result == RL_TXR_DONE && s.acked == RL_ACK_NOT_REQUESTED);
            tx_status++;
            if (ok) tx_ok++;
            else tx_bad++;
            printf("tx status cookie 0x%x: result %u, ack %u, transmissions %u, channel %u, %u us, error %d -> %s\n",
                   s.cookie, s.result, s.acked, s.tx_count, s.channel, s.elapsed_us, s.error, ok ? "as expected" : "UNEXPECTED");
        } else if (h.type == RL_CHANNEL) {
            struct rl_channel c;
            if (rl_get_channel(p, h.length, &c) == 0) {
                printf("channel: now %u, %s, %u us, TX %s\n", c.channel, c.ok ? "switched" : "FAILED", c.elapsed_us,
                       c.tx_allowed ? "allowed" : "not allowed");
                cur_channel = c.channel;
            }
        } else if (h.type == RL_BYE) {
            printf("server said BYE (reason %u)\n", h.length >= 1 ? p[0] : 0);
            gone = 1;
            break;
        } else {
            printf("message type %u (%u bytes)\n", h.type, h.length);
        }
    }
    if (!gone && send_msg(RL_STATS_REQ, NULL, 0) == 0) {
        uint64_t until = now_ms() + 2000u;
        while (now_ms() < until) {
            int n = recv_msg(100);
            struct rl_header h;
            if (n < 0) break;
            if (n == 0 || rl_get_header(rbuf, (size_t)n, &h) != 0) continue;
            if (h.type == RL_STATS && rl_get_stats(rbuf + RL_HEADER_SIZE, h.length, &stats) == 0) {
                have_stats = 1;
                break;
            }
            if (h.type == RL_TX_STATUS) {
                struct rl_tx_status s;
                if (rl_get_tx_status(rbuf + RL_HEADER_SIZE, h.length, &s) == 0) {
                    unsigned idx = s.cookie - 0x1000u;
                    int ok = idx >= 3 ? (s.result == RL_TXR_DONE && s.acked == RL_ACK_NO)
                                      : (s.result == RL_TXR_DONE && s.acked == RL_ACK_NOT_REQUESTED);
                    tx_status++;
                    if (ok) tx_ok++;
                    else tx_bad++;
                }
            }
        }
    }

    printf("\n==== radioctl summary (%llu s) ====\n", (unsigned long long)((now_ms() - t0) / 1000u));
    printf("messages %u; AWDL action frames %u (PSF %u, MIF %u, other %u), data frames %u, other frames %u; "
           "per-frame channel = tuned %u, other %u; transmitters %u",
           msgs, actions, act_by_sub[0], act_by_sub[3], actions - act_by_sub[0] - act_by_sub[3], data, other, on_channel,
           off_channel, n_ta);
    for (unsigned k = 0; k < n_ta && k < 8; k++) {
        printf(k ? ", " : " (");
        masked(ta[k]);
    }
    printf("%s", n_ta ? (n_ta > 8 ? ", ...)" : ")") : "");
    if (have_sig) printf("; RSSI %d..%d dBm", sig_min, sig_max);
    printf("\n");
    if (have_stats)
        printf("server: RX frames %u, forwarded %u, filtered %u, bad FCS %u, own %u, pipe full %u; TX submitted %u, "
               "refused %u, done %u, ACKed %u, not ACKed %u; uptime %u ms\n",
               stats.v[RL_ST_RX_FRAMES], stats.v[RL_ST_RX_FORWARDED], stats.v[RL_ST_RX_FILTERED], stats.v[RL_ST_RX_BAD_FCS],
               stats.v[RL_ST_RX_OWN], stats.v[RL_ST_RX_PIPE_FULL], stats.v[RL_ST_TX_SUBMITTED], stats.v[RL_ST_TX_REFUSED],
               stats.v[RL_ST_TX_DONE], stats.v[RL_ST_TX_ACKED], stats.v[RL_ST_TX_NOT_ACKED], stats.v[RL_ST_UPTIME_MS]);
    if (gone) printf("the server ended the session early\n");

    int rx_pass = actions > 0 && off_channel == 0;
    if (rx_pass)
        printf("RX: PASS - %u AWDL action frame(s) through the pipe, every per-frame channel = tuned channel\n", actions);
    else if (actions)
        printf("RX: FAIL - %u frame(s) carried a per-frame channel other than the tuned one\n", off_channel);
    else
        printf("RX: FAIL - no AWDL action frame (is the iPhone's share sheet open, on this channel?)\n");
    int tx_pass = 1;
    if (tx_test) {
        tx_pass = tx_sent == 6 && tx_status == 6 && tx_bad == 0;
        printf("TX: %s - %u sent, %u statuses, %u as expected, %u unexpected\n", tx_pass ? "PASS" : "FAIL", tx_sent,
               tx_status, tx_ok, tx_bad);
    }
    rc = rx_pass && tx_pass ? 0 : 1;
    CloseHandle(pipe_h);
    return rc;
}
