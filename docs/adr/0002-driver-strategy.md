# ADR-0002: Driver strategy for the MT7921 radio

- **Status:** Accepted (2026-11-09, maintainer). No driver work starts before the safety checklist in CLAUDE.md §9 and `docs/DEV-SETUP.md` is complete.
- **Deciders:** maintainer
- **Inputs:** `docs/RESEARCH.md` §0, §3, §5, §8, §9; `docs/FINDINGS.md` F-001

## Context

AWDL needs three things the MediaTek Windows driver does not expose: raw 802.11 receive, raw transmit, and channel control. F-001 proved that the chip and its stock firmware can do all three under Linux. On Windows something of ours has to own the PCIe device while "Receive" is on.

What we know (FACT unless marked):

- The Linux driver logic we need is ~2.5–3.5 kLOC in mt76 (BSD-3-Clause-Clear): firmware download, MCU commands, DMA rings, sniffer mode, channel set, raw TX.
- Firmware acknowledgement of unicast frames addressed to our MAC decides whether a transfer works at all (OWL has no retransmission logic and sets no no-ACK flag) and how fast it is (winawdl: ~10 kB/s without, ~16 MB/s with).
- During a transfer the phone was on channel 44 in 15 of 16 slots; channel switch on Linux cost ~8 ms.
- MT7921 uses one interrupt, BAR0 only, and a **32-bit DMA mask**.
- Two Windows projects have done this for other chips: winawdl (Realtek; UMDF register proxy + user-space chip driver + user-space DMA; **Secure Boot on, no test mode**) and 21Suspect (Intel; full KMDF; test mode).
- This laptop: Secure Boot off, Device Encryption off, no Ethernet port, Windows 11 Home build 26200.

A short glossary for this document:

- **KMDF** – framework for kernel-mode drivers. Full hardware access; a bug can crash the machine (blue screen); the binary must be signed, or Windows must be in test-signing mode.
- **UMDF 2** – framework for drivers that run in a user-mode host process. A crash kills only that process. Can map device registers and receive interrupts; has **no DMA support**.
- **DMA** – the device reads and writes packet buffers in RAM by itself, using physical addresses we give it. A wrong address corrupts memory regardless of where our code runs.
- **MCU command** – a message to the chip's own processor (firmware), e.g. "switch to channel 44".

## Options

### A. UMDF 2 register proxy + chip driver in user mode (winawdl model)

A small UMDF driver binds to the device, maps BAR0 and relays register reads/writes (and interrupts) to an elevated user-mode process that contains the mt76-derived logic. Packet buffers are locked physical pages obtained from user mode.

- (+) No kernel code of ours → bugs in chip logic cannot blue-screen directly; ordinary debugger; fast edit-run cycle.
- (+) Works with Secure Boot on and without test mode; a locally trusted certificate is enough. Removes the EV-certificate/attestation problem from M9 for end users.
- (+) Proven end to end on the same Windows build (other chip).
- (−) Unproven on MediaTek. Open questions: enough lockable pages **below 4 GiB** (32-bit DMA) on a machine with fragmented memory; behaviour under Kernel DMA Protection; polling vs UMDF interrupt latency.
- (−) User-space DMA is still dangerous: a wrong physical address handed to the chip corrupts memory. An admin-level process effectively gains DMA.
- (−) Crash safety must be designed (replay a "stop DMA" sequence on handle close; not covered: host crash, sleep).
- (−) Needs `SeLockMemoryPrivilege` for the radio process.

### B. Full KMDF driver (21Suspect model, CLAUDE.md §6 original plan)

All chip logic in a kernel driver; user mode sees IOCTLs for RX, TX, channel.

- (+) Canonical model: proper DMA API (`WdfDmaEnabler`, 32-bit profile, common buffers), interrupts, power callbacks. IOMMU-safe.
- (+) Proven for a full AirDrop flow on Windows (other chip).
- (−) Every bug is a potential blue screen; debugging needs WinDbg and, ideally, a second machine over KDNET – which this laptop cannot do without a supported wired NIC.
- (−) Test-signing mode + Secure Boot off for development; attestation signing (EV certificate, paid) for any user distribution.
- (−) Largest amount of kernel code for a first driver project (6–10 kLOC estimated).

### C. Hybrid: thin KMDF driver (BAR mapping, interrupt, common buffers, DMA stop) + chip logic in user mode

- (+) Correct DMA allocation and a kernel-side guaranteed "stop DMA" on handle close; chip logic still debuggable in user mode.
- (+) Kernel part is small (~1 kLOC) and rarely changes.
- (−) Still needs test signing / attestation.
- (−) Register access by IOCTL round trips; needs batching for the firmware download.

### Rejected without further study

- Filter driver over the vendor driver: there is no monitor or injection path to filter.
- Vendor test-mode OIDs: undocumented, almost certainly insufficient.
- Own WDI/NDIS miniport replacing the vendor driver for normal Wi-Fi too: far too large.
- Patching the vendor `.sys`: breaks its signature and its licence.

## Decision

**Start with A, keep the chip logic independent of how registers and buffers are reached, and fall back to C (not B) if A fails on DMA.**

Concretely:

1. The mt76-derived logic lives in one portable C library, `driver/mt792x/`, that talks to hardware only through a small **HAL** (hardware abstraction layer): `reg_read32`, `reg_write32`, `dma_alloc`, `irq_wait`, `sleep`. It has no Windows headers.
2. The first HAL backend is the UMDF proxy (A). A second backend for a thin KMDF driver (C) can be added without touching chip logic.
3. Above the chip library sits the **radio interface** used by `awdld` (next section). `awdld` never sees which backend is underneath.
4. Bring-up proceeds in gated spikes. Each gate has a hardware-observable check; no step is called done on the strength of "no error returned" (lesson from win-drop).

| Spike | What | Observable check | Risk to the machine |
|---|---|---|---|
| S1 | UMDF proxy binds, read-only register access | `MT_HW_CHIPID` reads `0x7961`; ASIC revision reads `79610010` as under Linux (F-001) | Driver swap only; Wi-Fi off until rollback |
| S2 | Lockable pages below 4 GiB; ring setup without enabling DMA | Physical addresses < 4 GiB obtained repeatedly | None beyond S1 |
| S3 | Ownership handshake, WFSYS reset, firmware download | Firmware-ready bit; version string equals the vendor blob's build time | DMA enabled → memory-corruption risk if addresses are wrong |
| S4 | Sniffer mode on one channel, RX ring | Beacons from the home AP decode; per-frame channel matches | as S3 |
| S5 | Channel set + raw TX + ACK of unicast to our MAC | Second device in monitor mode sees our frame and the chip's ACKs | as S3 |
| S6 | Vendor driver rebinds without reboot after our teardown | Wi-Fi reconnects automatically, 10/10 cycles | – |

Decision points: if S2 fails (no low memory) or S3–S4 show DMA cannot be made reliable from user mode → switch the HAL backend to C. B is only reconsidered if both A and C fail.

5. **First target is a fixed channel (44), not hopping.** F-001 showed 15/16 slots on 44 during transfers. Hopping to channel 6 for slot 8 is added only if discovery proves unreliable without it. Whether the phone keeps sending unicast when we *advertise* a non-verbatim sequence is tested in M5 before committing (RESEARCH §9 item 2).

## Radio interface (chip-family independent)

This is the contract between `awdld` and any radio backend. It is what makes other chip families (MT7922/RZ616 first; Intel or Realtek by other people) a matter of adding a backend.

| Operation | Notes |
|---|---|
| `open(session parameters)` / `close()` | Lease-based: the backend stops DMA and firmware if the client disappears (handle close or missed heartbeat) |
| `get_caps()` | Chip id, firmware version, supported channels, own MAC, max frame size |
| `set_mac(addr)` | The address the firmware must ACK for |
| `set_channel(channel, width)` → actual latency | Explicitly programs the radio every time |
| `rx()` → `[metadata][802.11 frame]` | Metadata per frame: **channel actually received on**, RSSI, timestamp, FCS-ok, rate. `awdld` synthesises radiotap from it (OWL needs FLAGS and signal on every frame) |
| `tx(frame, rate, want_ack, retries)` → status | Honest status: acked / not acked / dropped |
| `set_rx_filter(bssid)` | AWDL BSSID filter in the backend to cut copies |
| `get_stats()` | Counters for diagnostics |

The lease/heartbeat idea and "no raw register or memory access above the backend" come from 21Suspect's IOCTL design; the message set mirrors winawdl's `radio_link.h` (MIT).

## Driver switching (applies to all options)

Following winawdl and 21Suspect (RESEARCH §8):

- Back up the vendor driver package first (`pnputil /export-driver`).
- Our INF matches the same hardware ID but is installed explicitly for the device; the vendor package stays in the driver store.
- Give back: remove our driver for the device, rescan, wait for the vendor driver, verify Wi-Fi connects.
- A SYSTEM scheduled task restores the vendor driver at boot/logon; a separate watchdog process restores it after the receive window or when the client dies.
- Never unload while "DMA stopped" is unverified; require a reboot in that case.
- Every command that changes driver state is shown to the maintainer with its rollback command before it runs (CLAUDE.md §9).

## Consequences

- CLAUDE.md §6 diagram changes: `driver/` becomes "thin UMDF proxy + user-mode chip library (`radiod`)" for development; the IOCTL boundary moves below the chip logic.
- Secure Boot can stay off on this machine, but nothing in option A depends on it. Test-signing mode is not needed for S1.
- Firmware is read from the vendor package already in the Windows DriverStore at run time. Nothing is redistributed. Whether those blobs load with the mt76 sequence is S3's question.
- Before S1 (first driver swap) the maintainer needs: a System Restore point, an exported copy of the vendor driver, and internet without Wi-Fi (USB-C Ethernet adapter recommended; a second phone's USB tethering as fallback).
- The estimate "6–10 kLOC of kernel C" no longer applies; the kernel/UMDF part is a few hundred lines, the portable chip library a few thousand.

## Open points (carried into `docs/FINDINGS.md` as experiments)

- How the firmware is told which MAC to ACK in sniffer mode (own-MAC entry via `DEV_INFO_UPDATE`? a station/BSS entry?). Mainline mt76 now marks MT792x `no_active_monitor`, yet kernel 7.0 offered it in F-001 – read the 7.0 source for what "active monitor" programs.
- UMDF interrupt support for a PCIe device with one MSI vector vs polling cost.
- Whether the vendor blobs in the DriverStore are byte-compatible with the mt76 download sequence.
- Bluetooth (separate USB function of the same chip) across a WFSYS reset.
