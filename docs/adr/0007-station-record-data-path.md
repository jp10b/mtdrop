# ADR-0007: A station record for the peer, so unicast data can leave the ALTX queue

- **Status:** Accepted (2026-11-18, maintainer). S1/S2 measured 2026-11-19 (F-035): assumption confirmed, AC queue with a station record 1.3–3.4 ms per frame, burst 2156 vs 176 frames/s
- **Deciders:** maintainer
- **Inputs:** ADR-0006 A (throughput); `docs/FINDINGS.md` F-019/F-020 (data on the AC queue waits 115–560 ms, on ALTX0 ~6 ms per frame), F-030–F-034 (send plateau 130–220 kB/s, the radio completes one frame every ~5–6 ms); `docs/MT7921-BRINGUP.md` (H14, open points at lines ~1051–1063, 1283, 1313, 1325); mt76 (`openwrt/mt76` @ 0936617, BSD-3-Clause-Clear): `mt7921/main.c:806-855` (`mt7921_mac_sta_add`), `mt7921/mcu.c:1099-1121` (`mt7921_mcu_sta_update`), `mt76_connac_mcu.c:1094-…` (`mt76_connac_mcu_sta_cmd`); winawdl DESIGN-NOTES "Sending" (2.2 MB/s after rate control and block-ack aggregation on its chip)

## Context

Every unicast data frame we send goes through the firmware's management queue ALTX0 at a fixed rate, with the vif's own WTBL entry (wcid 19) and **no station record** for the phone (M5 choice, F-019/F-020: on the normal AC queue the same frames waited 115–560 ms before their first transmission). On ALTX0 a frame completes ~6 ms after submission even when it is ACKed at the first try, and the firmware handles them one after another: ~160 frames/s, the plateau of every send since F-028, at any PHY rate (F-034). TCP is no longer the limit (F-034: `host empty` 3783 → 230).

mt76 never sends data like this. For every peer it allocates a WTBL index and sends `MCU_UNI_CMD(STA_REC_UPDATE)` with a basic TLV (connection state, peer address, AID), the peer's capabilities (HT/VHT/HE, rates), the WTBL TLVs (generic, header translation, HT) and, with `offload_fw`, lets the firmware do rate adaptation; data then goes on the AC queues with that wcid, and aggregation is set up per TID with `STA_REC_BA` after an ADDBA exchange.

ASSUMPTION (not verified): the AC-queue delay of F-019/F-020 comes from the missing station record – our BSS is registered as `CONNECTION_INFRA_AP` with no station, so the firmware may treat unicast data to an unknown receiver like data for a sleeping client and release it periodically (100–500 ms matches a beacon/DTIM-like period).

## Options

| | A: station record + AC queues (proposed) | B: tune ALTX (smaller frames, different queue flags) | C: leave it |
|---|---|---|---|
| expected gain | large if the assumption holds (AC queues flow, firmware rate adaptation picks HT/VHT rates; later A-MPDU) | small: the ~6 ms per frame is the firmware's handling of ALTX | none (~190 kB/s) |
| risk | new MCU commands (hang risk as in F-011 – mitigated by steps, watchdog, give-back) | low | none |
| work | large (library TLVs + tests, radio link message per peer, serve/awdld) | small | – |

## Decision (proposed): A, in steps that each stop at a measurement

| step | what | measured with | acceptance |
|---|---|---|---|
| S0 | Research: exact TLVs of `STA_REC_UPDATE` for mt7921 (basic, WTBL generic/hdr-trans, HT/VHT, RA offload), WTBL index range, which `conn_type` a peer of our AP-type BSS gets; written into `docs/MT7921-BRINGUP.md` with source lines | – | reviewed |
| S1 | Library: `mt792x_sta_add(peer address)` / `mt792x_sta_remove` (basic + WTBL generic only), TX of unicast data to a known peer on its AC queue with its wcid; offline tests of the TLV bytes | `radiod acktest --data --sta` against the home access point (no phone): completion time of data frames on the AC queue | **completion ≤ 10 ms** (F-019: 115–560 ms); no hang |
| S2 | Burst throughput on the AC queue with a station record vs ALTX | `acktest --data --burst N` against the access point: frames per second | clearly above ALTX's ~160/s |
| S3 | `serve`: add/remove a station when awdld adds/removes a peer (new radio link message), data to known peers on AC queues | send of the 5.4 MB photo to the iPhone | faster than 190 kB/s, losses ≤ 3 % |
| S4 | Firmware rate adaptation (RA offload with the peer's HT/VHT capabilities from its AWDL frames) | same | further gain |
| S5 | Block-ack aggregation (`STA_REC_BA` after ADDBA) | same | further gain |

Each hardware step runs in the usual service session with the watchdog; the ALTX path stays the default until S3 is measured better, behind an option until then.

## Consequences

- `mt792x` gains station management; `radio_link.h` gains add/remove-peer messages (protocol version bump); OWL's peer add/remove hooks call them.
- If S1 shows the AC queue still holding frames with a station record, the assumption is wrong and the ADR is revisited before any further step.

## Open points

- WTBL size and which indices are free next to the vif's 19 (mt76: `MT792x_WTBL_STA - 1`, 20 entries reserved for vifs).
- Whether a peer of an AP-type BSS must be "associated" (`MT76_STA_INFO_STATE_ASSOC`) for the firmware to send to it.
- Power save: the phone's AWDL presence is not 802.11 power save; the firmware must not buffer for it.
