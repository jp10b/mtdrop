# Porting mtdrop to another Wi-Fi card

This guide is for anyone who wants mtdrop on a card other than the MediaTek MT7921: Intel, Realtek, Qualcomm/Atheros, Broadcom, the MT7922 and so on. It describes:
- what the radio has to do;
- how to find out cheaply whether a card can do it;
- what code to write;
- the test ladder the MT7921 port went through.

Every claim about the MT7921 below points to a numbered experiment in [`FINDINGS.md`](FINDINGS.md).

**Expect weeks, not days.** Windows vendor drivers do not expose raw 802.11 frames, so a port means driving the chip from your own code. The MT7921 port took ~40 experiments from the first chip-ID read to a working transfer.

## 1. What is chip-specific and what is not

```
 radio backend (chip-specific)          everything else (chip-independent)
 ┌───────────────────────────────┐
 │ Windows binding  (proxy/)     │      awdld     AWDL (OWL), awdl0 adapter
 │ chip library     (mt792x/)    │ ───▶ airdropd  mDNS, TLS, /Discover /Ask /Upload
 │ radio server     (radiod)     │ pipe app       tray, receiver list, prompts
 └───────────────────────────────┘      service   session, driver swap, watchdog
```

The boundary is the **radio link**: a message-mode named pipe with a small, documented protocol in [`driver/radiod/radio_link.h`](../driver/radiod/radio_link.h) (MIT, plain C11). A new card needs a radio server that speaks this protocol. `awdld` and everything above it do not change.

The chip-specific parts outside the radio backend are small:

| Where | What | MT7921 value |
|---|---|---|
| `driver/proxy/mtdrop_proxy.inf` | hardware ID the proxy binds to | `PCI\VEN_14C3&DEV_7961&CC_0280` |
| `service/Mtdrop.Service/ServiceConfig.cs` | hardware ID, instance prefix, vendor INF name, firmware file names | `mtkwl6ex.inf`, `WIFI_*_MT7961_*.bin` |
| `scripts/*.ps1` | `$HardwareMatch`, firmware names, vendor INF in the driver backup | same values |

If more cards are supported, these should become a per-card table in one place (good first contribution).

## 2. What the radio must be able to do

These are the requirements the MT7921 port actually ran into. Each one broke something when it was missing.

| # | Requirement | Why | MT7921 evidence |
|---|---|---|---|
| R1 | **Receive every frame** on a chosen channel (monitor/sniffer): AWDL action frames and data frames to the AWDL BSSID `00:25:00:ff:94:73`, with a correct per-frame channel and RSSI | AWDL sync and data | F-007, F-008 |
| R2 | **Date each received frame precisely** (radio TSF or a host clock read close to reception; error well under 1 ms) | OWL follows the peer's slot timing from these times. With frames dated up to 1.4 s late the iPhone sent no data at all | F-019, F-022, F-023 |
| R3 | **Transmit raw 802.11 frames** (action frames, data frames) from an address of our choice | AWDL beacons (PSF/MIF) and data | F-010, F-013 |
| R4 | **The chip ACKs unicast frames sent to that address** (a random, locally administered one) while in monitor mode | Without ACKs the iPhone retries and gives up | F-011, F-013, F-016 |
| R5 | **TX status per frame** (ACKed / not ACKed, retries) | OWL and the hand-over logic need it; a frame without status blocks the queue | F-014 |
| R6 | **Unicast data leaves within a few ms** at an OFDM rate | The MT7921's default data path waited 115–560 ms and used CCK 1 Mbit/s (useless on 5 GHz). Fixed with the ALTX queue + fixed rate, then station records | F-019, F-020, F-035 |
| R7 | **Channel switch in ~10 ms** (only needed if you hop; mtdrop stays on channel 44) | AWDL channel sequence | F-009, ADR-0002 |
| R8 | **No power saving** while active | Runtime PM silently stopped RX on Linux | CLAUDE.md §3 |
| R9 | **A clean stop that always works**, and giving the card back to the vendor driver | A missed interrupt or a forced driver switch hung Windows twice | F-011, F-032 |

Nice to have: TX on channels 44 and 149 at 5 GHz (no DFS), and 2.4 GHz channel 6. 40/80 MHz is not needed.

## 3. Step 0: prove the card on Linux first

Do not write Windows code before this works. The MT7921 project did it first (milestone M0, F-001).

1. Boot Linux from USB on the **same laptop**.
2. Check that the in-kernel driver supports monitor mode and injection: `iw list` (supported interface modes), then `aireplay-ng --test` or an injected probe request answered by an access point.
3. Run [OWL](https://github.com/seemoo-lab/owl) with [OpenDrop](https://github.com/seemoo-lab/opendrop). The iOS 26 patches are in [airdrop-mt7921](https://github.com/jedbillyb/airdrop-mt7921).
4. Open the share sheet on an iPhone set to "Everyone for 10 Minutes" and receive one photo.

If the photo arrives on Linux, the silicon and firmware can do it, and the Linux driver is your reference for every register. If it does not arrive, find out why on Linux, where the tools are far better, before going further.

Write down:
- the Linux driver and kernel version;
- the firmware files it loads (from `dmesg`);
- the PCI ID;
- whether the photo arrived.

## 4. Licences: what you may reuse

mtdrop is GPL-3.0-or-later. Before you port code from a Linux driver, read the SPDX line of **every file** you take:

- `BSD-*`, `ISC`, `MIT`, and dual licences with such an option (for example `GPL-2.0 OR BSD-3-Clause`): usable. Keep the notice and use the permissive option.
- `GPL-2.0-only` with no other option: **not compatible** with GPL-3.0. Learn from it, write your own code.
- No licence file at all (for example wrldwideee/win-drop): all rights reserved. Read only, or ask the author.

Record every reused file (path, upstream commit, licence) in [`third_party/NOTICE`](../third_party/NOTICE) and in the file header, as `driver/mt792x` does for mt76.

**Firmware is never committed.** Load it from the user's system or fetch it from its official source at install time ([`FIRMWARE.md`](FIRMWARE.md)). Check whether the **Windows** vendor firmware even accepts the open driver's commands. The MT7921's did not, so mtdrop uses linux-firmware (F-007).

## 5. Choose how to bind on Windows

| Approach | Used by | Pros | Cons |
|---|---|---|---|
| **UMDF proxy + user-mode chip library** (mtdrop) | mtdrop (MT7921), winawdl (RTL8852CE) | Crashes stay in user mode; the chip code is portable C and testable on Linux; no kernel debugging | DMA from user mode needs locked pages below 4 GiB and SeLockMemoryPrivilege (radiod runs as SYSTEM, F-005); polling instead of interrupts |
| **KMDF driver** | [AirDrop-for-Windows](https://github.com/21Suspect/AirDrop-for-Windows) (Intel AX211) | Real interrupts and DMA APIs | Kernel bugs crash Windows; needs test-signing or an EV/attestation signature |

The proxy in [`driver/proxy`](../driver/proxy) is mostly chip-independent:
- it maps BAR0 and runs batched register reads and writes;
- it keeps a **stop list** of register writes that it replays when the client's handle closes, so DMA stops however the client dies.

For a new card, change the INF hardware ID and write the card's own stop list ([`MT7921-BRINGUP.md`](MT7921-BRINGUP.md) §7.2 is the MT7921 one).

Known prior art per vendor:
- **Intel:** [AirDrop-for-Windows](https://github.com/21Suspect/AirDrop-for-Windows) (AX211, GPL-3.0).
- **Realtek:** [winawdl](https://github.com/raph559/winawdl) (RTL8852CE). Note: its `radio/` directory is GPL-2.0-only; the rest is MIT/GPL-3.0.
- **MT7922:** [win-drop](https://github.com/wrldwideee/win-drop) (no licence: learn only). On Linux it is in the same `mt792x` family as the MT7921, so `driver/mt792x` is the natural starting point.

See [`RESEARCH.md`](RESEARCH.md) for details.

## 6. Write the chip library

Follow the structure of [`driver/mt792x`](../driver/mt792x):

- **Portable C, no OS headers.** All host access goes through a small HAL ([`include/mt792x/hal.h`](../driver/mt792x/include/mt792x/hal.h)) for registers, DMA memory, sleep and the clock. The library then builds and unit-tests on Linux and in CI without hardware.
- **A written bring-up spec first.** [`MT7921-BRINGUP.md`](MT7921-BRINGUP.md) lists, from the Linux source, every register write, ring, command and firmware format in execution order, with file:line references. Write the same for your chip before code. It is what lets you review the code against the reference.
- **Staged bring-up.** Each stage can be run alone and always ends with an orderly shutdown:
  1. chip ID;
  2. memory;
  3. firmware download;
  4. post-boot commands;
  5. sniffer;
  6. TX.
- **Safety built in:** bounded polls, no unbounded loops, interrupts masked while the chip is ours, the stop list armed **before** DMA is enabled.

## 7. Speak the radio link

A radio server for the new card must:

1. create `\\.\pipe\mtdrop-radio` (SYSTEM + Administrators only) **before** touching the chip;
2. send `HELLO`: channel, band, its own address (the one the chip ACKs), maximum frame size;
3. forward received frames as `RX` with the radiotap header built by `rl_radiotap_build`:
   - TSFT is a host clock in µs, the same clock awdld uses (R2);
   - CHANNEL is the per-frame channel;
4. for every `TX`, send exactly one `TX_STATUS` (done/lost/refused, ACKed or not, transmissions), decide ACK and retries from address 1 (group frames: no ACK), and keep unicast data fast (R6);
5. handle `SET_CHANNEL`, `SET_BSSID_FILTER`, `STATS`, `HEARTBEAT`, and stop with `BYE` when the client is silent for `RL_LEASE_MS`.

The simplest route is a new backend inside `radiod serve` ([`serve.c`](../driver/radiod/serve.c)), which already does the pipe, filtering, radiotap, leases, TX backlog and statistics. `radiod serve --loopback` and `radioctl` test the pipe side without any hardware.

## 8. Test ladder

Run the steps in order and record each one as a FINDINGS entry: hypothesis, what would falsify it, method, raw result, conclusion. Write predictions **before** each run. These are the MT7921 acceptance criteria, which are a good default.

| Step | Command (MT7921) | Pass when | MT7921 |
|---|---|---|---|
| 1. Bind + read chip ID | `scripts/s1-chipid.ps1` | ID read, vendor driver restored | F-004 |
| 2. DMA memory | `scripts/s2-memory-probe.ps1` | locked blocks below 4 GiB | F-005 |
| 3. Firmware | `radiod bringup` | firmware ready | F-006 |
| 4. Sniff | `radiod sniff 1 8 …` | beacons with the correct per-frame channel | F-007 |
| 5. iPhone frames | `radiod sniff 44 …` with the share sheet open | AWDL action frames from the phone on 44 and 6 | F-008 |
| 6. Raw TX | `radiod txtest` | access points answer our probes | F-010 |
| 7. Unicast ACK + TX status | `radiod acktest` | AP frames ACKed, ghost frames NOT ACKed after all retries | F-013, F-014 |
| 8. Our random address is ACKed | `radiod txtest --mac random` (single and `--pair`) | fewer duplicate responses than the control | F-016 |
| 9. Pipe | `radiod serve` + `radioctl` | iPhone frames through the pipe, TX statuses intact | F-015 |
| 10. AWDL | `radiod serve` + `awdld` | peer found, sync followed, `ping` to the iPhone over `awdl0` | F-021 |
| 11. Receive | full stack | PC in the share sheet, photo intact | F-023 |
| 12. Send | full stack | photo delivered to the iPhone | F-028 |
| 13. Robustness | kill each process, restart, close the lid | Wi-Fi comes back every time, no hang | F-027 |

Measure:
- the RX dating error;
- the time from submit to TX status;
- the share of frames NOT ACKed;
- the transmissions per frame;
- the transfer rate.

The awdld `radio:` line and radiod's summary print all of them.

## 9. Safety rules (non-negotiable)

- Before the first driver swap:
  - create a System Restore point;
  - export the vendor driver;
  - save the BitLocker recovery key off the machine;
  - have a way to reach the internet without Wi-Fi.
- Every script that changes driver or adapter state prints its plan and its rollback, and changes nothing without `-Apply`.
- Give-back always removes the proxy package with `pnputil /delete-driver /uninstall`. Never switch a running device to another INF: on the MT7921 that hung Windows (F-032).
- Keep interrupts masked while the chip is yours (F-011, F-013).
- Never commit:
  - firmware;
  - captures;
  - TLS key logs;
  - private keys;
  - device names;
  - MAC addresses;
  - account identifiers.

## 10. Contributing a port

Open an issue first with:
- the card's PCI ID (`Get-PnpDevice -Class Net`);
- the Windows driver version;
- the results of step 0 on Linux.

Work in small, reviewable commits with conventional messages (`feat(driver): …`). Add an ADR in [`adr/`](adr/) for decisions that differ from the MT7921 port. Results go into `FINDINGS.md` with the card named in the entry title.
