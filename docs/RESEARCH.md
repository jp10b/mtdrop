# Prior-art research

Status: first pass, 2026-11-08. Sources: shallow (`--depth 1`) clones in `third_party/` (git-ignored). Commit hashes below are the cloned HEADs. Line numbers refer to those commits (CR-stripped where the Windows checkout converted line endings).

Labels: **FACT** = read in source/docs at the given path; **ASSUMPTION** = our inference; **TODO-VERIFY** = needs a test or another source. Commit counts and issue trackers were not inspected (shallow clones) – TODO-VERIFY with `gh`.

---

## 0. Summary – what changes the plan

1. **A Secure-Boot-compatible driver path exists (winawdl).** On Windows 11 build 26200 – our exact build – `raph559/winawdl` runs AWDL+AirDrop end to end with a **UMDF 2 register proxy + user-space chip driver + AWE-based user-space DMA**, signed with a local certificate, **Secure Boot on, no test mode**. CLAUDE.md §2 assumed a kernel driver and Secure Boot off. ADR-0002 must evaluate this as a first-class option. Unproven on any MediaTek chip.
2. **Hardware/firmware ACK of unicast frames decides throughput, more than the driver model.** winawdl: 85 % retries / ~10 kB/s without ACK → ~0–4 % retries / ~16 MB/s with it. 21Suspect chose firmware IBSS mode for the same reason. The Linux MT7921 rig (~45 kB/s) depends on mt76 "active monitor". Our driver must make the MT7921 firmware ACK our AWDL MAC (likely: OMAC address = AWDL MAC). Top research question for M3/M4.
3. **Fixed-channel operation works on Windows (winawdl ch44, 21Suspect ch6)** but jedbillyb found `pin` breaks unicast from iOS 26 on Linux. Contradiction – TODO-VERIFY on our iPhone. If fixed channel works, M4/M5 get much simpler (no 10 ms hops every 64 TU).
4. **M0 kernel choice matters:** current mainline mt76 sets `no_active_monitor = true` for MT792x (`openwrt_mt76/mt792x_core.c:824`). The Linux reference rig needs active monitor → pick a kernel that still offers it and check `iw list` before anything else.
5. **Firmware does not need to be redistributed.** The installed MediaTek Windows package ships `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and `WIFI_RAM_CODE_MT7961_1.bin` (same names as linux-firmware) in the DriverStore. Load from there (or pass in via IOCTL). Compatibility of the vendor copies with the mt76 load sequence – TODO-VERIFY. Firmware licence forbids reverse engineering and is not GPL-compatible → never commit or ship blobs.
6. **`win-drop` (MT7922, closest on paper) is non-functional scaffolding** – wrong register map, faked firmware load, MCU timeouts reported as success, TX looped back into RX. Use mt76 as the source of truth, not win-drop.
7. **OWL's host interface is L2 (TAP), Wintun is L3.** The Windows backend must add/strip Ethernet headers and map EUI-64 link-local ↔ MAC. winawdl already does exactly this (`awdl/win/host_frame.c`, GPL-3.0-or-later).
8. **Stock OpenDrop is not viable on Windows** without what amounts to a rewrite. Windows-proven alternative: winawdl's stdlib-only Python AirDrop layer (MIT). Hardened Rust alternative: AirDrop-for-Everything (MIT), unvalidated over AWDL with iOS 26.

---

## 1. Licence overview

| Project | HEAD | Licence (FACT: file) | May we copy? |
|---|---|---|---|
| seemoo-lab/owl | 0ee3263 | GPL-3.0-or-later (`COPYING`, file headers) | Yes |
| jedbillyb/owl | 57b8678 | GPL-3.0 (`COPYING`) | Yes |
| seemoo-lab/opendrop | 11fe7ba | GPL-3.0 (`LICENSE`, `server.py:6-9`) | Yes |
| jedbillyb/airdrop-mt7921 | 51e705e | GPL-3.0 (`LICENSE`) | Yes |
| jedbillyb/mt7921-dual-channel | b06d010 | GPL-3.0 (`LICENSE`); kernel/hostapd patch files: TODO-VERIFY | Yes (repo), knowledge only needed |
| Foglio24/linux-airdrop | 6b1bf73 | GPL-3.0 app; kernel modules GPL-2.0/BSD | App yes; never the prebuilt `.ko`/binaries |
| raph559/winawdl | 0db7031 | **Mixed**: MIT (root, `driver/`, `app/`, `airdrop/`, `radio/radio_link.h`), **GPL-3.0-or-later** (`awdl/`), **GPL-2.0-only** (`radio/`, devourer submodule) | MIT + GPL-3 parts yes; GPL-2.0-only parts **no** |
| 21Suspect/AirDrop-for-Windows | 9ae8771 | GPL-3.0-or-later; provenance in `PROVENANCE.md` | Yes (Intel-specific code useless to us) |
| BenItBuhner/AirDrop-for-Everything | 34bdc8d | MIT; NOTICE mentions an unlicensed omdrop-owl influence | Yes, keep notice |
| wrldwideee/win-drop | e9be009 | **No LICENSE file**; SPDX MIT headers but no copyright holder or text | **No** – learn only; ask author |
| jiaqizou-msft/Airdrop-on-Windows | 675497b | None ("To be determined", `README.md:250-252`) | **No** |
| openwrt/mt76 | 0936617 | MT7921/connac/core files `SPDX BSD-3-Clause-Clear` (GPL-compatible per FSF list – TODO-VERIFY); some unrelated files GPL-2.0-only (mt76x0, mt7615 sdio/usb) | Reimplement; keep BSD notices if copying |
| MediaTek firmware | – | Binary licence: no reverse engineering; not GPL | **Never commit or redistribute** |

---

## 2. Linux, MT7921/MT7922 family

### 2.1 seemoo-lab/owl – upstream AWDL daemon

- **What:** user-space AWDL in C for Linux/macOS, "no longer maintained" (FACT: `README.md`). Requires active monitor + injection.
- **Structure (FACT):** `src/` platform-independent (`sync`, `election`, `channel`, `peers`, `rx`, `tx`, `frame`, `wire`, `schedule`, `state`); `daemon/` platform layer (`core.c` libev loop, `io.c` pcap + TAP, `netutils.c` nl80211/rtnetlink, `owl.c` main). Radiotap parser is a submodule (empty in clone).
- **How:** pcap on monitor iface, radiotap required (`daemon/io.c:80`), BPF `wlan addr3 00:25:00:ff:94:73` (AWDL BSSID, `io.c:86`, `src/frame.h:30`). Data frames → Ethernet → **TAP** (`io.c:144`), TAP MAC = WLAN MAC (`io.c:164-170`), MTU 1450 (`io.c:182`). Peers added as permanent NDP entries keyed on RFC 4291 link-local (`netutils.c:118-150`).
- **Reusable:** all of `src/*` (~3.5 kLOC), `daemon/core.c` as scheduler template, googletest tests.
- **Limitations (FACT):** static channel sequence (`src/state.c` ~58-60), no peer sequence adoption, fixed election metric (`src/election.h:26-27`), TX at fixed legacy 12 Mbit/s (`src/tx.c:309-312`), host monotonic clock instead of TSFT (`src/rx.c:474`), RX data array without bounds check (`core.c:113-119`).

### 2.2 jedbillyb/owl – fork

Changes vs upstream (FACT, diff):
- Channel-sequence strategies `-S verbatim|rotate|pin|widen|intersect`, `-W n` (`src/channel.{c,h}`, `daemon/owl.c`).
- Adopts the sync master's sequence on the 1 s tick (`daemon/core.c:366-560`). The fork README's claim that upstream already does this is wrong.
- `sync_offset` actually populated and signed (`src/rx.c`, `src/peers.h`); peer sequence decoded with `peer->sequence_enc` (`src/schedule.c`).
- Channel 36 accepted (`CHAN_OPCLASS_36`).
- Asynchronous `set_channel()` via non-blocking netlink; each switch still blocks ~10 ms (median 10.6 ms over 1038 switches on MT7922, comment `core.c:315-321`).
- RX bounds check, random LAA MAC for pcap replay, 10 s `STATS` line, new tests (`test_awdl_channel/rx/txgate.cpp`).
- Peer timeout is really 2 s with a 1 s sweep (`src/peers.c:28-33`).

The MT7921 power-management/monitor workarounds are in `airdrop.sh`, not in the fork. **Reuse:** strategies + tests, `sync_offset` and encoding fixes, bounds check.

### 2.3 jedbillyb/airdrop-mt7921 – primary reference

- **What:** bash/Python tooling (`airdrop.sh`, 1082 lines), daemon `daemon/airdropd`, `install.sh`, diagnostic harnesses in `tools/` (`bursts.py`, `slotmap.py`, `txarms.sh`, `blewake*`), and `docs/FINDINGS.md` (§1–§50, ~3500 lines, includes retractions). OpenDrop fixes now live in fork `jedbillyb/opendrop` (not cloned) – `patches/README.md:1-20`.
- **Flow (FACT):** monitor pair + PM off (`airdrop.sh:388-410`) → channel sweep counting AWDL frames (`:442-555`) → `owl -i <active vif> -c CHAN -N -S verbatim -vv` (`:597`) → `awdl0` → ping6 (`:651-716`) → OpenDrop with captures. Network restore via bash trap + `setsid` watchdog.
- **Status (FACT, README):** receive + send work on MT7921 (Void) and MT7922 (Arch) with iOS 26; ~40–67 kB/s; Wi-Fi off during transfer in exclusive mode; Everyone mode only.
- **Open issues (FACT):** tail lost in P2P-GO mode (99.1 %), phone narrows sequence mid-transfer, no roam detection, daemon send path unproven on MT7921. OpenDrop pin differs between `install.sh:28` (`787f694…`) and README (`03b4adc…`) – TODO-VERIFY which is current.

### 2.4 jedbillyb/mt7921-dual-channel

- **What:** Wi-Fi + AWDL on one MT7921, stock kernel. A P2P-GO interface gets its own channel context (only that iface combination allows 2 channels); hostapd patched to keep P2P-GO type; monitor TX borrows the GO's channel context when the monitor MAC equals the GO MAC (cites `net/mac80211/tx.c:~2377`).
- **Negative result (FACT):** kernel patches giving the monitor its own channel context failed – firmware received only the AP channel (593/593 frames, `HANDOFF.md:175-194`). Firmware CNM time-slices GO/Wi-Fi Direct roles; sniffer is not part of it (`HANDOFF.md:182-188`, FINDINGS §45).
- **Cost (FACT):** uplink latency ~2.6 ms → ~40 ms median, 280–590 ms worst.
- **Use for us:** knowledge for M9. ASSUMPTION: our own driver could request a GO-type second channel context from firmware.

### 2.5 Foglio24/linux-airdrop

- **What:** GTK 4 app (Italian UI), privileged bridge, three OWL variants (`owl-modern` = jedbillyb/owl `832d70f` + omdrop patches, `owl-nexa` rewrite, original), hardened OpenDrop (`secure_receiver.py`, 17 patches), `nexaawdl/` passive AWDL decoder/analyser, `tests/awdl_link_emulator.py`.
- **How:** Wi-Fi Direct group pins a fixed channel (6 or 44); passive + active monitor take the group MAC; `owl -i ldack0 -c <ch> -N -S intersect -E -F [-A] [-R]`. Uses stock driver if kernel offers active monitor + P2P-GO, else loads patched modules built for Ubuntu 7.3.0-9 (`backend/driver-compat/`, incl. `mt7922-active-monitor.patch`, `mt7922-pci-injected-rate.patch`).
- **Results (FACT, `docs/SENDING.md`):** MT7922 on ch44: 2.85 MB in ~12 s, 1.5 MB in 3.3 s – 3–4× faster than ch6, and far above the ~45 kB/s of the jedbillyb rig. **Tested only on MT7922** (iOS 27, iPad).
- **Reusable:** `nexaawdl` for capture regression fixtures, link emulator, receiver hardening (`secure_receiver.py`: consent, Ask/Upload binding by peer + TransferID `:137-158`, own dvzip decoder `:409-470`, own cpio extractor `:490-596`). Hardening is POSIX-only (`os.O_NOFOLLOW` `:558`, `if_nametoindex` `:733`).

---

## 3. Windows prior art

### 3.1 raph559/winawdl (Realtek RTL8852CE) – most relevant

- **Proven (FACT, README / DESIGN-NOTES):** receive ~16 MB/s, send 9–14 MB/s, app + installer + give-back work, Windows 11 build 26200. One PC, one card, one iPhone (iOS version not stated).
- **Driver model:** UMDF 2 driver `driver/regproxy/regproxy.c` (265 lines) maps BAR2 and relays batched register R/W via IOCTL (`GET_INFO`, `BATCH`, `SET_CLOSE_OPS`). INF: `UmdfDirectHardwareAccess`, `RegisterAccessUsingUserModeMapping`, SDDL SYSTEM+Admins. Chip driven in user space by a fork of OpenIPC **devourer** (GPL-2.0).
- **User-space DMA:** locked 2 MiB pages with known PFNs via `AllocateUserPhysicalPages2` + `MEM_EXTENDED_PARAMETER_NONPAGED_LARGE`; needs `SeLockMemoryPrivilege`. Works with Kernel DMA Protection + VBS on. Polling, ~31 µs per register round trip.
- **Crash safety:** stored "stop DMA" register sequence replayed on handle close (incl. process kill). Not covered: driver-host crash, sleep/hibernate.
- **Signing:** self-made cert into LocalMachine Root + TrustedPublisher, catalog signed, private key deleted. **Works with Secure Boot on, no test mode** (UMDF).
- **Bind/unbind (FACT, `app/src/Radio/WifiCard.cs:146-188`, `installer/install.ps1:242-247`):** generic-ID INF (vendor still wins by default) → `pnputil /add-driver` + forced `UpdateDriverForPlugAndPlayDevicesW`; give back with `pnputil /delete-driver /uninstall /force` + `/scan-devices` (wait ≤20 s); Wi-Fi profiles carried over (per-adapter GUID); SYSTEM scheduled task restores on boot/logon.
- **OWL on Windows:** unmodified OWL submodule compiled with MinGW against shims in `awdl/compat/` (BSD headers, libev subset, pcap); replaced `io.c`/`netutils.c` with `awdl/win/io_win.c`, `radio_client.c` (named pipe to radio daemon, protocol `radio/radio_link.h`: RX, TX, SET_CHANNEL, BSSID filter, address), `ev_win.c` (high-res timers), `netutils_win.c`.
- **Wintun:** `awdl/win/host_if.c` creates `awdl0`; `host_frame.c` converts Ethernet ↔ IP, derives MAC from EUI-64 (no ND needed); strips Windows' random link-local; MTU 1450.
- **AWDL fixes worth taking:** `frame_fixup.c` (clear OWL's 40 MHz HT cap bits; trim trailing zeros from iPhone MIF – otherwise peer never valid), `tcp_merge.c`, `service_bridge.c` (DNS-SD records inside AWDL action frames → mDNS).
- **AirDrop layer:** stdlib-only Python (`airdrop/mdns.py`, `receiver.py`, `cpio.py`), adapted from omdrop (MIT). Announces on `awdl0` every 2 s + NSEC; handles uploads that never send the final 0-chunk by detecting a complete CPIO.
- **Channel:** fixed, default 44, no hopping (`scripts/awdl-run.ps1:18`). **Hardware ACK critical** (DESIGN-NOTES "Acknowledging frames").
- **Risks:** admin process can aim DMA anywhere; low-memory fragmentation (after ~13 days only one free low 2 MiB block); lock-pages privilege.
- **Reuse:** MIT – regproxy pattern, `WifiCard.cs`, installer/scheduled task, `airdrop/*.py`, `radio_link.h`. GPL-3.0+ – all of `awdl/` (≈ our `awdld/` backend). **Not** `radio/`/devourer (GPL-2.0-only, Realtek).

### 3.2 21Suspect/AirDrop-for-Windows (Intel AX211)

- **Proven (FACT, `WINDOWS-STATUS.md`):** iPhone→PC (photos, video), PC→iPhone, Mac→PC; ~6 MB in 28 s (~215 kB/s); owner-reported iOS 27.2. One exact device ID.
- **Driver:** KMDF (~5.7 kLOC incl. tools), WDF common buffers, **MSI-X ISR** (polling-only firmware boot timed out), INF `DmaRemappingCompatible=1`. Firmware: OpenBSD `iwx`, downloaded with hash checks.
- **Radio mode:** firmware **IBSS on HT20 ch6** (firmware does ACKs/beacons); pure monitor saw no AWDL peer. Fixed MAC, legacy OFDM rates.
- **IOCTL design (good model):** lease-based `SESSION_START`/`SESSION_EXCHANGE` (batched RX/TX, 1 ms timer)/`SESSION_STOP`; no raw register/memory access; 10 s heartbeat lease and handle close both stop firmware + DMA.
- **Signing:** test mode + Secure Boot off.
- **Switching (FACT, `native/intel/Test-IntelDriver.ps1:100-160, 270-330`):** `pnputil /export-driver` backup, forced install, **separate watchdog process** restores after 180 s, verifies INF path + ProblemCode, **refuses to unload while DMA stop is unverified** (requires reboot, detected via boot time), named mutex, temp cert removed.
- **AWDL:** Owly (Rust, MIT), not OWL. Wintun official DLL. AirDrop: OpenDrop + `opendrop/windows/*`, python-zeroconf; receiver unelevated, elevated helper owns the radio.
- **Reuse (GPL-3.0+):** watchdog/restore state machine, lease/heartbeat KMDF session design, `opendrop/windows/*`, provenance discipline.

### 3.3 wrldwideee/win-drop (MT7922) – do not build on it

- KMDF driver with 4 IOCTLs (`SET_CHANNEL`, `INJECT_FRAME`, `READ_FRAMES`, `GET_STATUS`), Rust daemon (own AWDL, tokio, mdns-sd, rustls, Wintun), PowerShell setup with test signing. Ships prebuilt `.sys`/`.exe`/`wdksetup.exe` – **never run them**.
- **Evidence it does not work (FACT, vs mt76):** TX ring at `0x4000+0x300` vs mt76 `0xd4300`; chip ID at `0x88000000+0x8F0` vs `0x70010200`; invented 32-byte descriptor vs mt76 16-byte `mt76_desc` (`dma.h:120`); FW_SCATTER CID `0x72` vs `0xee`; patch semaphore stubbed to succeed (`mt7922_fw.c:209-222`); `PATCH_FINISH` 0x70 vs 0x07; hard-coded FW version; MCU timeouts return `STATUS_SUCCESS`; fixed RSSI −70; INJECT loops frames back into RX FIFO with "simulated RSSI" (`mt7922_ioctl.c:94-141`); Wintun mock mode (`tun.rs:126`).
- **Lesson:** a KMDF port without hardware validation can look complete while doing nothing. Every bring-up step in our driver must have a hardware-observable check. Still worth contacting the author about MT792x collaboration.

### 3.4 jiaqizou-msft/Airdrop-on-Windows – counter-example

C#/.NET + WinRT Wi-Fi Direct, BLE adverts, Kestrel HTTP/2-only server. Wi-Fi Direct is not AWDL → never appears in the share sheet (ASSUMPTION consistent with CLAUDE.md). Placeholders in connection setup, no tests, no licence. Nothing reusable.

---

## 4. Protocol layer above AWDL

### 4.1 seemoo-lab/opendrop

- v0.13.0, ~1.3 kLOC, last commit 2026-08-17; `setup.py` lists macOS/Linux, Python 3.6–3.9 (FACT). Upstream in maintenance mode; iOS 26 work lives in forks (ASSUMPTION).
- **Wrong for iOS 26 (FACT):** `int(Content-Length)` on chunked `/Discover`/`/Ask` (`server.py:166,225`); `/Upload` accepts only `x-cpio` (406 otherwise, `:246-255`) while iOS 26 sends `x-dvzip`; single-threaded `HTTPServer` (`:128-129`); `/Ask` always 200 (no consent); libarchive extract into CWD with no secure flags (`:293-304`).
- **Windows blockers:** interface lookup by `ifaddr` name `awdl0` (on Windows `name` is a GUID; `util.py:305-309`); libarchive-c needs native `archive.dll`; certificate generated by shelling out to `openssl` (`config.py:130-155`); deprecated `pkg_resources`; iOS 26 re-announce patch uses zeroconf private API; Foglio hardening POSIX-only.
- **Reusable as reference:** flag bit table (`config.py:32-51`), `/Discover` and `/Ask` response dicts (`server.py:204-243`), sender `/Ask` body (`client.py:147-190`).

### 4.2 BenItBuhner/AirDrop-for-Everything (Rust, MIT)

- v0.2.0, last commit 2026-10-24, no CI. Synchronous Rust; `airdrop-core` ~3.9 kLOC: mdns-sd, rustls+rcgen, hand-rolled HTTP/1.1 with strict chunked parsing, plist, dvzip + newc/ODC cpio codecs with tests, WinRT/hcitool BLE, CLI, egui GUI, C ABI.
- **Windows:** builds; only native dep is the `windows` crate. AWDL listed as "no driver exists" → **never validated against iOS 26 over AWDL**.
- **Defects found (FACT):** looks up header `transfer-id` but iOS sends `TransferID` → `transferid` (`server.rs:614`, `http.rs:166`); reads the whole chunked body before replying with 30 s idle timeout (`server.rs:626-650`, `:32`) → likely 408 because iOS withholds the final 0-chunk (ASSUMPTION, high impact); `/Upload` not bound to an accepted `/Ask` in Everyone mode; mDNS not interface-restricted and no periodic re-announce; ECDSA P-256 self-signed key (iOS acceptance TODO-VERIFY; RSA-2048 is proven); sender uses newc cpio, 64 KiB dvzip blocks and a `Host` header where Apple uses ODC, 128 KiB, no Host.
- **Reusable:** codecs + tests, safe-name / no-clobber / disk-budget logic (`server.rs:791-930`), WinRT BLE advertiser (`ble.rs:115-147`), sender with TransferID.

### 4.3 iOS 26 behaviour (jedbillyb patch series, FACT: `patches/README.md`)

Receiving: chunked `/Discover`+`/Ask`; accept `x-dvzip` (4-byte BE block header, bit 31 = stored else zlib, 128 KiB blocks → ODC cpio); `SO_RCVBUF` + `100-continue` only when ready; threaded server (idle keep-alive blocked `accept()`); mDNS re-announce every 5 s; links arrive only in `/Ask` `Items`; salvage partial uploads; **end upload at `TRAILER!!!` + `TotalBytes` without waiting for the withheld 0-chunk**.
Sending: `TransferID` in `/Ask` repeated in `/Upload`; Apple header set/order without `Host`; small (~45 KB, 360 px) thumbnail; PTR re-query every 1.5 s; multi-file; stall timeouts. Diagnostic: `SSLKEYLOGFILE`.

Details belong in `docs/PROTOCOL.md` (next deliverable).

---

## 5. mt76 / MT7921 – minimal driver path

Reference: `openwrt_mt76` @ `0936617d`. Scope: firmware load, MCU + DMA init, 20 MHz channel set, monitor RX, raw TX, stay awake.

- **Probe (FACT, `mt7921/pci.c:259`):** device `0x7961` (also MT7920 – detected by `MT_HW_BOUND 0x70010020` bit 7, TODO-VERIFY on our board); one IRQ vector (MSI or INTx, shared); **32-bit DMA mask** (`pci.c:330`); only **BAR0** mapped. Register remap `__mt7921_reg_addr` (`pci.c:70`): direct < `0x100000`, fixed table (e.g. `0x7c060000→0xe0000`), sliding **L1 window** (`MT_HIF_REMAP_L1`, `mt7921.h:232`) for `0x18…`, `0x70…`, `0x7c…` ranges – shared mutable state, must be serialised on Windows. WFDMA base `0xd4000`.
- **Ownership handshake:** FW-own (`HOST_SET_OWN`, poll bit 2) then driver-own (`HOST_CLR_OWN`, poll 0) on `MT_CONN_ON_LPCTL 0x7c060010` (`mt792x_core.c:1040,1085`). Chip ID `MT_HW_CHIPID 0x70010200`. **WFSYS reset**: `0x18000140` bit 0 low 50 ms → high, poll bit 4 ≤500 ms (`mt792x_dma.c:603`).
- **Firmware (FACT, `mt792x.h:48,57`):** `WIFI_MT7961_patch_mcu_1_2_hdr.bin` (ROM patch) + `WIFI_RAM_CODE_MT7961_1.bin` (no separate WA). Sequence (`mt792x_core.c:1112`): `NIC_POWER_CTRL` → poll `FW_PWR_ON` → patch (`PATCH_SEM_CONTROL 0x10`, `PATCH_START_REQ 0x05`, `FW_SCATTER 0xee` ≤4 KiB chunks on **TX ring 16**, `PATCH_FINISH_REQ 0x07`) → RAM (`TARGET_ADDRESS_LEN_REQ 0x01`, scatter, `FW_START_REQ 0x02`) → poll `FW_N9_RDY` ≤1.5 s. Then `GET_NIC_CAPAB`, CLC, thermal, FW log.
  - Vendor DriverStore copies: `mtkwl6ex.inf_amd64_a26835a46a3adbed` (patch 108 576 B, RAM 673 516 B, 2022) and `…_e4e766c9f7adc16a` (RAM 764 232 B, 2025-09); hashes differ from mt76/linux-firmware copies. Compatibility TODO-VERIFY.
- **MCU interface:** legacy or UNI TXD header (`mt76_connac_mcu.h:55,103`), 4-bit sequence 1..15, commands on **TX ring 17**; events on RX ring 0 / 4; 3 s timeout → full reset in mt76.
- **Minimal MCU command set:** `NIC_POWER_CTRL`, patch/RAM download, `GET_NIC_CAPAB 0x8a`, `EFUSE_BUFFER_MODE 0x21`, `MAC_INIT_CTRL 0x46`, `SET_CHAN_DOMAIN 0x0f`, `SET_RX_PATH 0x4e` / `CHANNEL_SWITCH 0x08`, `SET_RATE_TX_POWER 0x5d`, UNI `DEV_INFO_UPDATE`/`BSS_INFO_UPDATE` (own MAC), UNI `SNIFFER 0x24` (tag 0 enable, tag 1 channel), `SET_RX_FILTER 0x0a`, `CHIP_CONFIG "KeepFullPwr 1"`.
- **DMA rings (FACT, `pci.c:171-250`):** 16-byte `mt76_desc`; TX0 (2048, all data+mgmt), TX16 (FW download), TX17 (MCU), RX0 (MCU events), RX2 (1536, frames), RX4 (events/TX-free after FW download – TODO-VERIFY). 2048 B RX buffers.
- **RX parsing (`mac.c:169`):** base RXD 6 dwords (FCS/ICV error, `HDR_TRANS`, pad, `RXD3.CH_FREQ` = per-frame channel number → "trust per-frame frequency"); optional groups 4/1/2 (TSF)/3 (P-RXV rate, RCPI)/5 (C-RXV). RSSI = `(RCPI − 220)/2`. Must ensure raw 802.11 headers (no RX header translation) and that groups 2/3/5 are attached – TODO-VERIFY.
- **Monitor:** monitor gets a real firmware vif (`WANT_MONITOR_VIF`); enable = `mt7921_mcu_set_sniffer` + **PM and deep sleep off** + beacon filter off (`main.c:603`); RX filter `FCSFAIL|CONTROL|OTHER_BSS`. Channel: `CHANNEL_SWITCH` (non-CNM firmware) or `config_sniffer` tag 1 (CNM). `mt792x_assign_vif_chanctx` does not program the radio – matches the Linux "in-place switch doesn't retune" finding. Our driver must explicitly program the channel on every hop; which command MT7961 firmware honours and the per-hop latency – TODO-VERIFY.
- **Raw TX:** TXWI (32 B TXD + HW TXP) in ring, frame fetched via TXP pointers (cut-through); **token table** (≤8192), completion via `TXRX_NOTIFY` on RX ring, not descriptor DONE. TXD: mgmt to `ALTX0`, 802.11 header format, `FIX_RATE`, `REM_TX_COUNT 15`, `NO_ACK` only on request, injected SN. wcid 0 + one OMAC presumably enough (ASSUMPTION). Set the rate explicitly on every frame (mt76 ignored injected radiotap rate on MT7922 – Foglio patch).
- **ACK for received unicast:** TODO-VERIFY – likely requires OMAC address (`DEV_INFO.omac_addr`) = AWDL MAC. mainline sets `no_active_monitor` for MT792x (`mt792x_core.c:824`).
- **Power:** never issue `SET_OWN` after init, send `KeepFullPwr 1`, don't send PM profile commands, keep `L0S_DIS`/ASPM off, stay in D0 (no S0 idle). Log `MCU_EVENT_LP_INFO` as a canary.
- **Teardown for vendor rebind:** mask IRQs → stop DMA + poll idle → reclaim → WFSYS reset → optionally `SET_OWN` (mirrors `mt7921e_unregister_device`, `pci.c:44`). mt76 probe itself tolerates leftover state. Vendor rebind without reboot – TODO-VERIFY empirically (fallback: FLR / D3cold, support unknown).
- **Size (ASSUMPTION):** ~2.5–3.5 kLOC of relevant Linux logic → 6–10 kLOC KMDF C incl. INF, IOCTLs, logging. Hardest: FW download handshake without visibility, WFDMA prefetch/GLO_CFG, fast reliable channel switch, token TX completion, RXD groups, clean rebind.
- **KMDF mapping:** BAR → `MmMapIoSpaceEx` in `EvtDevicePrepareHardware`; IRQ → `WdfInterruptCreate` (ISR at DIRQL only masks/acks, DPC at DISPATCH walks rings with budget); `WdfDmaEnabler` **32-bit profile** + `WdfCommonBuffer` for rings, RX buffers and a TX bounce pool (copying is fine at our rates); MCU commands at PASSIVE under `WDFWAITLOCK` waiting on `KEVENT` signalled from the DPC; `WdfWorkItem`/`WdfTimer`; firmware read via `ZwReadFile` or passed in by user mode; no mac80211 port – user mode gets `[metadata][802.11 frame]`.
- **Bluetooth:** PCIe WFSYS reset touches only the WF subsystem (by name); BT is a separate USB function here. ASSUMPTION: BT survives; TODO-VERIFY (shared CONN_INFRA). Linux saw a BLE advert killed by a Wi-Fi reset on the exclusive path (FINDINGS §36, §39).

---

## 6. OWL platform abstraction – what our Windows backend implements

Replace `daemon/io.{c,h}`, `daemon/netutils.{c,h}` and the libev glue; keep `src/*`.

| Concern | OWL (Linux) | Windows backend needs |
|---|---|---|
| Raw RX | pcap fd, radiotap required, addr3 filter (`io.c:41-105`) → `awdl_rx()` | Driver delivers frames + metadata; we synthesise radiotap (FLAGS incl. bad-FCS, DBM_ANTSIGNAL, TSFT). RSSI < −65 dBm dropped by default (`src/state.h:32-33`) – Foglio saw this reject 9.4 % of iPhone action frames. Filter addr3 + own TX in driver (ASSUMPTION: cheaper). |
| Raw TX | `wlan_send()` → `pcap_inject`, radiotap RATE only (`io.c:361-371`) | IOCTL TX with explicit rate, ACK/no-ACK flag, retries; honest TX status. |
| Channel | `set_channel()` nl80211 (`netutils.c:391-417`), called each AW boundary (`core.c:279-308`) | IOCTL; measure latency vs 64 TU (65.5 ms) slot. Linux: ~8.5–10.6 ms per switch. |
| Host iface | TAP (L2), MTU 1450, MAC = WLAN MAC, permanent NDP entries | Wintun (L3): add/strip Ethernet header, EUI-64 ↔ MAC, `ff02::fb` → `33:33:00:00:00:fb` – already in winawdl `host_frame.c`. |
| Event loop | libev `ev_io` + timers (chan, peer 1 s, psf, mif, tx, tx_mcast) | IOCP / waitable high-res timers (winawdl `ev_win.c`). |
| POSIX bits | libpcap, libnl, `/dev/net/tun`, `CLOCK_MONOTONIC`, `<endian.h>`, `<netinet/*>`, packed attrs, getopt, fork, SIGUSR1 | winawdl `awdl/compat/*` shims (GPL-3.0+) – review and reuse. |

---

## 7. Findings from prior work that constrain our design

- **Power management** silently kills monitor RX (FINDINGS §9). Keep chip awake for the whole session.
- **Only per-frame frequency is trustworthy** (§12). Driver must report the channel per received frame (`RXD3.CH_FREQ`).
- **Channel strategy:** `verbatim` works, `pin` broke iOS 26 unicast (§25), `widen` accepted up to 16/16 (§26); phone widens its sequence during transfers (§21, §47); slot 8 = ch6 in all captures; channel 36 usage disputed (jedbillyb saw it, Foglio says never). Windows projects work with a fixed channel – reconcile on our iPhone.
- **Throughput:** ~45 kB/s limited by slots offered (§16, §18, §21); 73 kB/s with intersect/P2P-GO (§47); MT7922 on fixed ch44 behind a GO: ~240 kB/s+ (Foglio); winawdl ~16 MB/s with hardware ACK.
- **TX:** request ACK + retries for unicast, none for multicast; without radio ACKs Foglio's sends stalled at 192 kB; honour per-frame fixed rate; injection with non-matching MAC while associated is silently dropped (§46) – driver must report TX status honestly.
- **Tail loss:** fixed with 1 s TCP keepalive on the upload socket (Foglio `SENDING.md`).
- **mDNS must bind to the `awdl0` interface index**, not by address (shared link-local addresses caused wrong-interface announcements).
- **iPhone:** share sheet must be open to receive (§19); BLE type-0x05 advert to send (§27).

---

## 8. Inputs for decisions

- **ADR-0001 (`airdropd`):** candidates (A) patched OpenDrop on Windows – rejected-leaning (rewrite in practice); (B) Rust on AirDrop-for-Everything with iOS 26 fixes ported; (C) C#; (D) winawdl's stdlib-Python layer (MIT, Windows-proven). Recommendation to be written in the ADR.
- **ADR-0002 (driver):** (A) UMDF register proxy + user-space MT7921 driver (Secure Boot on; 32-bit DMA → low-memory AWE pages; polling unproven for MT7921); (B) full KMDF (test mode; 21Suspect proves feasibility); (C) hybrid thin KMDF (common buffers + IRQ + DMA stop) with mt76 logic in user mode. Rejected-leaning: NDIS LWF over vendor driver (no monitor/injection to filter), vendor ATE OIDs (unknown, likely insufficient), own WDI miniport (too large), patching vendor `.sys` (signature + legal).
  - **Proposed cheap, reversible spike before deciding:** bind a UMDF proxy to `PCI\VEN_14C3&DEV_7961`, read `MT_HW_CHIPID` (0x70010200) through the L1 window, check availability of AWE pages below 4 GiB. Requires a driver swap → maintainer confirmation per CLAUDE.md §9.
- **Driver switching:** solved prior art (winawdl + 21Suspect): generic-ID INF, `pnputil /export-driver` backup, forced `UpdateDriverForPlugAndPlayDevices`, give back via `pnputil /delete-driver /uninstall /force` + `/scan-devices`, SYSTEM startup task + separate watchdog, Wi-Fi profile carry-over, refuse to unload while DMA stop unverified.

---

## 9. Open TODO-VERIFY list (prioritised)

1. Does our M0 kernel offer active monitor for MT7921 (`iw list | grep -i "active monitor"`)? Which mainline release introduced `no_active_monitor` for MT792x?
2. Does iOS 26.5 keep sending unicast to a fixed-channel peer (pin vs winawdl/21Suspect behaviour)?
3. How to make MT7921 firmware ACK our AWDL MAC (OMAC = AWDL MAC? active monitor flag mapping?).
4. Do vendor DriverStore firmware blobs load with the mt76 sequence?
5. Can MT7921 run polled (no IRQ) for the UMDF path? Enough AWE memory below 4 GiB?
6. Does `mtkwl6ex.sys` rebind cleanly without reboot after our driver unloads?
7. Which channel command does MT7961 firmware honour in monitor mode, and per-hop latency?
8. Does BT (USB function) survive a WFSYS reset?
9. Does iOS accept an ECDSA self-signed cert (only relevant if we use AfE as-is)?
10. Which `jedbillyb/opendrop` pin is current (`787f694` vs `03b4adc`)?
