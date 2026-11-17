/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Client end of `radiod serve` (driver/radiod/radio_link.h, docs/adr/0003-awdld-on-windows.md).
 * Function names follow raph559/winawdl awdl/win/radio_client.h @ 0db7031 so that the Windows I/O layer adapted
 * from it changes little; the implementation is ours and speaks our radio link.
 *
 * A reader thread queues received frames (radiotap + 802.11, as OWL's pcap callback expects), handles TX
 * statuses, channel answers and BYE, and sends a heartbeat when the event loop has been quiet for a second.
 * Everything else is called from the event loop. */
#pragma once
#include <stdint.h>

/* AWDL frames stay under 2.4 KiB; the radio link carries at most RL_MAX_PAYLOAD. */
#define RADIO_CLIENT_MAX_FRAME 4096

struct radio_client;

struct radio_client_stats {
	unsigned long long rx_frames, rx_dropped;   /* dropped: the queue was full */
	unsigned long long tx_sent, tx_acked, tx_not_acked, tx_no_ack_requested, tx_refused, tx_lost;
	unsigned long long channel_switches, channel_failures;
};

/* NULL when no radio server answers within 'timeout_ms' or it speaks another version. 'on_disconnect' runs on
 * the reader thread when the server goes away or says BYE. */
struct radio_client *radio_client_open(void (*on_disconnect)(void), unsigned timeout_ms);
void radio_client_close(struct radio_client *client);

/* Signalled whenever a frame is queued. A HANDLE, typed void* to keep windows.h out of this header. */
void *radio_client_rx_event(const struct radio_client *client);
/* The channel the radio is on (updated by the server's answers to set_channel). */
uint8_t radio_client_channel(const struct radio_client *client);
/* How wide a channel the radio receives on, in MHz. */
uint8_t radio_client_width_mhz(const struct radio_client *client);
/* The address the radio transmits from and ACKs (HELLO). */
const uint8_t *radio_client_address(const struct radio_client *client);
int radio_client_tx_allowed(const struct radio_client *client);

/* Copies the oldest queued frame (radiotap + 802.11) into 'frame' (RADIO_CLIENT_MAX_FRAME bytes) and returns
 * its length, or 0 when the queue is empty. */
int radio_client_pop(struct radio_client *client, uint8_t *frame);
int radio_client_has_frame(struct radio_client *client);

/* Transmits 'len' bytes of radiotap header + 802.11 frame (OWL's wlan_send buffer). Returns 0 on success. */
int radio_client_tx(struct radio_client *client, const uint8_t *buf, uint32_t len);
/* Asks for a channel switch; the answer arrives asynchronously. Returns 0 if the request was sent. */
int radio_client_set_channel(struct radio_client *client, uint8_t channel);
int radio_client_set_bssid_filter(struct radio_client *client, const uint8_t bssid[6]);

void radio_client_get_stats(struct radio_client *client, struct radio_client_stats *out);
