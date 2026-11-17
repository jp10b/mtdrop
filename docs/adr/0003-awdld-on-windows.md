# ADR-0003: awdld on Windows – process split, radio link, toolchain, reuse

- **Status:** Accepted (2026-11-17, maintainer). MinGW-w64 install via winget approved the same day.
- **Deciders:** maintainer
- **Inputs:** `docs/RESEARCH.md` §2.1–2.2, §3.1, §6, §7, §9; ADR-0001, ADR-0002 (radio interface, fixed channel first); `docs/FINDINGS.md` F-008 (AWDL RX on 44 and 6), F-013, F-014 (unicast TX, ACK, TXS); `third_party/raph559_winawdl` @ 0db7031 (`awdl/`, `DESIGN-NOTES.md`); `third_party/jedbillyb_owl` @ 57b8678

## Context

M3 and M4 are done: `radiod` brings the MT7921 up from user mode, receives on a chosen channel with the per-frame channel, switches channel in ~10 ms, transmits broadcast and unicast frames, the firmware ACKs frames to our MAC, and every unicast frame gets an honest TX status (F-014). M5 puts the AWDL protocol on top: the iPhone (share sheet open) and the PC synchronise, the peer becomes valid, and IPv6 link-local traffic flows over an `awdl0` adapter.

What we know (FACT unless marked):

- OWL (`src/`, ~3.5 kLOC) is the AWDL core; its daemon (`daemon/core.c`) drives it from libev with pcap (radiotap) for the radio and a TAP device for the host side (RESEARCH §2.1, §6). The jedbillyb fork adds channel-sequence strategies, sequence adoption, `sync_offset` and bounds fixes (RESEARCH §2.2).
- OWL is GNU C: `__attribute__((__packed__))` on every wire struct (`src/frame.h`, `src/ieee80211.h`), `__attribute__((unused))`, `__builtin_bswap*`. No bit-fields inside packed structs (only in `src/peers.h`, unpacked). MSVC cannot compile it unchanged; this machine has MSVC 14.51 only (no clang, no GCC).
- winawdl runs the **unmodified upstream** OWL core on Windows as a separate `awdld` process built with MinGW-w64, talking to its radio daemon over a named pipe, with Wintun for `awdl0` (RESEARCH §3.1). Its `awdl/` directory (Windows I/O layer, libev subset, pcap/POSIX shims, Wintun adapter, Ethernet ↔ IPv6 conversion, frame fixups) is GPL-3.0-or-later and may be reused with attribution. Its `radio/` directory is GPL-2.0-only and must not be reused (`third_party/NOTICE`).
- winawdl measured on a fixed channel 44: the iPhone adds slots on 44 while it sends; two fixes were needed (clear OWL's 40 MHz HT capability bits; cut trailing zero bytes off iPhone MIFs, or the peer never becomes valid) (`DESIGN-NOTES.md`, "Channel and rate").
- OWL sends action frames (broadcast) and data frames (unicast); it has no retransmission of its own, so unicast frames need radio ACK + retries – which we now have (F-014).
- The library already accepts an own address at sniffer start (`struct mt792x_sniffer_cfg.mac`, used for `DEV_INFO` and the TX address check). Whether the firmware then ACKs frames to such an address is **TODO-VERIFY** (F-014 tested the chip's own MAC only).

## Options

### 1. Process model

| | A: two processes, `radiod` serves a pipe, `awdld` is its client | B: one process, OWL linked into `radiod` |
|---|---|---|
| Matches ARCHITECTURE / ADR-0002 radio interface | yes | no (no interface boundary) |
| Chip-family independence | the pipe *is* the radio interface | needs a second refactor later |
| Toolchains | `radiod` MSVC, `awdld` anything | one compiler must handle both the library rules (`/W4 /WX`) and GNU C |
| Fault isolation | `awdld` crash → `radiod` stops TX and shuts the chip down orderly | a crash in OWL kills the process that owns DMA (the proxy stop list still runs) |
| Latency | one pipe hop per frame; winawdl reached MB/s with it | lowest |
| Prior art | winawdl, proven | none |

### 2. Toolchain for `awdld`

| | MinGW-w64 GCC (UCRT) | LLVM clang (MSVC target) | MSVC + source changes |
|---|---|---|---|
| Compiles OWL unchanged | yes (proven by winawdl) | GNU attributes yes; needs more POSIX shims (`sys/time.h`, `getopt`, `unistd.h`) than winawdl wrote | no – every packed struct and attribute must be edited |
| winawdl's `awdl/` shims usable as-is | yes | partly | no |
| OWL's googletest suite | g++ included | clang++ included | MSVC can, after the edits |
| Install | one package (winget or scoop) | one package (winget), or the VS "C++ Clang" component | nothing |

### 3. Radio link (pipe protocol)

Our own header (`driver/radiod/radio_link.h`, GPL-3.0-or-later or MIT, written here), message set modelled on winawdl's: `HELLO` (version, channel, own address, caps), `RX` (radiotap + 802.11 frame), `TX` (802.11 frame without FCS), `SET_CHANNEL`, `SET_BSSID_FILTER`, `SET_ADDRESS`, plus what our radio can do that winawdl's did not report: `TX_STATUS` (acked / not / dropped, transmissions) and `STATS`. We do not copy `radio/radio_link.h` (it sits in the GPL-2.0-only directory).

## Decision

1. **Two processes (option A).** `radiod serve <channel> <patch> <ram>` becomes a long-running radio server: one client at a time on `\\.\pipe\mtdrop-radio`, pipe ACL SYSTEM + Administrators. **Lease:** when the client disconnects or stays silent past a heartbeat (default 5 s), `radiod` stops transmitting, shuts the chip down orderly and exits; the existing session scripts then give the card back. The chip library and its HAL do not change.
2. **Radiotap is made in `radiod`, not in `awdld`.** Per received frame: `TSFT` (host clock in µs, as winawdl; OWL uses it only as a time base), `FLAGS` (bad FCS), `CHANNEL` (the per-frame channel, never the tuned one), `DBM_ANTSIGNAL`, `RATE` or `MCS`. `radiod` filters by AWDL BSSID (address 3 `00:25:00:ff:94:73`) and drops its own transmissions.
3. **TX policy lives in `radiod`:** group-addressed → `NO_ACK`, fixed rate; unicast → ACK requested, 15 tries, TX status reported back over the pipe. Rate for unicast data without `FIX_RATE` is H14 and is logged from the TXS (F-014 open point 5).
4. **`awdld` = jedbillyb/owl @ 57b8678** (`src/` and `daemon/core.c`, unmodified where possible) **+ a Windows layer adapted from winawdl `awdl/` @ 0db7031** (`compat/*`, `win/ev_win.c`, `win/io_win.c`, `win/radio_client.c`, `win/host_if.c`, `win/host_frame.c`, `win/netutils_win.c`, `win/frame_fixup.c`, `win/service_bridge.c`, `win/tcp_merge.c`, `win/redact.c`, tests). winawdl targets upstream OWL's `core.c`; the fork's `core.c` additionally uses `set_channel_fd()` / `set_channel_drain()` and a stats timer – our `netutils_win.c` returns `-1` for the fd (no asynchronous replies). Every reused file records its origin in its header and in `third_party/NOTICE`. Radiotap parser: `radiotap/radiotap-library` (OWL's submodule; licence checked before use – TODO-VERIFY ISC).
5. **Toolchain: MinGW-w64 GCC (UCRT) for `awdld` only**, statically linked (no runtime DLLs beside an elevated binary); `radiod` and the chip library stay on MSVC. Reason: the only option proven to build this exact code on Windows without edits. clang is the fallback if MinGW causes trouble.
6. **Channel: fixed 44 first**, no hopping (ADR-0002 §5), winawdl's two fixes applied from the start. Whether iOS 26.5 keeps sending unicast to a peer that advertises a fixed sequence is the first thing M5 measures (RESEARCH §9 item 2); hopping (6 + 44) is added only if discovery or transfer fails without it.
7. **AWDL address: random locally administered per session** (as Apple devices and winawdl do), programmed into the chip through `cfg.mac` so the firmware ACKs it. If the firmware does not ACK it, fall back to the chip's MAC and note the privacy cost in the README.
8. **Wintun:** the official signed `wintun.dll` (amd64), downloaded at setup time from wintun.net with a pinned SHA-256, placed next to `awdld.exe`, never committed. Loaded only from the executable's directory.

## Steps and acceptance gates (one hardware session per gate, each with written predictions)

| step | what | gate |
|---|---|---|
| M5.1 | `radiod serve` + `radio_link.h` + `radioctl` test client (MSVC) | offline: protocol unit tests; hardware: `radioctl` on channel 44 receives AWDL action frames from the iPhone (share sheet open) through the pipe with the right per-frame channel; a unicast TX through the pipe comes back with a TX status |
| M5.2 | random AWDL address | hardware: the firmware ACKs frames to the random address (`txtest --control` method with `cfg.mac` set) |
| M5.3 | `awdld` builds with MinGW; OWL's googletest suite and winawdl's C tests pass; offline replay of synthetic AWDL frames through the radio link | offline only |
| M5.4 | `radiod serve` + `awdld` + Wintun `awdl0` | hardware: logs show sync with the iPhone, peer valid; `ping` over `awdl0` link-local to the iPhone (TODO-VERIFY that iOS answers ICMPv6) – the M5 acceptance criterion of CLAUDE.md §7 |

## Consequences

- A MinGW-w64 toolchain is installed on the development machine (explicit step, `docs/DEV-SETUP.md`); CI gets a MinGW job for `awdld`.
- `awdld/` holds GPL-3.0-or-later code from winawdl and GPL-3.0 code from OWL; the project licence (GPL-3.0) already covers both.
- `radiod` stops being a one-shot test tool and becomes the radio server the `service` (M7) will start; its one-shot commands stay for diagnostics.
- Wintun installs its kernel driver on first use (signed by WireGuard LLC; no test mode needed).

## Open points

- Does iOS 26.5 keep sending unicast to a fixed-channel peer (M5.4 measures it)?
- Does the firmware ACK a locally administered address set through `DEV_INFO` (M5.2)?
- Unicast data rate without `FIX_RATE` (H14), visible in TXS during M5.4.
- Block ack (winawdl: iPhone asks for a BA agreement on TID 0 about once a second; ~2× throughput) – M9 throughput work, not M5.
