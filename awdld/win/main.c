/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AWDL daemon for Windows: OWL's protocol core (jedbillyb/owl), driven by `radiod serve`.
 * Usage: awdld [-c 6|44|149] [-S verbatim|rotate|pin|widen|intersect] [-W n] [-w s] [-f] [-n] [-r] [-s 1|2] [-l]
 *              [-j us] [-v] [-v].
 * -c: the channel the radio server was started on (checked). -S / -W: channel sequence strategy and how many
 * empty slots widen may fill (as jedbillyb/owl daemon/owl.c; default verbatim). -w: seconds to wait for the
 * radio server, 60 unless told. -f: keep OWL's RSSI filter (off by default: the radio's RSSI scale is not
 * calibrated). -n: no host adapter. -r: no device identities in the log. -s: spatial streams to announce, 2
 * unless told. -l: announce the long guard interval only. -j: microseconds to wait for the next TCP segment of a
 * burst before the joined ones go to the host, 400 unless told and 0 for no joining.
 *
 * Origin: raph559/winawdl awdl/win/main.c @ 0db7031 (GPL-3.0-or-later). Changed for mtdrop: -S/-W/-w/-f, radio
 * client statistics in the status line, messages name `radiod serve`. */
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "core.h"
#include "frame_fixup.h"
#include "host_if.h"
#include "log.h"
#include "radio_client.h"
#include "redact.h"
#include "win_internal.h"

#define STATUS_PERIOD_S 2.0
#define DEFAULT_CHANNEL 44
#define DEFAULT_STREAMS 2
/* Longer than most gaps between the frames of one aggregate, measured. */
#define DEFAULT_JOIN_WAIT_US 400

static int shown_level = LOG_INFO;
static int redact_log;
static struct redactor redactor;
/* The radio client's reader thread logs too. */
static SRWLOCK redactor_lock = SRWLOCK_INIT;

int __real_log_log(int level, const char *file, int line, const char *fmt, ...);

/* The linker sends every log_log call here, OWL's included (--wrap), so that
 * -r also covers the lines OWL formats itself. */
int __wrap_log_log(int level, const char *file, int line, const char *fmt, ...) {
	if (level > shown_level)
		return 0;
	va_list args, measured;
	va_start(args, fmt);
	va_copy(measured, args);
	int length = vsnprintf(NULL, 0, fmt, measured);
	va_end(measured);
	if (length < 0) {
		va_end(args);
		return 0;
	}
	char text[length + 1];
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (!redact_log)
		return __real_log_log(level, file, line, "%s", text);

	/* Redacting never doubles a line's length. */
	char shown[2 * sizeof(text)];
	AcquireSRWLockExclusive(&redactor_lock);
	redact_line(&redactor, text, shown, sizeof(shown));
	ReleaseSRWLockExclusive(&redactor_lock);
	return __real_log_log(level, file, line, "%s", shown);
}

static int channel_from_number(int number, struct awdl_chan *chan) {
	switch (number) {
		case 6:
			*chan = CHAN_OPCLASS_6;
			return 0;
		case 44:
			*chan = CHAN_OPCLASS_44;
			return 0;
		case 149:
			*chan = CHAN_OPCLASS_149;
			return 0;
		default:
			return -1;
	}
}

/* What each peer last said of its own place in the sync tree, which is all an
 * election goes by. Logged when the sync master changes: a master the device
 * being talked to does not share has cost a transfer 22 s. */
static void print_candidates(const struct daemon_state *state) {
	struct awdl_peer *peer;
	uint64_t now = clock_time_us();
	awdl_peers_it_t it = awdl_peers_it_new(state->awdl_state.peers.peers);
	while (awdl_peers_it_next(it, &peer) == PEERS_OK) {
		const struct awdl_election_state *said = &peer->election;
		char follows[18], master[18];
		snprintf(follows, sizeof(follows), "%s", ether_ntoa(&said->sync_addr));
		snprintf(master, sizeof(master), "%s", ether_ntoa(&said->master_addr));
		log_info("  %s%s follows %s under %s (met %u, ctr %u, height %u), itself (met %u, "
			 "ctr %u), heard %llu ms ago",
			 ether_ntoa(&peer->addr), peer->is_valid ? "" : " (not valid yet)", follows,
			 master, said->master_metric, said->master_counter, said->height,
			 said->self_metric, said->self_counter,
			 (unsigned long long) ((now - peer->last_update) / 1000));
	}
	awdl_peers_it_free(it);
}

static void print_status(struct ev_loop *loop, ev_timer *timer, int revents) {
	(void) loop;
	(void) revents;
	struct daemon_state *state = timer->data;
	const struct awdl_stats *stats = &state->awdl_state.stats;
	static struct ether_addr followed;
	if (memcmp(&followed, &state->awdl_state.election.sync_addr, sizeof(followed))) {
		followed = state->awdl_state.election.sync_addr;
		log_info("sync master is now %s, chosen among:", ether_ntoa(&followed));
		print_candidates(state);
	}
	char tree[256] = "";
	char joined[48] = "";
	awdl_election_tree_print(&state->awdl_state.election, tree, sizeof(tree));
	if (winawdl_segments_joined())
		snprintf(joined, sizeof(joined), " (%llu joined)", winawdl_segments_joined());
	struct radio_client_stats radio;
	unsigned long long delay_n;
	long long delay_mean, delay_min, delay_max;
	radio_client_get_stats(winawdl_radio(), &radio);
	mtdrop_rx_delay_take(&delay_n, &delay_mean, &delay_min, &delay_max);
	/* The sync tree stays last: redact.c hides everything up to the line's last bracket after an address
	 * unless that bracket holds the tree's "(met N, ctr N)". */
	log_info("radio: rx %llu (dropped %llu, delay us min %lld mean %lld max %lld over %llu), tx acked %llu, "
		 "not acked %llu, refused %llu, channel %u (switches %llu, failed %llu) | peers %d | rx action %llu, "
		 "data %llu%s | tx action %llu, data %llu (unicast %llu: deferred %llu for %llu ms, held %llu, in radio write "
		 "%llu ms; host empty %llu) | sync tree %s",
		 radio.rx_frames, radio.rx_dropped, delay_min, delay_mean, delay_max, delay_n, radio.tx_acked,
		 radio.tx_not_acked, radio.tx_refused,
		 radio_client_channel(winawdl_radio()), radio.channel_switches, radio.channel_failures,
		 awdl_peers_length(state->awdl_state.peers.peers), stats->rx_action, stats->rx_data,
		 joined, stats->tx_action, stats->tx_data, (unsigned long long) stats->tx_data_unicast,
		 (unsigned long long) stats->tx_unicast_deferred, (unsigned long long) (stats->tx_unicast_deferred_us / 1000),
		 (unsigned long long) stats->tx_unicast_held,
		 (unsigned long long) (stats->tx_unicast_send_us / 1000), (unsigned long long) stats->host_empty, tree);
}

int main(int argc, char *argv[]) {
	int channel = DEFAULT_CHANNEL;
	int log_level = LOG_INFO;
	const char *host_name = HOST_IF_NAME;
	int streams = DEFAULT_STREAMS;
	int short_guard_interval = 1;
	int option;
	int filter_rssi = 0;
	enum awdl_chanseq_strategy strategy = AWDL_CHANSEQ_VERBATIM;
	int widen_max = 4;
	winawdl_join_segments(DEFAULT_JOIN_WAIT_US);
	while ((option = getopt(argc, argv, "c:S:W:w:fnrs:lj:v")) != -1) {
		if (option == 'c') {
			channel = atoi(optarg);
		} else if (option == 'S') {
			if (!strcmp(optarg, "verbatim"))
				strategy = AWDL_CHANSEQ_VERBATIM;
			else if (!strcmp(optarg, "rotate"))
				strategy = AWDL_CHANSEQ_ROTATE;
			else if (!strcmp(optarg, "pin"))
				strategy = AWDL_CHANSEQ_PIN;
			else if (!strcmp(optarg, "widen"))
				strategy = AWDL_CHANSEQ_WIDEN;
			else if (!strcmp(optarg, "intersect"))
				strategy = AWDL_CHANSEQ_INTERSECT;
			else {
				fprintf(stderr, "unknown strategy '%s' (verbatim, rotate, pin, widen, intersect)\n",
					optarg);
				return EXIT_FAILURE;
			}
		} else if (option == 'W') {
			widen_max = atoi(optarg);
			if (widen_max < 0 || widen_max > AWDL_CHANSEQ_LENGTH) {
				fprintf(stderr, "-W must be 0..%d\n", AWDL_CHANSEQ_LENGTH);
				return EXIT_FAILURE;
			}
		} else if (option == 'w') {
			int seconds = atoi(optarg);
			if (seconds < 1 || seconds > 600) {
				fprintf(stderr, "-w must be 1..600\n");
				return EXIT_FAILURE;
			}
			mtdrop_radio_connect_timeout((unsigned) seconds * 1000u);
		} else if (option == 'f') {
			filter_rssi = 1;
		} else if (option == 'n') {
			host_name = "";
		} else if (option == 'r') {
			redact_log = 1;
		} else if (option == 's') {
			streams = atoi(optarg);
		} else if (option == 'l') {
			short_guard_interval = 0;
		} else if (option == 'j') {
			winawdl_join_segments(atoi(optarg));
		} else if (option == 'v') {
			if (log_level < LOG_TRACE)
				log_level++;
		} else {
			fprintf(stderr,
				"usage: %s [-c 6|44|149] [-S verbatim|rotate|pin|widen|intersect] [-W n] "
				"[-w s] [-f] [-n] [-r] [-s 1|2] [-l] [-j us] [-v] [-v]\n",
				argv[0]);
			return EXIT_FAILURE;
		}
	}
	log_set_level(log_level);
	shown_level = log_level;

	struct awdl_chan chan;
	if (channel_from_number(channel, &chan) < 0) {
		log_error("unsupported channel %d (use 6, 44 or 149)", channel);
		return EXIT_FAILURE;
	}

	static struct daemon_state state;
	if (awdl_init(&state, "radio", host_name, chan, NULL) < 0) {
		log_error("could not initialize: is `radiod serve` running?");
		return EXIT_FAILURE;
	}
	if (radio_client_channel(winawdl_radio()) != channel) {
		log_error("the radio server is on channel %d, not %d",
			  radio_client_channel(winawdl_radio()), channel);
		awdl_free(&state);
		return EXIT_FAILURE;
	}
	frame_fixup_announce(streams == 2, short_guard_interval,
			     radio_client_width_mhz(winawdl_radio()));
	/* The radio's RSSI scale is not calibrated, so peers are not gated on it unless -f. */
	state.awdl_state.filter_rssi = filter_rssi;
	state.awdl_state.channel.strategy = strategy;
	state.awdl_state.channel.widen_max = widen_max;
	log_info("channel sequence strategy %s%s", strategy == AWDL_CHANSEQ_PIN ? "pin" :
		 strategy == AWDL_CHANSEQ_ROTATE ? "rotate" : strategy == AWDL_CHANSEQ_WIDEN ? "widen" :
		 strategy == AWDL_CHANSEQ_INTERSECT ? "intersect" : "verbatim",
		 strategy == AWDL_CHANSEQ_WIDEN ? " (see -W)" : "");
	log_info("AWDL address %s on channel %d", ether_ntoa(&state.io.if_ether_addr), channel);
	if (state.io.host_ifindex)
		log_info("host interface %s, index %d", state.io.host_ifname,
			 state.io.host_ifindex);

	struct ev_loop *loop = EV_DEFAULT;
	awdl_schedule(loop, &state);

	ev_timer status;
	status.data = &state;
	ev_timer_init(&status, print_status, STATUS_PERIOD_S, STATUS_PERIOD_S);
	ev_timer_start(loop, &status);

	ev_run(loop, 0);

	awdl_free(&state);
	return EXIT_SUCCESS;
}
