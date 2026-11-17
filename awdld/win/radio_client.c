/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Client end of `radiod serve`: see radio_client.h. */
#include "radio_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "radio_link.h"

#define QUEUE_FRAMES 1024u
#define HEARTBEAT_MS 1000u
#define HELLO_WAIT_MS 5000u

struct radio_client {
	HANDLE pipe;
	HANDLE thread;
	HANDLE rx_event;   /* auto-reset: a frame was queued */
	HANDLE stop_event; /* manual-reset: radio_client_close */
	HANDLE write_done;
	SRWLOCK write_lock;
	uint8_t wbuf[RL_MAX_MESSAGE];
	uint32_t next_cookie;
	volatile LONG64 last_write_ms;

	SRWLOCK queue_lock;
	uint8_t (*queue)[RADIO_CLIENT_MAX_FRAME];
	uint32_t queue_len[QUEUE_FRAMES];
	unsigned head, count;

	struct rl_hello hello;
	volatile LONG channel;
	SRWLOCK stats_lock;
	struct radio_client_stats stats;
	void (*on_disconnect)(void);
	volatile LONG closing;
};

static int write_msg(struct radio_client *c, uint16_t type, const uint8_t *a, uint32_t alen, const uint8_t *b,
		     uint32_t blen) {
	OVERLAPPED ov;
	DWORD n = 0;
	int rc = -1;
	if (alen + blen > RL_MAX_PAYLOAD)
		return -1;
	AcquireSRWLockExclusive(&c->write_lock);
	rl_put_header(c->wbuf, sizeof(c->wbuf), type, alen + blen);
	if (alen)
		memcpy(c->wbuf + RL_HEADER_SIZE, a, alen);
	if (blen)
		memcpy(c->wbuf + RL_HEADER_SIZE + alen, b, blen);
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = c->write_done;
	ResetEvent(c->write_done);
	if (WriteFile(c->pipe, c->wbuf, RL_HEADER_SIZE + alen + blen, &n, &ov) ||
	    (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(c->pipe, &ov, &n, TRUE)))
		rc = 0;
	InterlockedExchange64(&c->last_write_ms, (LONG64) GetTickCount64());
	ReleaseSRWLockExclusive(&c->write_lock);
	return rc;
}

static void queue_frame(struct radio_client *c, const uint8_t *p, uint32_t len) {
	int queued = 0;
	if (len > RADIO_CLIENT_MAX_FRAME)
		return;
	AcquireSRWLockExclusive(&c->queue_lock);
	if (c->count < QUEUE_FRAMES) {
		unsigned slot = (c->head + c->count) % QUEUE_FRAMES;
		memcpy(c->queue[slot], p, len);
		c->queue_len[slot] = len;
		c->count++;
		queued = 1;
	}
	ReleaseSRWLockExclusive(&c->queue_lock);
	AcquireSRWLockExclusive(&c->stats_lock);
	if (queued)
		c->stats.rx_frames++;
	else
		c->stats.rx_dropped++;
	ReleaseSRWLockExclusive(&c->stats_lock);
	if (queued)
		SetEvent(c->rx_event);
}

static void on_tx_status(struct radio_client *c, const struct rl_tx_status *s) {
	AcquireSRWLockExclusive(&c->stats_lock);
	if (s->result == RL_TXR_REFUSED)
		c->stats.tx_refused++;
	else if (s->result != RL_TXR_DONE)
		c->stats.tx_lost++;
	else if (s->acked == RL_ACK_YES)
		c->stats.tx_acked++;
	else if (s->acked == RL_ACK_NO)
		c->stats.tx_not_acked++;
	else if (s->acked == RL_ACK_NOT_REQUESTED)
		c->stats.tx_no_ack_requested++;
	ReleaseSRWLockExclusive(&c->stats_lock);
	if (s->result == RL_TXR_REFUSED)
		log_debug("radio refused TX %u: error %d", s->cookie, s->error);
	else if (s->acked == RL_ACK_NO)
		log_trace("TX %u not acknowledged after %u transmissions", s->cookie, s->tx_count);
}

/* Returns 0 to keep reading, -1 when the server said BYE. */
static int handle(struct radio_client *c, const uint8_t *msg, DWORD len) {
	struct rl_header h;
	const uint8_t *p = msg + RL_HEADER_SIZE;
	if (rl_get_header(msg, len, &h) != 0) {
		log_warn("radio link: malformed message (%lu bytes) ignored", (unsigned long) len);
		return 0;
	}
	switch (h.type) {
		case RL_RX:
			queue_frame(c, p, h.length);
			return 0;
		case RL_TX_STATUS: {
			struct rl_tx_status s;
			if (rl_get_tx_status(p, h.length, &s) == 0)
				on_tx_status(c, &s);
			return 0;
		}
		case RL_CHANNEL: {
			struct rl_channel ch;
			if (rl_get_channel(p, h.length, &ch) != 0)
				return 0;
			InterlockedExchange(&c->channel, ch.channel);
			AcquireSRWLockExclusive(&c->stats_lock);
			if (ch.ok)
				c->stats.channel_switches++;
			else
				c->stats.channel_failures++;
			ReleaseSRWLockExclusive(&c->stats_lock);
			if (!ch.ok)
				log_warn("radio could not switch channel; still on %u", ch.channel);
			return 0;
		}
		case RL_BYE:
			log_warn("radio server said goodbye (reason %u)", h.length >= 1 ? p[0] : 0);
			return -1;
		default:
			return 0; /* HELLO again, STATS: nothing to do */
	}
}

static DWORD WINAPI reader(LPVOID arg) {
	struct radio_client *c = arg;
	static uint8_t buf[RL_MAX_MESSAGE];
	OVERLAPPED ov;
	HANDLE read_done = CreateEventA(NULL, TRUE, FALSE, NULL);
	int pending = 0, gone = 0;
	if (!read_done)
		gone = 1;
	while (!gone && !c->closing) {
		DWORD n = 0;
		if (!pending) {
			memset(&ov, 0, sizeof(ov));
			ov.hEvent = read_done;
			ResetEvent(read_done);
			if (ReadFile(c->pipe, buf, sizeof(buf), &n, &ov)) {
				if (handle(c, buf, n) != 0)
					gone = 1;
				continue;
			}
			if (GetLastError() != ERROR_IO_PENDING) {
				gone = 1;
				break;
			}
			pending = 1;
		}
		HANDLE wait[2] = {read_done, c->stop_event};
		DWORD r = WaitForMultipleObjects(2, wait, FALSE, HEARTBEAT_MS / 2);
		if (r == WAIT_OBJECT_0 + 1)
			break;
		if (r == WAIT_TIMEOUT) {
			/* The lease: say something at least once a second even when OWL is quiet. */
			if ((uint64_t) GetTickCount64() - (uint64_t) c->last_write_ms >= HEARTBEAT_MS &&
			    write_msg(c, RL_HEARTBEAT, NULL, 0, NULL, 0) != 0)
				gone = 1;
			continue;
		}
		pending = 0;
		if (!GetOverlappedResult(c->pipe, &ov, &n, FALSE)) {
			gone = 1;
			break;
		}
		if (handle(c, buf, n) != 0)
			gone = 1;
	}
	if (pending)
		CancelIoEx(c->pipe, &ov);
	if (read_done)
		CloseHandle(read_done);
	if (gone && !c->closing) {
		log_warn("radio server disconnected");
		if (c->on_disconnect)
			c->on_disconnect();
	}
	return 0;
}

static void free_client(struct radio_client *c) {
	if (c->pipe && c->pipe != INVALID_HANDLE_VALUE)
		CloseHandle(c->pipe);
	if (c->rx_event)
		CloseHandle(c->rx_event);
	if (c->stop_event)
		CloseHandle(c->stop_event);
	if (c->write_done)
		CloseHandle(c->write_done);
	free(c->queue);
	free(c);
}

static int read_hello(struct radio_client *c) {
	uint8_t buf[RL_HEADER_SIZE + RL_HELLO_SIZE + 64];
	OVERLAPPED ov;
	DWORD n = 0;
	struct rl_header h;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = c->write_done;
	ResetEvent(c->write_done);
	if (!ReadFile(c->pipe, buf, sizeof(buf), &n, &ov)) {
		if (GetLastError() != ERROR_IO_PENDING)
			return -1;
		if (WaitForSingleObject(c->write_done, HELLO_WAIT_MS) != WAIT_OBJECT_0) {
			CancelIoEx(c->pipe, &ov);
			return -1;
		}
		if (!GetOverlappedResult(c->pipe, &ov, &n, FALSE))
			return -1;
	}
	if (rl_get_header(buf, n, &h) != 0 || h.type != RL_HELLO ||
	    rl_get_hello(buf + RL_HEADER_SIZE, h.length, &c->hello) != 0)
		return -1;
	return 0;
}

struct radio_client *radio_client_open(void (*on_disconnect)(void), unsigned timeout_ms) {
	struct radio_client *c = calloc(1, sizeof(*c));
	ULONGLONG until = GetTickCount64() + timeout_ms;
	DWORD mode = PIPE_READMODE_MESSAGE;
	if (!c)
		return NULL;
	InitializeSRWLock(&c->write_lock);
	InitializeSRWLock(&c->queue_lock);
	InitializeSRWLock(&c->stats_lock);
	c->on_disconnect = on_disconnect;
	c->queue = calloc(QUEUE_FRAMES, RADIO_CLIENT_MAX_FRAME);
	c->rx_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	c->stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	c->write_done = CreateEventA(NULL, TRUE, FALSE, NULL);
	c->pipe = INVALID_HANDLE_VALUE;
	if (!c->queue || !c->rx_event || !c->stop_event || !c->write_done) {
		free_client(c);
		return NULL;
	}
	for (;;) {
		c->pipe = CreateFileA(RL_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
				      FILE_FLAG_OVERLAPPED, NULL);
		if (c->pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetTickCount64() >= until) {
			log_error("no radio server on %s (error %lu)", RL_PIPE_NAME, GetLastError());
			free_client(c);
			return NULL;
		}
		Sleep(250);
	}
	if (!SetNamedPipeHandleState(c->pipe, &mode, NULL, NULL) || read_hello(c) != 0) {
		log_error("the radio server sent no valid HELLO");
		free_client(c);
		return NULL;
	}
	if (c->hello.version != RL_VERSION) {
		log_error("radio link version %u, expected %u", c->hello.version, RL_VERSION);
		free_client(c);
		return NULL;
	}
	c->channel = c->hello.channel;
	c->last_write_ms = (LONG64) GetTickCount64();
	c->thread = CreateThread(NULL, 0, reader, c, 0, NULL);
	if (!c->thread) {
		free_client(c);
		return NULL;
	}
	return c;
}

void radio_client_close(struct radio_client *c) {
	if (!c)
		return;
	InterlockedExchange(&c->closing, 1);
	SetEvent(c->stop_event);
	WaitForSingleObject(c->thread, 5000);
	CloseHandle(c->thread);
	free_client(c);
}

void *radio_client_rx_event(const struct radio_client *c) {
	return c->rx_event;
}

uint8_t radio_client_channel(const struct radio_client *c) {
	return (uint8_t) c->channel;
}

uint8_t radio_client_width_mhz(const struct radio_client *c) {
	return c->hello.width_mhz;
}

const uint8_t *radio_client_address(const struct radio_client *c) {
	return c->hello.addr;
}

int radio_client_tx_allowed(const struct radio_client *c) {
	return c->hello.tx_allowed;
}

int radio_client_pop(struct radio_client *c, uint8_t *frame) {
	int len = 0;
	AcquireSRWLockExclusive(&c->queue_lock);
	if (c->count) {
		len = (int) c->queue_len[c->head];
		memcpy(frame, c->queue[c->head], (size_t) len);
		c->head = (c->head + 1u) % QUEUE_FRAMES;
		c->count--;
	}
	ReleaseSRWLockExclusive(&c->queue_lock);
	return len;
}

int radio_client_has_frame(struct radio_client *c) {
	int has;
	AcquireSRWLockShared(&c->queue_lock);
	has = c->count != 0;
	ReleaseSRWLockShared(&c->queue_lock);
	return has;
}

int radio_client_tx(struct radio_client *c, const uint8_t *buf, uint32_t len) {
	uint8_t cookie[4];
	uint32_t id = ++c->next_cookie; /* event loop thread only */
	cookie[0] = (uint8_t) id;
	cookie[1] = (uint8_t) (id >> 8);
	cookie[2] = (uint8_t) (id >> 16);
	cookie[3] = (uint8_t) (id >> 24);
	if (write_msg(c, RL_TX, cookie, 4, buf, len) != 0)
		return -1;
	AcquireSRWLockExclusive(&c->stats_lock);
	c->stats.tx_sent++;
	ReleaseSRWLockExclusive(&c->stats_lock);
	return 0;
}

int radio_client_set_channel(struct radio_client *c, uint8_t channel) {
	return write_msg(c, RL_SET_CHANNEL, &channel, 1, NULL, 0);
}

int radio_client_set_bssid_filter(struct radio_client *c, const uint8_t bssid[6]) {
	return write_msg(c, RL_SET_BSSID_FILTER, bssid, 6, NULL, 0);
}

void radio_client_get_stats(struct radio_client *c, struct radio_client_stats *out) {
	AcquireSRWLockShared(&c->stats_lock);
	*out = c->stats;
	ReleaseSRWLockShared(&c->stats_lock);
}
