/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mtdrop: OWL dates received AWDL frames with radiod's radiotap TSFT (host QPC microseconds at which the frame left
 * the RX ring) instead of the time it gets round to processing them (owl/src/rx.c, OWL_RADIOTAP_TSFT_IS_HOST_CLOCK,
 * docs/FINDINGS.md F-022). Built into owl_tests, against the same OWL library as the daemon. */

extern "C" {
#include "peers.h"
#include "rx.h"
#include "state.h"
#include "version.h"
}

#include <vector>

#include "gtest/gtest.h"

namespace {

const struct ether_addr self = {{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}};
const struct ether_addr peer_addr = {{0x06, 0x66, 0x77, 0x88, 0x99, 0xaa}};

/* radiotap as radiod builds it (driver/radiod/radio_link.c): TSFT, FLAGS (no FCS); then an AWDL PSF without TLVs. */
std::vector<uint8_t> psf(bool with_tsft, uint64_t tsft) {
	std::vector<uint8_t> f;
	if (with_tsft) {
		f = {0x00, 0x00, 17, 0x00, 0x03, 0x00, 0x00, 0x00};
		for (int i = 0; i < 8; i++)
			f.push_back((uint8_t) (tsft >> (8 * i)));
		f.push_back(0x00); /* flags */
	} else {
		f = {0x00, 0x00, 9, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};
	}
	const uint8_t hdr[] = {0xd0, 0x00, 0x00, 0x00};       /* action, duration */
	f.insert(f.end(), hdr, hdr + sizeof(hdr));
	f.insert(f.end(), 6, 0xff);                             /* addr1: broadcast */
	f.insert(f.end(), peer_addr.ether_addr_octet, peer_addr.ether_addr_octet + 6);
	const uint8_t bssid[] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
	f.insert(f.end(), bssid, bssid + 6);
	f.push_back(0x00);
	f.push_back(0x00);                                      /* sequence control */
	const uint8_t action[] = {127, 0x00, 0x17, 0xf2, 8, awdl_version(1, 0), 0 /* PSF */, 0};
	f.insert(f.end(), action, action + sizeof(action));
	f.insert(f.end(), 8, 0x00);                             /* phy_tx, target_tx */
	return f;
}

class RxTsft : public ::testing::Test {
  protected:
	struct awdl_state state;

	void SetUp() override {
		struct awdl_chan c44 = CHAN_OPCLASS_44;
		awdl_init_state(&state, "test", &self, c44, clock_time_us());
	}
	void TearDown() override { awdl_peers_free(state.peers.peers); }

	uint64_t rx_and_last_update(const std::vector<uint8_t> &f) {
		const struct buf *frame = buf_new_const(f.data(), f.size());
		struct buf *out[4];
		struct buf **cursor = &out[0];
		EXPECT_EQ(awdl_rx(frame, &cursor, &out[4], &state), RX_OK);
		buf_free(frame);
		struct awdl_peer *peer = nullptr;
		EXPECT_EQ(awdl_peer_get(state.peers.peers, &peer_addr, &peer), PEERS_OK);
		return peer ? peer->last_update : 0;
	}
};

TEST_F(RxTsft, dates_frame_with_radiod_timestamp) {
	uint64_t received = clock_time_us() - 400000; /* left the RX ring 400 ms ago */
	EXPECT_EQ(rx_and_last_update(psf(true, received)), received);
}

TEST_F(RxTsft, falls_back_to_now_without_tsft) {
	uint64_t before = clock_time_us();
	uint64_t dated = rx_and_last_update(psf(false, 0));
	EXPECT_GE(dated, before);
	EXPECT_LE(dated, clock_time_us());
}

TEST_F(RxTsft, ignores_timestamp_from_the_future) {
	uint64_t before = clock_time_us();
	uint64_t dated = rx_and_last_update(psf(true, before + 5000000));
	EXPECT_GE(dated, before);
	EXPECT_LE(dated, clock_time_us());
}

TEST_F(RxTsft, ignores_timestamp_from_another_clock) {
	uint64_t before = clock_time_us();
	uint64_t dated = rx_and_last_update(psf(true, 1000)); /* a chip TSF or similar: far older than 10 s */
	EXPECT_GE(dated, before);
}

} // namespace
