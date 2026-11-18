# ADR-0005: Sending PC → iPhone from the tray app, woken over Bluetooth LE, on the existing receive session

- **Status:** Accepted (2026-11-18, maintainer; first send worked, F-028)
- **Deciders:** maintainer
- **Inputs:** CLAUDE.md §3 (BLE), §7 (M8); ADR-0001 (C# for `airdropd`/`app`); ADR-0003 (`awdld` with winawdl's Windows layer); ADR-0004 (service sessions); `docs/PROTOCOL.md` §9; prior art: `jedbillyb/airdrop-mt7921` `docs/FINDINGS.md` §19, §22, §27, §33, §36, §37, §40 (GPL-3.0); `raph559/winawdl` `DESIGN-NOTES.md` "Sending", `airdrop/sender.py`, `scripts/airdrop-beacon.ps1` (MIT), `awdl/win/service_bridge.c` (GPL-3.0+, already in our `awdld`); `21Suspect/AirDrop-for-Windows` `app/OpenDrop.cs` (GPL-3.0)

## Context

Receiving works and is robust (M6, M7). M8 is the other direction: a file chosen on the PC lands in the iPhone's Photos/Files, with nothing installed on the phone, in "Everyone for 10 Minutes".

What prior art established (REPORTED by them unless marked; none of it verified by us yet):

1. **The phone only announces itself as a receiver after a BLE advertisement.** An iPhone with the share sheet closed does not advertise `_airdrop._tcp`; Apple's sender first broadcasts BLE manufacturer data (company `0x004C`, Continuity type `0x05`, 18 bytes: 8 zero bytes, version `0x01`, four 2-byte contact hashes, one zero byte). In "Everyone" the hashes are not checked; zero or random hashes work. Proven by a control run (mt7921 §27: advert expired → no receiver despite a perfect link; restarted → found in 4 s). winawdl: the phone announced 3, 9, 13 and 50 s after its beacon started (four attempts). 21Suspect uses a different payload (`05 12 40 00…00 03 00…`); recorded as an alternative.
2. **On the same combo chip the BLE advert dies when Wi-Fi is reconfigured** (mt7921 §36, Linux: the advertising instance did not survive the monitor-mode set-up). They start the advert *after* the radio has settled. On this laptop Bluetooth is a separate USB function (`VID_04CA&PID_3802`, CLAUDE.md §4); whether our swap and WFSYS reset touch it is TODO-VERIFY.
3. **The phone's service records travel inside AWDL action frames** (service response TLVs), not in mDNS packets (winawdl). `awdld` already contains winawdl's `service_bridge.c`, which turns them into the mDNS response the phone would have sent, with a link-local address derived from its MAC, and hands it to `awdl0` (FACT, `awdld/win/io_win.c:202`). The Linux rig found the phone by plain mDNS, but only after re-asking every 1.5 s instead of exponential back-off (mt7921 §19: 0 → 14 replies).
4. **Requests.** `/Discover` (`Connection: close`) returns `ReceiverComputerName`/`ReceiverModelName`. Then, on a new connection, `/Ask` with `TransferID = {id: UUID}` (a dictionary), `TransferType = {files: {}}`, `Files` with `FileName`, `FileType` (UTI), `FileSize`, `FileBomPath = ./name`, `FileIsDirectory = false`, and `/Upload` on the **same** connection repeating the id in a `TransferID` header, `Content-Type: application/x-dvzip`, chunked. Without the announced id the phone closes TLS at the headers; with it, **no Apple ID or signed sender record is needed** (mt7921 §37 result, winawdl). Apple's own requests carry only `User-Agent: AirDrop/1.0`, `Connection`, `Transfer-Encoding: chunked` (mt7921 §33). TLS: self-signed client certificate, no server verification (winawdl `sender.py:182-184`); a TLS alert about the certificate means the phone is in Contacts Only.
5. **Body:** dvzip of an odc cpio that starts with a `.` directory member, padded to 512 bytes – the same shape we decode (PROTOCOL §6) and our test sender already writes (`Wire.Cpio`, `Wire.Dvzip`).
6. **Rate:** OWL sends data at 12 Mbit/s; winawdl measured ~1 MB/s while the phone listened, 0.5 MB/s overall. Enough for photos.

What we have: the radio session from the service (ADR-0004), `awdl0`, mDNS encode/decode for our responder, the archive and plist codecs, an HTTPS server. FACT (2026-11-18, read-only WinRT query on this laptop): the Bluetooth adapter supports LE, the **peripheral role** (needed to advertise), advertisement offload and extended advertising; radio on.

## Options

### 1. Where the sender runs

| | A: tray app, `Mtdrop.Protocol` client (proposed) | B: port winawdl's Python sender | C: inside the service |
|---|---|---|---|
| Language/stack | C#, one codebase with the receiver (ADR-0001) | second runtime to ship | C# |
| Privilege | user (reads the user's files, shows UI) | user | SYSTEM would read user files: no |
| Tests | loopback against our own `AirDropServer` | separate | same as A |

### 2. BLE wake

| | A: WinRT `BluetoothLEAdvertisementPublisher` in the tray app (proposed) | B: PowerShell 5.1 script as winawdl |
|---|---|---|
| Process | in-process, status events (`Aborted`) visible | child process, PowerShell 5.1 only |

### 3. When the advert runs

Start it after `awdl0` is up (prior art 2; also correct if the swap does not touch Bluetooth), keep it while browsing and until `/Upload` ends, restart it if the publisher reports `Aborted`. Stop it as soon as the send ends.

### 4. Radio session

Reuse the service's receive session unchanged (Start for N minutes, give-back at the end). During a send the tray app does not announce its own receiver, to keep the scarce co-channel airtime for the phone (mt7921 §19 found a stray announcer burning it). The session ends right after the transfer, not at the window end.

## Decision (proposed)

1. **Sender in the tray app**, protocol code in `Mtdrop.Protocol`: `CpioOdcWriter` and `DvzipWriter` (streaming from disk), `MdnsBrowser` (PTR query for `_airdrop._tcp.local` every 1.5 s on `awdl0`, parses PTR/SRV/TXT/AAAA), `AirDropClient` (`/Discover`, `/Ask`, `/Upload` with the iPhone's header set and a self-signed client certificate).
2. **BLE wake by WinRT publisher**, payload 1 (zero prefix, version 1, random hashes), started after `awdl0` is up, kept until the upload ends. 21Suspect's payload stays as the fallback if the phone does not announce.
3. **Same service session** (ADR-0004, no service change); the receiver is not announced during a send; the session is stopped after the send.
4. **UI:** tray menu "Send files…" → file picker → session → list of receivers found (name from `/Discover`) → pick → "Waiting for <name> to accept" → progress → result card. Then an Explorer context menu "Send to iPhone" (per-user `HKCU\Software\Classes\*\shell`, no elevation) that hands the files to the running tray app.
5. `SenderComputerName` = the PC name (as the receiver already shows it), `SenderModelName` free text, `SenderID` = the same 12-hex id the receiver uses, `BundleID` = `com.apple.finder` (winawdl), `ConvertMediaFormats` = false.

## Plan

| Step | What | Acceptance |
|---|---|---|
| M8.1 | Writers, `MdnsBrowser`, `AirDropClient`, offline tests; loopback test: our sender → our `AirDropServer` → file saved intact; `mtdrop.exe --send <file>` developer switch | tests pass offline |
| M8.2 | Hardware, maintainer: session + BLE advert + browse only, no transfer | log shows the phone's `_airdrop._tcp` records via `service_bridge`, seconds from advert to announce; whether Bluetooth survives the swap |
| M8.3 | First send, developer switch | one photo in the iPhone's Photos |
| M8.4 | Tray "Send files…", receiver picker, progress; Explorer "Send to iPhone" | **M8 acceptance:** a photo sent from Explorer's context menu arrives on the iPhone |

## Consequences

- No new privileged code; the service and the give-back are untouched.
- The tray app gains Bluetooth use (an advert that nearby Apple devices react to, for the length of a send only).
- One more TLS role (client) and a client certificate in the user's profile (generated once, like the receiver's).

## Open points

- Does our swap reset the BT function? (M8.2: publisher status before and after the swap.)
- Channel: our radio stays on 44 with `intersect`. As a receiver the phone may prefer other channels (mt7921 §40 saw 149-dominant sequences). If it announces but no transfer gets through, try another fixed channel before anything cleverer.
- Must the phone be unlocked / screen on? Must the advert stay up during the upload (mt7921 §27 left it open)?
- Throughput and large files are not a goal (CLAUDE.md §1 non-goals); photos first.
