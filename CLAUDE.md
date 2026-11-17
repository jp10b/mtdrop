# CLAUDE.md – Project context for Claude Code

> Working name: **mtdrop** (placeholder – check GitHub for name collisions before publishing; `windrop`, `win-drop` and `winawdl` are already taken).
> Do NOT use "AirDrop" as the project name or in a logo – it is an Apple trademark. Describe the project as "AirDrop-compatible".

---

## 0. How to work with the maintainer

- The maintainer communicates in **Polish**. Reply in Polish. Code, comments, commit messages and public docs (README, docs/) are in **English** (open-source audience).
- The maintainer is ambitious and ready to learn, but kernel/driver development may be new to them. When you introduce a non-obvious concept (NDIS, KMDF, IRQL, DMA rings, radiotap, AWDL sync), explain it briefly the first time.
- Research before code. This project sits on top of reverse-engineered, poorly documented protocols. Read prior art (section 5) before designing anything.
- Separate **FACT** (verified in source code, a capture or official docs), **ASSUMPTION** (plausible, not yet verified) and **TODO-VERIFY**. Never present an assumption as fact. Prior projects in this space retracted several confident conclusions – see `jedbillyb/airdrop-mt7921/docs/FINDINGS.md`.
- Anything that needs a physical action (iPhone in hand, rebooting, changing BIOS/UEFI settings, router settings) – ask the maintainer and give exact step-by-step instructions.

---

## 1. Goal

Make a Windows laptop with a **built-in MediaTek MT7921** Wi-Fi card appear in the **native iPhone AirDrop share sheet**, and receive photos from it – exactly like a MacBook does.

### Target user story (v0.1)

1. On the PC, the user clicks "Receive" in the tray app (the PC becomes visible for a limited time).
2. On the iPhone, the user opens Photos, taps Share, taps AirDrop.
3. The PC (e.g. "DESKTOP-ABC") appears in the AirDrop sheet next to Apple devices.
4. The user taps it. The iPhone sends.
5. On Windows, a toast notification appears: "iPhone of X wants to send 1 photo – Accept / Decline".
6. On Accept, the photo lands in `%USERPROFILE%\Downloads` (configurable). The toast offers "Open folder".

### Hard requirements

- **No app on the iPhone.** Only the native share sheet.
- **No dongle, no extra hardware.** Built-in MT7921 only.
- **No Linux VM / WSL** in the final product. Native Windows 11.
- Open source, **GPL-3.0** (forced anyway by OWL / OpenDrop if we reuse them).

### Non-goals (for now)

- "Contacts Only" AirDrop mode. It requires an Apple-signed validation record and cannot be forged. We support **"Everyone for 10 Minutes"** only. This is a permanent limitation, state it in the README.
- macOS, Linux, Android targets. Other Wi-Fi chipsets (MT7922 may come later – same driver family on Linux).
- Video / large files performance (see section 3, throughput).
- Wi-Fi Aware / NAN path – Windows has no Wi-Fi Aware API, and this path is not needed for AWDL.

### Secondary goals (later milestones)

- Sending PC → iPhone.
- Keeping internet over Wi-Fi while receiving.
- Signed driver + installer for normal users.

---

## 2. Known constraints and accepted trade-offs

| Constraint | Consequence | Status |
|---|---|---|
| AirDrop runs only over AWDL (Apple Wireless Direct Link), which needs raw 802.11 RX/TX (monitor + injection) and precise channel hopping. | Windows vendor drivers do not expose monitor/injection (`netsh wlan show wirelesscapabilities` → monitor mode not supported). We need **our own kernel driver for MT7921**. | Core problem, milestone M3–M4 |
| Our driver replaces the MediaTek driver while active. | **No Wi-Fi internet while receiving** in v0.1 (Ethernet unaffected). Accepted for MVP. Must auto-restore vendor driver afterwards. | Accepted for v0.1 |
| Unsigned kernel driver. | Development requires **Windows test-signing mode**, which requires **Secure Boot disabled**. Some anti-cheat games refuse to run. Distribution to normal users needs Microsoft attestation signing (EV certificate). | Dev: accepted. Release: M9. **Challenged by research**: winawdl runs a UMDF path with Secure Boot on – see `docs/RESEARCH.md` §0 and ADR-0002 |
| Contacts-only mode not forgeable. | iPhone must be set to "Everyone for 10 Minutes" (it expires). | Permanent |
| Measured throughput on MT7921 under Linux: ~45–67 kB/s receive (2.56 MB photo in ~40 s). | A typical iPhone photo takes ~1 minute. Improving this is an open research question (channel sequence strategy). | Accepted for v0.1 |
| Apple can change AirDrop in any iOS update (iOS 26 already broke stock OpenDrop). | Protocol layer must be easy to patch; keep captures as regression fixtures. | Ongoing |

---

## 3. Technical background (what we are implementing)

AirDrop stack, bottom to top:

1. **Bluetooth LE** – an AirDrop *sender* broadcasts an Apple Continuity advertisement that wakes nearby receivers' AWDL. For **receiving** (our v0.1), the iPhone is the sender: opening the share sheet is what brings its AWDL up. We do **not** need BLE for v0.1. BLE is needed only for **sending** to an iPhone (M8). Windows can transmit such adverts via WinRT `BluetoothLEAdvertisementPublisher` (reported working by `AirDrop-for-Everything`).
2. **AWDL** – Apple's proprietary peer-to-peer Wi-Fi link. Devices sync via action frames (PSF / MIF), elect a master, follow a channel sequence across social channels (6, 44, 149) plus others (e.g. 36). Data frames carry IPv6. Implemented in open source by **OWL** (seemoo-lab/owl, C, GPL-3.0).
3. **IPv6 link-local** over an `awdl0` interface. On Windows, create it with **Wintun** (already done by `winawdl`).
4. **mDNS / DNS-SD** – service `_airdrop._tcp`, advertised on `awdl0` only.
5. **HTTPS** (self-signed TLS) – endpoints `/Discover`, `/Ask`, `/Upload`. Payload of `/Upload` from iOS 26 is `application/x-dvzip` wrapping an ODC cpio archive (FACT, `docs/FINDINGS.md` F-002; details in `docs/PROTOCOL.md`). Implemented by **OpenDrop** (seemoo-lab/opendrop, Python, GPL-3.0), but **stock OpenDrop does not work with iOS 26** – use the patches from `jedbillyb/airdrop-mt7921/patches/`.

### Hard-won knowledge from airdrop-mt7921 (Linux, same chip) – read their FINDINGS.md

- Receiving and sending work end to end with iOS 26 on MT7921, no Apple ID, in "Everyone" mode.
- Sending was blocked by a missing `TransferID` field: `/Ask` must declare `TransferID={'id': UUID}` and `/Upload` must repeat the same id.
- Phone must have the **share sheet open** to be heard when we receive. Control Centre alone → zero AWDL frames.
- MT7921 runtime power management silently kills monitor RX (disable `runtime-pm` and `deep-sleep`). On Windows our driver must equivalently keep the chip awake.
- In-place interface type switch does not retune the radio – a dedicated monitor interface is needed. On Windows this translates into: our driver must explicitly program channel on every switch.
- **Do not trust reported channel**; trust the radiotap frequency on captured frames.
- Channel strategy `verbatim` works; `pin` breaks TX to iOS 26; `widen` is accepted by iOS 26 and may improve throughput (open question).
- MT7921 is a combo Wi-Fi + BT chip: reconfiguring Wi-Fi resets the BT controller (matters for M8 BLE wake).
- Wi-Fi coexistence: same-channel mode (AP on 36/44/149/6) coexists; a P2P-GO + MAC-aliased monitor trick allows a separate AWDL channel (`jedbillyb/mt7921-dual-channel`), transfer reached 99.1% but not proven complete.

---

## 4. Target hardware / environment

Values marked FACT were read from the system on 2026-11-08 (`Get-CimInstance`, `Get-PnpDevice`, `netsh wlan`). Device-identifying values (MAC, SSID, BT peers) are deliberately NOT recorded here.

- Laptop model: **Acer Swift X SFX14-41G** (FACT, WMI). No built-in RJ45 port (FACT: no Ethernet adapter enumerated).
- Wi-Fi card: **MediaTek Wi-Fi 6 MT7921**, `PCI\VEN_14C3&DEV_7961&SUBSYS_380211AD&REV_00` (FACT). Vendor driver version `3.5.0.1349` (FACT).
  - `netsh wlan show wirelesscapabilities`: Network monitor mode **Not supported**; Wi-Fi Direct Device/GO/Client supported; 2 concurrent channels; 2x2 MIMO (FACT).
- Bluetooth: **MediaTek Bluetooth MT7921** on **USB** `VID_04CA&PID_3802` (FACT) – the BT half of the combo chip is a separate USB function, not PCIe.
- Windows version / build: **Windows 11 Home 25H2, build 26200** (FACT).
- Secure Boot: **disabled** (maintainer, 2026-11-08).
- BitLocker / Device Encryption: **disabled** (maintainer, 2026-11-08). Re-check before first driver install; if it ever gets enabled, back up the recovery key first.
- iPhone model / iOS version: **iPhone 15 Pro, iOS 26.5.2** (maintainer). Policy: do NOT update iOS during the project unless a finding requires it – iOS cannot be downgraded and updates may break AirDrop interop. Record the iOS version in every FINDINGS entry.
- Router: currently connected on **2.4 GHz, channel 1** (FACT at time of reading). Not an AWDL social channel → same-channel coexistence (M9) would not apply with this AP as-is. Router supports 5 GHz (maintainer); 5 GHz SSID/channel `TODO` (not visible under the same SSID prefix in `netsh wlan show networks`).
- Ethernet / internet fallback without Wi-Fi: **no adapter**. Plan: iPhone USB tethering (Windows needs Apple Devices app / Apple Mobile Device driver – TODO-VERIFY on this machine; Linux uses `ipheth`). A USB-C Ethernet adapter is recommended before M3 (also relevant for KDNET).
- Second machine for kernel debugging: **available, Windows** (maintainer) → WinDbg host. Caveat: KDNET needs a supported wired NIC on the *target* (this laptop has only Wi-Fi; USB NICs are generally not supported by KDNET – TODO-VERIFY with `kdnet.exe`). Fallbacks: crash dumps + WinDbg, or KDNET via a supported adapter.
- USB stick for M0: **64 GB** (maintainer) → full Linux install on the stick (not live+persistence).
- Other software on the machine relevant to driver work: VMware Workstation (VMnet adapters), Tailscale (FACT).

---

## 5. Prior art – study before building

Clone into `third_party/` (git-ignored) or read online. **Check each licence before copying any code.** A repository with no licence file = all rights reserved: do not copy, only learn from it, and/or ask the author.

| Project | What it is | Use it for | Licence |
|---|---|---|---|
| `seemoo-lab/owl` | AWDL implementation (C) for Linux/macOS | AWDL protocol core for `awdld` | GPL-3.0 |
| `seemoo-lab/opendrop` | AirDrop protocol (Python) | Protocol layer reference / MVP | GPL-3.0 |
| `jedbillyb/airdrop-mt7921` (+ fork `jedbillyb/owl`) | AirDrop on Linux with built-in MT7921, iOS 26 working | **Primary reference**: patches, findings, MT7921 quirks, Phase 0 | GPL-3.0 |
| `jedbillyb/mt7921-dual-channel` | Wi-Fi + AWDL on separate channels on one MT7921 | M9 coexistence | GPL-3.0 (LICENSE @ b06d010) |
| `raph559/winawdl` | AWDL + AirDrop natively on Windows on Realtek RTL8852CE | **Windows I/O layer for OWL, Wintun `awdl0`, app structure** | Mixed (@ 0db7031): MIT root/driver/app/airdrop, **GPL-3.0+ `awdl/`**, **GPL-2.0-only `radio/` – do not use** |
| `21Suspect/AirDrop-for-Windows` | Native Windows AirDrop on Intel AX211, custom KMDF driver + "Owly" | Architecture reference for custom driver approach | GPL-3.0 (LICENSE @ 9ae8771) |
| `wrldwideee/win-drop` | AWDL/AirDrop for Windows on **MT7922** (same family as MT7921), early stage, has `driver/mt7922_awdl` and `userland/inject_test` | **Closest driver reference**. Consider contacting the author to collaborate. | **No licence file** (@ e9be009) – learn only, do not copy; ask author |
| `Foglio24/linux-airdrop` | GTK app, MT7921/MT7922 on Linux | UX reference, hardened OpenDrop | GPL-3.0 (LICENSE @ 6b1bf73) |
| `BenItBuhner/AirDrop-for-Everything` | Clean-room AirDrop protocol in Rust, multi-platform shells | Protocol reference, WinRT BLE | MIT (LICENSE @ 34bdc8d) |
| Linux kernel `drivers/net/wireless/mediatek/mt76/mt7921/` | Open MT7921 driver | **Reference for chip init, firmware load, MCU commands, DMA, channel switch, monitor RX, raw TX** | Cloned as `openwrt/mt76` @ 0936617: all MT7921/connac/core files `SPDX: BSD-3-Clause-Clear` (GPL-compatible per FSF list – TODO-VERIFY); a few unrelated files (mt76x0, mt7615 sdio/usb) are GPL-2.0-only – do not use those |
| `jiaqizou-msft/Airdrop-on-Windows` | Wi-Fi Direct instead of AWDL | Counter-example: Wi-Fi Direct does NOT make the PC appear natively in the share sheet | None (README: "To be determined") – do not copy |

First research deliverable: `docs/RESEARCH.md` – for each project: how it works, what is reusable, licence, maturity, open issues.

---

## 6. Architecture (proposed – confirm or revise via ADRs)

```
 iPhone (share sheet, "Everyone for 10 min")
        │  AWDL frames (802.11)
        ▼
 ┌─────────────────────────────┐
 │ driver/  mt7921awdl.sys     │  KMDF/NDIS kernel driver bound to MT7921 PCIe.
 │  - firmware load, MCU init  │  Exposes raw 802.11 RX/TX + channel control
 │  - raw RX (radiotap-like)   │  to user mode via IOCTL.
 │  - raw TX, set channel      │  Only active while "Receive" is on.
 └──────────────┬──────────────┘
                │ IOCTL (DeviceIoControl)
 ┌──────────────▼──────────────┐
 │ awdld/  (C)                 │  OWL protocol core + Windows I/O backend.
 │  - AWDL sync, peers, chseq  │  Creates Wintun adapter "awdl0" (IPv6 LL).
 └──────────────┬──────────────┘
                │ IPv6 link-local over awdl0
 ┌──────────────▼──────────────┐
 │ airdropd/                   │  mDNS responder (_airdrop._tcp on awdl0),
 │  - /Discover /Ask /Upload   │  HTTPS server, archive extraction,
 │  - iOS 26 patches           │  accept/decline hook. Runs unelevated.
 └──────────────┬──────────────┘
                │ local IPC (named pipe / localhost)
 ┌──────────────▼──────────────┐
 │ app/  tray app (C#/.NET)    │  "Receive" toggle, toast Accept/Decline,
 │                             │  save location, history, status.
 └─────────────────────────────┘
 ┌─────────────────────────────┐
 │ service/ (elevated helper)  │  Swaps MediaTek driver ↔ our driver,
 │                             │  watchdog that ALWAYS restores Wi-Fi.
 └─────────────────────────────┘
```

### Proposed language choices (write ADRs, revise if research says otherwise)

- `driver/`: C, KMDF (or NDIS miniport if research shows it is needed), WDK + Visual Studio 2022.
- `awdld/`: C, to reuse OWL directly. Windows I/O backend modelled on `winawdl`.
- `airdropd/`: MVP = patched OpenDrop (Python) running on Windows. Later option: rewrite in Rust or C# for a single-binary install. Decide in an ADR.
- `app/` + `service/`: C# / .NET, WinUI 3 or WPF, Windows App SDK toast notifications with buttons.

### Driver switching (critical, research item)

The MediaTek vendor driver and ours bind to the same PCI hardware ID. "Receive on" = unbind vendor, bind ours. "Receive off" or crash = rebind vendor. Research SetupAPI / `UpdateDriverForPlugAndPlayDevices` / `DiInstallDevice` / `pnputil`. Requirements:
- Watchdog that restores the vendor driver even if our process is killed (scheduled task or service).
- Time-limited receive window (default 2 minutes, configurable).
- Never leave the user without Wi-Fi after exit.

---

## 7. Milestones and acceptance criteria

Each milestone ends with: code merged, `docs/FINDINGS.md` updated, acceptance criterion demonstrated by the maintainer.

**M0 – Prove the chip on Linux (no Windows work yet)** — *DONE 2026-11-09, see `docs/FINDINGS.md` F-001/F-002.*
- Maintainer boots Linux from USB (e.g. Ubuntu / Fedora live with persistence) on the same laptop.
- Reproduce `airdrop-mt7921` receive: photo from iPhone arrives.
- Capture: pcap of AWDL session, TLS keylog of `/Ask` + `/Upload` (keep **outside** the repo), firmware file names/versions the Linux driver loads, PCI ID.
- ✅ Acceptance: photo received on Linux on this exact laptop. If this fails, STOP and reassess the whole project.

**M1 – Repo foundation**
- Structure (section 8), GPL-3.0 licence, README (English, honest limitations), `docs/ARCHITECTURE.md`, `docs/adr/`, `docs/FINDINGS.md` (numbered lab notebook), `docs/RESEARCH.md`, CI that builds user-mode parts, `.gitignore` for captures/keys/firmware.
- ✅ Acceptance: clean repo builds, docs explain the project to a newcomer.

**M2 – Protocol layer on Windows (testable without AWDL)**
- `airdropd` runs on Windows, serves `/Discover`, `/Ask`, `/Upload` with iOS 26 patches.
- Test harness replays recorded/synthesised requests (from M0 captures, redacted) and asserts a file is extracted correctly.
- Tray app prototype: toast with Accept/Decline wired to `/Ask` response.
- ✅ Acceptance: automated test sends a replayed upload, toast appears, file saved.

**M3 – Driver spike: raw RX (highest risk, time-box and report honestly)**
- Windows dev setup documented: WDK, test-signing, BitLocker key backup, Secure Boot off, restore procedure, (optionally) KDNET.
- KMDF driver binds to MT7921, loads firmware, initialises MCU and DMA following mt76, puts radio into a monitor-like mode on a chosen channel, delivers raw frames to user mode via IOCTL.
- ✅ Acceptance: user-mode tool dumps beacons from nearby APs and AWDL action frames from an iPhone with share sheet open (verified via radiotap frequency / frame content).

**M4 – Driver: raw TX + channel switching**
- Inject frames, switch channels on demand with low latency, keep chip awake (no power save).
- ✅ Acceptance: injected test frame seen by another monitor device; channel switch measured.

**M5 – awdld on Windows**
- OWL core with Windows backend talking to our driver; Wintun `awdl0` with IPv6 link-local.
- ✅ Acceptance: logs show AWDL sync with the iPhone, peer discovered, `ping` over `awdl0` link-local to the iPhone works (TODO-VERIFY whether iOS answers ICMPv6).

**M6 – End-to-end receive = v0.1 demo**
- Full user story from section 1.
- ✅ Acceptance: screen recording – iPhone share sheet shows the PC, toast on Windows, photo opens. This GIF/video goes to the top of the README.

**M7 – Robust driver switching**
- Service + watchdog, receive time window, guaranteed Wi-Fi restore, crash recovery.
- ✅ Acceptance: kill every process mid-transfer → Wi-Fi comes back automatically.

**M8 – Sending PC → iPhone**
- BLE wake via WinRT (mind the combo-chip BT reset), discovery, `/Ask` with `TransferID`, `/Upload`.
- Explorer context menu "Send to iPhone".

**M9 – Quality of life**
- Same-channel coexistence mode (keep internet if AP on 36/44/149/6), then dual-channel research.
- Throughput research (`widen` strategy).
- Installer, attestation-signed driver, MT7922 support.

---

## 8. Repository layout

```
mtdrop/
├── CLAUDE.md
├── README.md                 # English, demo GIF, honest limitations
├── LICENSE                   # GPL-3.0
├── docs/
│   ├── ARCHITECTURE.md
│   ├── RESEARCH.md           # prior art analysis
│   ├── FINDINGS.md           # numbered lab notebook: hypothesis → test → result
│   ├── DEV-SETUP.md          # WDK, test-signing, BitLocker, recovery
│   ├── PROTOCOL.md           # our understanding of BLE/AWDL/mDNS/HTTPS layers
│   └── adr/                  # 0001-language-choices.md, ...
├── driver/                   # KMDF driver (C)
├── awdld/                    # AWDL daemon (C, OWL-based)
├── airdropd/                 # AirDrop protocol service
├── app/                      # tray app (C#)
├── service/                  # elevated helper + watchdog (C#)
├── tools/                    # capture analysis, replay harness, diagnostics
├── tests/
│   └── fixtures/             # REDACTED protocol fixtures only
├── scripts/                  # setup / build / run (PowerShell)
└── third_party/              # git-ignored clones of prior art
```

---

## 9. Safety rules (non-negotiable)

- **Never** run `bcdedit`, install/uninstall/swap drivers, change Secure Boot, or modify network adapter state **without explicit confirmation** from the maintainer in that moment. Always print the exact command and the rollback command first.
- Before the first driver install: verify the maintainer has (1) BitLocker recovery key saved outside the laptop, (2) a System Restore point, (3) a way to get internet without Wi-Fi.
- Every script that changes Wi-Fi/driver state must have a tested restore path.
- **Never commit**: firmware blobs, TLS keylogs, raw pcaps, private keys, real device names, MAC addresses, Apple IDs, phone numbers, email addresses. Redact fixtures (replace MACs, names, hashes).
- Do not commit or redistribute MediaTek firmware. Load it from the user's system or fetch from its official source at install time (licence: TODO-VERIFY).
- Do not copy code from repositories without a compatible licence. Record the origin of every reused file (path + commit + licence) in `third_party/NOTICE` or file headers.
- Never `git push` or create public releases without the maintainer's approval.
- No Apple code, binaries or keys. This is an independent interoperability implementation.

---

## 10. Engineering conventions

- Small, reviewable commits; conventional commit messages (`feat(driver): ...`, `docs(findings): ...`).
- Every non-trivial decision → ADR in `docs/adr/`.
- Every experiment → numbered entry in `docs/FINDINGS.md`: hypothesis, what would falsify it, method, raw result, conclusion. Write predictions **before** running.
- Logs: structured, with timestamps; capture radiotap frequency for every RX'd frame in debug mode.
- Kernel code: follow WDF/KMDF patterns, check IRQL rules, run Driver Verifier during development.
- Prefer measured data over intuition, especially for channel/timing issues.

---

## 11. Your first tasks (in this order)

1. Ask the maintainer to fill section 4 (hardware/environment). Update this file. — *done (2026-11-08); only the 5 GHz channel remains TODO.*
2. Clone prior art into `third_party/` and write `docs/RESEARCH.md` (licences included). Flag anything that changes the plan. — *done (2026-11-08), first pass; see RESEARCH.md §0 and §9.*
3. Write `docs/PROTOCOL.md` from OWL + OpenDrop + airdrop-mt7921 findings (mark FACT / ASSUMPTION / TODO-VERIFY). — *done (2026-11-09), includes our own decrypted capture.*
4. Propose ADR-0001 (language/stack) and ADR-0002 (driver strategy: full custom MT7921 driver vs alternatives) with pros/cons. — *accepted by the maintainer (2026-11-09), see `docs/adr/`. ADR-0001 proposes C#/.NET for `airdropd`; ADR-0002 proposes a UMDF proxy + user-mode chip library first, thin KMDF as fallback, fixed channel 44 first.*
5. Scaffold the repo (M1). — *done (2026-11-09): README, LICENSE, ARCHITECTURE, DEV-SETUP, FINDINGS, ADRs, `third_party/NOTICE`, CI (Python tool tests), reference codecs in `tools/proto/`.*
6. Write a step-by-step **M0 guide in Polish** for the maintainer: which Linux live USB, how to set it up, how to build patched OWL + OpenDrop, how to capture pcaps and TLS keylog, what to send back. — *done (2026-11-08), moved ahead of 3–5 at maintainer's request: `docs/M0-GUIDE.pl.md` + `tools/m0/`. Results go to `C:\Users\desktop\mtdrop-private\` (outside the repo, never committed).*

Do not start driver code before M0 succeeds. (M0 succeeded 2026-11-09. Driver work additionally requires ADR-0002 accepted and the checklist in `docs/DEV-SETUP.md` complete.)

**M2 status (2026-11-09):** `airdropd` library + 76 tests + console host + tray app prototype are in place (FINDINGS F-003). Remaining for M2 acceptance: maintainer confirms the toast flow by hand (`tools/demo/fake_sender.py`).

**M3 spike S1 (2026-11-10): PASS** – UMDF proxy bound without test mode, chip ID 0x7961 read, vendor driver restored (FINDINGS F-004). **S2 (2026-11-10): PASS as SYSTEM** – 4–16 locked 2 MiB blocks below 4 GiB; the maintainer's admin account lacks SeLockMemoryPrivilege, so the radio process must run as SYSTEM (F-005). **S3 (2026-11-10): PASS** – vendor patch + RAM firmware downloaded from user mode, firmware ready in 470 ms (F-006). Hardware sessions must be started by the maintainer by hand (`scripts/run-as-system.ps1` + `scripts/s3-bringup.ps1`; Claude Code's classifier blocks launching them). **Sniffer RX (2026-11-11): PASS with linux-firmware** – 10k frames in 8 s on channel 1, 116 good beacons, per-frame channel correct; the Windows vendor firmware ignores UNI commands, so linux-firmware's MT7961 image is required (F-007). Session start needs a real restart if uptime is long (low-memory fragmentation; Fast Startup shutdown does not count). **M3 ACCEPTED (2026-11-11)** – AWDL action frames from the iPhone received on channels 44 and 6 (149 and 127 in 15 s), per-frame channel correct, channel switch median 10.1 ms; DMA region now built from scattered 4 KiB pages, no restart needed (F-008, F-009). **M4 raw TX (2026-11-11): works** – probe requests are radiated and answered by access points; firmware ACK of frames to our MAC still undecided (F-010). **ACK control + system hang (2026-11-11, F-011):** the control arm shows the firmware ACKs our MAC (1.32 vs 2.58 copies per response); the next run (`--control --pair`) froze Windows (no bugcheck, normal boots hung until the card was disabled in Safe Mode; recovered by a full power-off). Suspected unhandled interrupt (risk R1). Mitigated 2026-11-17: PCIe interrupt gate 0 while we own the chip, interrupt guard, write-through `radiod-live.log`. Re-entry ladder A–D (2026-11-17): **all PASS, no hang** – step A sniff (F-012), B/C TX single and pair, D the exact session that hung; ACK of our MAC confirmed for both vif setups (1.21/1.16 vs 2.5 copies per pair); interrupt gates stay closed by default (F-013). Next: unicast TX with TX status record (TXS), then M5 (OWL core on Windows).

**Earlier plan:** M3 preparation – `docs/DEV-SETUP.md` checklist with the maintainer, then spike S1 of ADR-0002 (UMDF proxy, read-only chip ID). Every driver-state command needs explicit confirmation (§9).
