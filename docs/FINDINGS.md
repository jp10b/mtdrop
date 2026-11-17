# Findings – lab notebook

Numbered entries: hypothesis → what would falsify it → method → raw result → conclusion.
Predictions are written **before** the experiment is run.

Labels: **FACT** = measured or read from a log/capture; **ASSUMPTION** = our interpretation; **TODO-VERIFY** = needs another experiment.

Raw artefacts (pcaps, TLS keylogs, logs, received files) are kept outside the repository and are never committed. Everything below is aggregated and redacted: no MAC addresses, device names, pseudonyms, push tokens or transfer IDs.

---

## F-001 – M0: the MT7921 in this laptop receives a photo from iOS 26.5.2 under Linux

- **Date:** 2026-11-09
- **Milestone:** M0 (acceptance: photo received on Linux on this exact laptop)
- **iPhone:** iPhone 15 Pro, iOS 26.5.2, AirDrop "Everyone for 10 Minutes", Personal Hotspot off
- **Laptop:** Acer Swift X SFX14-41G, MT7921 `14c3:7961` (subsystem `11ad:3802`)

### Hypothesis (written 2026-11-08, in `docs/M0-GUIDE.pl.md`)

The `jedbillyb/airdrop-mt7921` rig (patched OWL + patched OpenDrop) receives a photo from the iPhone on this laptop when booted into a Linux kernel that still offers *active monitor* for MT792x.

### What would falsify it

- `iw list` shows no "active monitor" on the chosen kernel (then the test says nothing about the chip, only about the kernel), or
- the laptop never appears in the share sheet, or
- `/Upload` never completes with a valid image after retries on several channels.

### Method

- Ubuntu 26.04.1 LTS live session from USB, kernel `7.0.0-30-generic`, in-tree `mt7921e`.
- `tools/m0/m0-setup.sh`, then `REG=PL ACTIVE=1 RECV_TIME=180 SSLKEYLOGFILE=… ./airdrop.sh receive`.
- airdrop-mt7921 @ `ff3826e`, jedbillyb/owl @ `57b8678`, channel strategy `verbatim` (default).
- One JPEG photo (2 374 534 B, 4032×3024) sent from the Photos share sheet.
- Artefacts: OWL log, OpenDrop log, Ethernet-level capture on `awdl0` (TLS, with keylog), two 5 s radiotap scan captures (ch 6, ch 44). No radiotap capture of the transfer itself was taken.

### Raw result

**Environment (FACT)**

| Item | Value |
|---|---|
| Kernel / distro | 7.0.0-30-generic, Ubuntu 26.04.1 LTS |
| Active monitor | offered ("Device supports active monitor (which will ACK incoming frames)") |
| ASIC revision | `79610010` |
| Firmware loaded by mt76 | `WIFI_MT7961_patch_mcu_1_2_hdr.bin`, `WIFI_RAM_CODE_MT7961_1.bin`; HW/SW version `0x8a108a10`, WM build `20260224110949` |
| Regulatory domain | PL (DFS-ETSI) |
| Interface modes | managed, AP, AP/VLAN, monitor, P2P-client, P2P-GO, P2P-device |

**Outcome (FACT)**

| Attempt | Result | Bytes | Duration | Average rate |
|---|---|---|---|---|
| 1 | Sender went quiet; partial file kept | 1 093 781 of 2 374 534 | ~31 s | ~35 kB/s |
| 2 | **Complete, valid JPEG** | 2 372 459 (archive) → 2 374 534 (file) | 33.6 s | **~71 kB/s** |

- Peak 5-second window in attempt 2: ~82 kB/s.
- Attempt 2 finished through the server's "archive complete, not waiting for the 0-chunk" path. The capture shows a terminating 0-chunk on the wire for this upload (see F-002); whether it arrived before or after the server's `200` was not determined.
- Request sequence seen by the server: `/Discover` → `/Ask` (answered after ~3.2 s and ~1.5 s) → `/Upload` with `Content-Type: application/x-dvzip`, `TotalBytes`, `TransferID`, `SenderPseudonym`, `SenderPushToken`, `Transfer-Encoding: chunked`.

**AWDL behaviour (FACT, from the OWL log, 180 s window)**

- Sequences advertised by the phone (16 slots; 0 = not available):

  | Sequence | When |
  |---|---|
  | `1,0,44,0,0,0,0,0,6,0,44,0,0,0,0,0` | idle / right after discovery |
  | `44,44,44,0,0,0,0,0,6,44,44,0,0,0,0,0` | idle with our peer known |
  | `44 ×5,0,0,0,6,44 ×4,0,0,0` | transitional |
  | `44 ×8,6,44 ×7` (15/16 on ch 44, slot 8 on ch 6) | during `/Ask` and `/Upload` |

- Slot 8 was channel 6 in every sequence. Channels 36 and 149 were never used. Scan before the run: AWDL frames on 2437 MHz (44 frames / 5 s) and 5220 MHz (54 frames / 5 s), none on ch 36, 132, 149.
- Channel 1 appears only in the idle sequence. The home AP is on 2.4 GHz channel 1. **ASSUMPTION:** it is the phone's infrastructure channel.
- The phone was sync master throughout, apart from two seconds at start-up. OWL followed its sequence (encoding 3).
- Channel switches requested by OWL: 432 in 180 s (215 → ch 44, 176 → ch 6, 41 → ch 1).
- **Channel switch latency (nl80211 round trip): min 6.4 ms, median 7.7 ms, p95 8.8 ms, max 9.1 ms** (n = 432).
- Sync error after lock: within ±10 TU.
- OWL counters at the end: `tx_action 4344, tx_data 2216 (unicast 2138), rx_action 2266, rx_data 3029, rx_unknown 2050`.
- 2132 received action frames were rejected with "unexpected bytes (2) at end of frame" – almost half of all received action frames.
- Signal: −35…−45 dBm on ch 44, −55…−70 dBm on ch 6 (same phone, same position).
- Reported channel in the rig's `radio.log`: "NA" for every sample.

**TCP level (FACT, capture on `awdl0`)**

- Client MSS 1440, window scale 6, SACK permitted; typical segment payload 1378 B.
- Attempt 2: 1923 data segments, 76 retransmitted/out-of-order, 70 duplicate ACKs.
- Inter-segment gap: median 0.9 ms, p99 ~0.4 s (bursty: data flows only in shared slots).

**Attempt 1 stall (FACT + ASSUMPTION)**

- FACT: the phone narrowed its sequence from 15/16 to 5/16 slots on ch 44 at +46 s and received data stopped within ~3 s; the server gave up after 8 s of silence.
- FACT: the phone retried by itself 34 s later, without any action on the laptop, and the retry completed.
- ASSUMPTION: same phenomenon as "phone narrows its sequence mid-transfer" in airdrop-mt7921 FINDINGS. Cause not established.

**Problems hit during setup (FACT, fixed on the spot)**

- Live session mounts the Windows volume under `/run/media/$USER/…`, not `/media/$USER/…`.
- `git` is not on the live ISO.
- CMake 4 rejects OWL's bundled googletest; fixed with `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- `sudo-rs` 0.2.13: `sudo -v` asks for a password despite `NOPASSWD`; `airdrop.sh` line 228 changed to `sudo true`.

### Conclusion

1. **M0 acceptance met.** The MT7921 in this laptop, with the stock `WIFI_*_MT7961_*` firmware, can receive AirDrop from iOS 26.5.2. The remaining problem is Windows software, not hardware capability.
2. **A fixed channel 44 would cover 15 of 16 slots during a transfer** on this phone. A driver that sits on ch 44 and only visits ch 6 for slot 8 (or never) is a realistic first target; full hopping is not required for the data phase. Whether the phone keeps sending unicast to a peer that *advertises* a fixed sequence is still open (see RESEARCH §9 item 2) – what we measured is the phone's own sequence, which we followed verbatim.
3. **Channel switch budget:** ~8 ms per hop on Linux is 12 % of a 64 TU slot. The Windows driver should aim for the same order of magnitude and report the per-frame channel.
4. **Throughput ~71 kB/s** matches the range published for this rig. It is enough for v0.1 (one photo in ~35 s).
5. **Robustness matters as much as speed:** one of two attempts stalled. `airdropd` must keep the session open across a stalled attempt, because the phone retries by itself.
6. **Kernel choice:** Ubuntu 26.04 LTS (kernel 7.0) still offers active monitor for MT792x. This answers RESEARCH §9 item 1 for this kernel.

### Follow-ups

- TODO-VERIFY: the 2132 action frames rejected for 2 trailing bytes. winawdl trims trailing zeros from iPhone MIF frames for the same symptom. Check which frame subtype is affected and whether accepting them improves sync or peer validity.
- Done in F-002: TLS capture decrypted; request shapes recorded. Still to do for M2: build synthetic fixtures with the same shape.
- TODO-VERIFY: cause of the mid-transfer narrowing (attempt 1).
- Not measured: retry rate at 802.11 level and the data rates used by the phone (no radiotap capture of the transfer). Needed before M4 to judge whether firmware ACKs work as on Linux.

---

## F-002 – What iOS 26.5.2 actually sends: decrypted `/Discover`, `/Ask`, `/Upload`

- **Date:** 2026-11-09 (analysis of the F-001 capture)
- **iPhone:** iPhone 15 Pro, iOS 26.5.2. **Receiver:** jedbillyb OpenDrop fork on Linux.

### Hypothesis

The request formats described in `docs/RESEARCH.md` §4.3 (from the airdrop-mt7921 patch notes) match what this phone sends: chunked requests, `application/x-dvzip` upload made of 128 KiB blocks wrapping an ODC cpio archive.

### What would falsify it

Any of: `Content-Length` requests, a different upload content type, a block or archive layout our decoder cannot walk to `TRAILER!!!`.

### Method

TLS 1.3 records of the `awdl0` capture decrypted offline with the session keylog (own script: TCP reassembly, HKDF-Expand-Label, AES-256-GCM). Plists decoded, dvzip blocks inflated, cpio headers walked. Only the *shape* is recorded here; decrypted data stays outside the repository.

### Raw result (all FACT for this phone and this receiver)

**TLS**

- TLS 1.3, cipher suite `TLS_AES_256_GCM_SHA384` (0x1302), on all six connections.
- Server flight: EncryptedExtensions, Certificate, CertificateVerify, Finished. The OpenDrop server did not request a client certificate and the phone sent none. The phone accepted the self-signed server certificate.
- One connection completed the server flight but the phone never sent its Finished and sent no request.

**`POST /Discover`**

- Headers: `User-Agent: AirDrop/1.0`, `Connection: close`, `Transfer-Encoding: chunked`. No `Host`, no `Content-Type`, no `Content-Length`.
- Body: one chunk (3918 B) + terminating 0-chunk. Binary plist (`bplist00`) with two keys:
  - `SenderRecordData` – data, 3802 B, DER (starts `30 80 06 09 2a 86 48 86`, a CMS/PKCS#7 container). Identifies the sender's Apple account → never store in fixtures.
  - `DeviceSupportFlags` – integer, `111611` (`0x1B3FB`).
- The receiver answered `200` with `Content-Length` and a binary plist: `ReceiverComputerName`, `ReceiverModelName`, `ReceiverMediaCapabilities` (data holding the JSON text `{"Version": 1}`). The phone then listed the receiver.

**`POST /Ask`**

- Headers: `User-Agent: AirDrop/1.0`, `Connection: keep-alive`, `Transfer-Encoding: chunked`.
- Body: 157 344 B in chunks of 131 072, 16 371, 9 901 B + 0-chunk. Binary plist, keys in this order:

  | Key | Type | Observed |
  |---|---|---|
  | `TransferID` | dict | `{ id: <36-char UUID string> }`, new for every attempt |
  | `TransferType` | dict | `{ files: {} }` |
  | `SenderRecordData` | data | 3802 B, same container as in `/Discover` |
  | `SenderID` | string | 12 lowercase hex characters |
  | `BundleID` | string | `com.apple.mobileslideshow` (Photos) |
  | `SenderComputerName` | string | the phone's name |
  | `SenderModelName` | string | `iPhone` |
  | `Items` | array | empty |
  | `Files` | array of dict | one entry, see below |
  | `FileIcon` | data | 152 908 B, JPEG 2000 (`00 00 00 0c 6a 50 20 20`) |
  | `ConvertMediaFormats` | bool | false |
  | `SenderIdentityAuthTag` | data | 3 B, different in each attempt |

- `Files[0]`: `FileName` (string), `FileType` = `public.jpeg`, `FileSize` = exact byte size, `FileBomPath` = `./<FileName>`, `FileIsDirectory` = false, `ShouldConvertMediaFormats` = false.
- The receiver answered `200` with `ReceiverComputerName` and `ReceiverModelName` only. That was enough for the phone to start the upload.

**`POST /Upload`**

- Sent on the **same TCP/TLS connection** as the `/Ask` it belongs to (keep-alive), 1–2 s after the `/Ask` response.
- Headers, in order: `User-Agent`, `TotalBytes` (= the file's byte size, not the archive size), `Content-Type: application/x-dvzip`, `SenderPseudonym`, `SenderPushToken`, `TransferID` (same UUID as in `/Ask`), `Connection: keep-alive`, `Transfer-Encoding: chunked`.
- Chunks: 16 371 B each (first chunk 87 B, last 900 B), 166 chunks for the complete upload.
- dvzip stream = sequence of blocks, each `[4-byte big-endian header][payload]`. Header bit 31 set → payload stored as is; clear → zlib stream (`78 9c`). Low 31 bits = payload length.
  - 20 blocks: 12 zlib then 8 stored. Every block inflates to 131 072 B except the first (169 B) and the last.
  - Compressed payloads of up to 131 118 B occurred, i.e. **larger than the 128 KiB they inflate to**. A decoder must not assume payload ≤ 128 KiB.
  - Total inflated size 2 376 704 B = 4642 × 512 (archive padded to a 512-byte boundary with zeros).
- Inner archive: cpio "odc" (magic `070707`, 76-byte ASCII-octal header). Entries, in order:
  1. `.` – directory, mode `040700`
  2. `./<FileName>` – regular file, mode `100644`, size = `FileSize`
  3. `./._<FileName>` – 1791 B **AppleDouble** sidecar (magic `00 05 16 07`, "Mac OS X"): extended attributes, not user content
  4. `TRAILER!!!`
- In the stalled attempt the stream ended inside a zlib block (49 109 of 131 091 B) with no 0-chunk.

### Conclusion

1. Hypothesis confirmed. Our `/Upload` decoder needs: chunked decoding, dvzip with oversized zlib payloads, ODC cpio.
2. **New:** the `._*` AppleDouble entry must be dropped, not saved next to the photo, and must not count as a second received file.
3. **New:** `/Upload` arrives on the `/Ask` connection, so the accept/decline decision and the upload can be tied together per connection in addition to `TransferID`.
4. `TotalBytes` describes the payload file size; "complete" must be decided from the archive (`TRAILER!!!`) or the 0-chunk, whichever comes first.
5. The receiver does not need a client certificate, an Apple-signed record or any `ReceiverRecordData` for "Everyone" mode on iOS 26.5.2.
6. Fixtures for M2 must be **synthetic** (own plist values, own image, same shape). `SenderRecordData`, `SenderID`, the pseudonym and the push token identify the sender and must not be copied.

### Follow-ups

- TODO-VERIFY: meaning of `DeviceSupportFlags` bits and of `SenderIdentityAuthTag`.
- TODO-VERIFY: how long the phone waits for the `/Ask` response (decides how long the Accept/Decline toast may stay open). Observed so far: answers after 1.5 s and 3.2 s were accepted.
- TODO-VERIFY: multi-file and HEIC/Live Photo/video transfers (`Files` with several entries, `ConvertMediaFormats`).

---

## F-003 – The C# receiver decodes what the phone really sent

- **Date:** 2026-11-09
- **Milestone:** M2 (protocol layer on Windows, testable without AWDL)
- **Code:** `airdropd/Mtdrop.Protocol` at the commit that adds this entry

### Hypothesis

A receiver written only from `docs/PROTOCOL.md` accepts the byte stream captured in F-001 and produces the same file the Linux rig saved.

### What would falsify it

Any `/Discover`, `/Ask` or complete `/Upload` from the capture is rejected, or the saved file differs from the original by even one byte, or a sidecar/temporary file is left in the destination folder.

### Method

1. The decrypted client-to-server bytes of each TLS connection from F-001 (six connections) were sent unchanged over a new TLS connection to `AirDropServer` on the loopback interface (test `Replays_a_private_capture_when_available`, enabled by an environment variable; the data stays outside the repository).
2. Live smoke test of the console host on an isolated host-only virtual adapter of the same machine: a script listened for the announcements, sent a `PTR _airdrop._tcp.local` query, then connected by TLS to the advertised address and port and posted `/Discover`.

### Raw result (FACT)

| Capture connection | Content | Result |
|---|---|---|
| 0, 3 | `/Discover` | `200`, receiver name returned |
| 1 | `/Ask` + `/Upload` that stalled in F-001 | `/Ask` `200`; upload reported "sender went quiet"; **no file left** |
| 2 | `/Ask` + complete `/Upload` | `200` / `200`; saved file **byte-identical** to the photo saved by the Linux rig (2 374 534 B); AppleDouble sidecar not saved |
| 4 | TLS handshake only | no request (nothing to replay) |
| 5 | `/Ask` cut off by the end of the capture | connection timed out, no prompt, no file |

Smoke test: five-record announcement (PTR, SRV, TXT, AAAA, NSEC) heard every 2 s; a query was answered within 0.5 s; TLS 1.3 with `TLS_AES_256_GCM_SHA384` negotiated by the Windows TLS stack with our generated RSA-2048 certificate; `/Discover` returned the three expected keys.

Automated tests at this point: 76 C# tests and 18 Python tests pass.

### Conclusion

1. Hypothesis confirmed for the one transfer type we have (single JPEG from Photos).
2. The Windows TLS stack offers what the phone negotiated in F-001. Whether the phone accepts *our* certificate is still unverified: it was never shown to a phone.
3. Opening the mDNS socket next to the operating system's own responder on UDP 5353 works with address reuse on Windows 11 build 26200.

### Follow-ups

- Not verified without a radio: anything involving the real phone's reactions (certificate acceptance, decline status, how long it waits for `/Ask`).
- The multicast loopback option must stay enabled on Windows for a same-machine query to reach the responder (it acts on the receiving side there).

---

## F-004 – Spike S1: a UMDF register proxy reads the MT7921 chip ID on Windows

- **Date:** 2026-11-10
- **Milestone:** M3, spike S1 of ADR-0002
- **Machine:** Windows 11 Home build 26200, Secure Boot off, no test-signing mode
- **Code:** `driver/proxy`, `driver/tools/Mtdrop.RegTool`, `scripts/s1-chipid.ps1`, `scripts/wifi-giveback.ps1`

### Hypothesis (written before the run, in the script header and ADR-0002)

A UMDF 2 driver signed with a locally trusted certificate can be bound to the Wi-Fi card in place of the vendor driver without test mode, map BAR0, and read `MT_HW_CHIPID` = `0x7961` through the L1 remap window; afterwards the vendor driver rebinds without a reboot.

### What would falsify it

Windows refuses the package or the forced install; the proxy cannot map BAR0; the chip ID block reads all zeros/ones or another value; the vendor driver does not come back.

### Method

`s1-chipid.ps1`: catalog with Inf2Cat, self-signed code-signing certificate added to LocalMachine Root and TrustedPublisher (private key deleted after signing), `pnputil /add-driver`, forced `UpdateDriverForPlugAndPlayDevicesW` on hardware ID `PCI\VEN_14C3&DEV_7961&CC_0280`, `regtool chipid`, then `wifi-giveback.ps1` (`pnputil /delete-driver … /uninstall /force`, `/scan-devices`). A watchdog process was armed to give the card back after 150 s.

### Raw result (FACT)

| Step | Result |
|---|---|
| Package accepted, proxy bound | yes, 2.9 s, device status OK, no reboot requested |
| BAR0 | 0x100000 bytes, mapped directly into the driver host (`direct mapped: True`) |
| `MT_HIF_REMAP_L1` (0xFE24C) as found | `0x18451800` |
| `MT_CONN_ON_LPCTL` (0xE0010) | `0x00000000` |
| `MT_CONN_ON_MISC` (0xE00F0) | `0x00000001` |
| `MT_WFDMA0_GLO_CFG` (0xD4208) | `0x1010B870` – TX and RX DMA enable bits clear |
| `MT_WFDMA0_HOST_INT_ENA` (0xD4204) | `0x00000000` |
| `MT_PCIE_MAC_INT_ENABLE` (0x10188) | `0x000000FF` |
| `MT_HW_CHIPID` | **`0x00007961`** |
| `MT_HW_REV` | `0x00008A10` → ASIC revision `79610010`, identical to Linux (F-001) |
| `MT_HW_BOUND` | `0x00000018` (bit 7 clear → MT7921, not MT7920) |
| Remap register restored | yes, read back `0x18451800` |
| Vendor driver back | yes, ~15 s after the give-back started, same package and version, no reboot |
| Wi-Fi down in total | ~20 s between swap and vendor driver bound |
| Bluetooth (USB function) | device status OK after the test |
| Whole script | 69 s, of which ~30 s waiting for a Wi-Fi connection that had been switched off by the user beforehand |

Side effects observed:

- The network adapter was re-created: **all 32 saved Wi-Fi networks were gone** and were re-added from the export made before the swap (keys stay encrypted for the machine). The Wi-Fi software switch, which the user had turned off, came back on.
- `Win32_PnPSignedDriver` (CIM) still reported the vendor driver while the proxy was bound. The scripts now read live PnP properties instead.
- The signing certificate stays trusted on the machine until `wifi-giveback.ps1 -RemoveCertificate` is run.

### Conclusion

1. **Hypothesis confirmed.** Option A of ADR-0002 clears its first gate on MediaTek hardware: no kernel code of ours, no test mode.
2. The vendor driver leaves the chip with DMA disabled and host interrupts masked when it unloads – a clean starting point for our bring-up.
3. The chip ID block is readable right after the swap without any ownership handshake.
4. Driver swap in both directions works without a reboot and takes seconds; the saved-network loss must be handled on every give-back (it is).

### Follow-ups

- S2: can we get DMA-capable memory below 4 GiB from user mode on this machine (32-bit DMA mask)?
- Not tested: behaviour across sleep/hibernate while the proxy is bound; a crash of the driver host process.
- TODO-VERIFY: meaning of `MT_CONN_ON_MISC = 1` (firmware state after the vendor driver unloads) – read the mt76 definitions before S3.

---

## F-005 – Spike S2: locked memory below 4 GiB is obtainable from user mode, as SYSTEM

- **Date:** 2026-11-10
- **Milestone:** M3, spike S2 of ADR-0002
- **Machine state:** 15.4 GiB RAM, 2.9 GiB free, 11 h since boot
- **Code:** `driver/tools/awe_probe` (winawdl's probe, MIT, unchanged), `scripts/s2-memory-probe.ps1`, `scripts/run-as-system.ps1`

### Hypothesis

An elevated process can allocate locked 2 MiB physical blocks with known addresses, at least two of them below 4 GiB (the MT7921 takes 32-bit DMA addresses), and map them into its address space.

### What would falsify it

The privilege is unavailable to every account we can run as; no block below 4 GiB in repeated runs; a block cannot be mapped and written.

### Method

The probe enables `SeLockMemoryPrivilege`, allocates 512 MiB of ordinary locked pages and as many 2 MiB blocks as it gets (`AllocateUserPhysicalPages2` with the non-paged-large attribute), reports their physical addresses, tries to map one block, and frees everything. Three runs, 2 s apart. No device involved.

### Raw result (FACT)

| Run as | Result |
|---|---|
| Elevated administrator (the maintainer's account) | **Privilege not held** – the probe cannot run. Windows Home grants "Lock pages in memory" to no user by default |
| SYSTEM (temporary scheduled task) | Privilege held; results below |

| Run | 2 MiB blocks obtained | below 4 GiB | Lowest addresses |
|---|---|---|---|
| 1 | 57 | **16** | 0x07600000, 0x08C00000, 0x10A00000 … |
| 2 | 46 | **11** | 0x07600000, 0x07E00000, 0x08C00000 … |
| 3 | 41 | **4** | 0x10A00000, 0x18400000, 0x18600000, 0x81400000 |

- Mapping: reserving a view with plain `MEM_PHYSICAL` and mapping all 512 frames of a block works (first and last byte writable). Mapping the block as a single frame, or with large-page flags, does not.
- Ordinary 4 KiB locked pages: ~20 000 of 131 072 below 4 GiB, but fragmented (longest run 64–1024 pages); not a substitute for the 2 MiB blocks.

### Conclusion

1. **Hypothesis confirmed with a condition: the radio process must run as SYSTEM** (or the account must be granted the privilege, which needs a policy change and a new logon). This matches the architecture: the elevated `service/` starts `radiod`.
2. Low memory is available but varies from run to run (16 → 4 blocks within seconds, while other allocations were live). The chip library must therefore fit its DMA memory into **as few low blocks as possible** and fail cleanly, before touching the chip, if it cannot get them.
3. Budget estimate (ASSUMPTION, to be fixed in the S3 design): rings ≈ 100 KiB, firmware scatter buffers ≈ 64 KiB, RX buffers 256 × 2 KiB ≈ 512 KiB, TX bounce pool ≈ 512 KiB → **one 2 MiB block** can hold everything if the RX ring is smaller than mt76's 1536 entries.
4. Physical addresses are assumed to work as bus addresses (no IOMMU translation), as winawdl found on the same Windows build. Unverified here until S3; Kernel DMA Protection state on this machine is TODO-VERIFY.

### Follow-ups

- S3 precondition: check Kernel DMA Protection / VBS state (`msinfo32`), decide ring sizes, write the "no low block → abort before touching hardware" path first.
- Long-uptime behaviour (winawdl saw a single low block after 13 days) – mitigate by allocating at session start and reporting a clear error.

---

## F-006 – Spike S3: MediaTek firmware boots on Windows from a user-mode driver

- **Date:** 2026-11-10
- **Milestone:** M3, spike S3 of ADR-0002
- **Code:** `driver/mt792x` (library 0.1.0), `driver/radiod`, `driver/proxy` (version 2), `scripts/s3-bringup.ps1`
- **Firmware:** the files of the installed vendor package 3.5.0.1349 (patch build `20250922193033`, RAM build `20250922193119`), read from the maintainer's backup; nothing redistributed.

### Hypothesis (written before the run: ADR-0002 spike table, `docs/MT7921-BRINGUP.md` section 6)

Following the mt76 sequence S0–S23 from user mode – registers through the UMDF proxy, DMA in one locked 2 MiB block below 4 GiB, polling instead of interrupts – the chip accepts the vendor's own patch and RAM image and reports `MT_CONN_ON_MISC` bits 1:0 = 3 (firmware ready).

### What would falsify it

TX ring indexes never advance (no bus mastering, or DMA blocked by the platform); no event arrives in our RX buffers; the semaphore or download commands are rejected; firmware-ready never appears.

### Method

Gated stages, each in its own `radiod` run followed by the orderly shutdown, inside three driver-swap sessions run as SYSTEM: stages 1–2 (ownership, reset; no DMA), 3–4 (rings, DMA on, first MCU commands), 5 (downloads). A stop list (10 register operations plus a 50 ms wait) was armed in the proxy before each run; a watchdog gave the card back after 240 s regardless.

### Raw result (FACT)

| Step | Observation |
|---|---|
| S1/S2 ownership | `LPCTL` 0x4 then 0x0, first attempt, 4–16 ms |
| S4 WFSYS reset | `INIT_DONE` after 15 ms; `CONN_ON_MISC` reads 0 afterwards |
| S7 DMA disable | `GLO_CFG` 0x00103870 |
| S8 rings | all six base/size registers read back as written, `dma_idx` 0 |
| S10 DMA enable | `GLO_CFG` **0x5030b875**, exactly the value predicted from the measured start state |
| S14 `NIC_POWER_CTRL` | consumed from TX ring 17 (**the chip read host memory**); the chip also answers it (event id 0x03, seq 1), which mt76 does not wait for |
| S15 | `CONN_ON_MISC` = 1 after 10–25 ms |
| S16 semaphore | event on RX ring 0, 36 bytes, result **2** (patch not loaded) (**the chip wrote host memory**) |
| S17–S19 patch | 92 032 bytes in 23 chunks in 4 ms; finish result 0 |
| S20 release | result 3, after ~245 ms |
| S21 RAM | four regions, 763 904 bytes, ~75 ms in total |
| S22 `FW_START_REQ` | response on **RX ring 0** (answers a TODO of the specification), after 19 ms |
| S23 | **`CONN_ON_MISC` = 0x3, firmware ready**, immediately |
| Whole bring-up | **470 ms** |
| After firmware start | `HOST_INT_ENA` reads 0x20000000 although the host never wrote a non-zero value; `DUMMY_CR` 0xffff0003 |
| Shutdown | DMA stopped, WFSYS reset, remap restored, ~90 ms each time |
| Vendor driver | rebound after every session, no reboot |

Other observations:

- DMA worked at three different physical addresses (0xC4E00000, 0x28000000, 0x8E000000) with virtualization-based security running: physical addresses are valid bus addresses for this device on this machine, and Bus Master Enable is set without us touching configuration space.
- First attempt at stages 3–4 failed **before touching the chip**: with 0.8 GiB of RAM free no 2 MiB block could be allocated (error 1450). The allocator now asks in rounds and the script checks memory before the driver swap.
- Claude Code's permission classifier refused to launch the session as SYSTEM; the maintainer ran the same command by hand.
- Polling cost: proxy register round trips were fast enough for 4 KiB chunks at ~10 MB/s.

### Conclusion

1. **Hypothesis confirmed. ADR-0002 option A (UMDF proxy + user-mode chip library) carries the MT7921 through firmware boot.** No kernel code of ours, no test-signing, no interrupts.
2. The risks R2 (bus mastering) and R3 (DMA remapping) of the specification did not materialise on this machine.
3. The vendor's Windows firmware files are compatible with the mt76 download sequence.
4. Events arrive on RX ring 0 before and at firmware start; ring 4 was not needed up to this point.
5. Process requirements that are now facts: run as SYSTEM; free RAM matters at session start.

### Follow-ups

- Next gate (S4 of ADR-0002 = acceptance of M3): post-boot MCU commands, sniffer mode on one channel, RX ring 2 → beacons from the home AP decoded with a per-frame channel.
- TODO-VERIFY: why `HOST_INT_ENA` reads 0x20000000 after firmware start, and whether an unhandled interrupt can result (R1). No ill effect was seen in a 470 ms window.
- TODO-VERIFY: behaviour if `radiod` is killed mid-download (stop list replay) – exercise deliberately before longer sessions.
- The 50 ms wait in the stop list relies on `Sleep` inside the UMDF host; not yet exercised by a real process death.

---

## F-007 – Monitor reception works on Windows, but only with the linux-firmware image

- **Date:** 2026-11-11
- **Milestone:** M3 (acceptance part 1: beacons from nearby APs with a per-frame channel)
- **Code:** `driver/mt792x` (post-boot, sniffer, RX path), `radiod sniff`, `scripts/s3-bringup.ps1`
- **Firmware compared:** (A) vendor Windows package 3.5.0.1349: RAM `____000000`, build 20250922, release-info feature byte 0x00; (B) linux-firmware as shipped in the mt76 clone: RAM `____010000`, build 20231109, feature byte 0xc0 (CNM). Neither is in the repository.

### Hypothesis

After firmware boot, the mt76 post-boot command sequence followed by the mt76 monitor-interface sequence makes the chip deliver raw 802.11 frames on RX ring 2, with the channel in the RX descriptor equal to the tuned channel.

### What would falsify it

A command of the sequence is rejected or unanswered; no frame arrives; beacons arrive only with bad FCS; the per-frame channel differs from the tuned one.

### Method

`radiod bringup 6`, then `radiod sniff 1 <seconds>` (channel 1 = the home AP's channel), first with firmware A, then A and B back to back with debug logging, each in a driver-swap session as SYSTEM. Machine rebooted beforehand (see "memory" below).

### Raw result (FACT)

**Post-boot (stage 6), firmware A and B alike**

- `GET_NIC_CAPAB`: 21 TLVs; MAC address present; PHY capability: 2 spatial streams, 2.4 and 5 GHz, HE; no chip-capability TLV.
- `EFUSE_BUFFER_MODE`, `PROTECT_CTRL`, `MAC_INIT_CTRL`: answered, result 0. `EFUSE_BUFFER_MODE` takes ~775 ms.
- **After firmware start, command responses arrive on RX ring 4** (before it, on ring 0). EXT responses carry event id 0xed, the capability response 0xec, UNI responses 0x01.
- Stage 6 total: ~1.27 s from cold.

**Sniffer start**

| Step | Firmware A (Windows package) | Firmware B (linux-firmware) |
|---|---|---|
| `SET_RX_PATH` (EXT) | result 0 | result 0 |
| `DEV_INFO_UPDATE` (first UNI command) | **no response at all in 3 s; no event of any kind on any ring** | result 0 after 5.9 ms |
| `BSS_INFO_UPDATE`, `SNIFFER` enable/config | not reached | result 0 |
| Tuning command chosen | `CHANNEL_SWITCH` (no CNM bit) – not reached | sniffer config (CNM bit set) |

**Reception with firmware B, channel 1, 8 s**

| Metric | Value |
|---|---|
| Frames | 10 652 (1 331 /s) |
| Per-frame channel field | channel 1 for all 10 652 |
| Beacons, good FCS | **116**, from 2 distinct SSIDs |
| Other frames | probe request/response, RTS, CTS, ACK and other control frames, 366 data frames |
| FCS errors / malformed / fragmented | 0 / 0 / 0 |
| FCS trailer present in frame bytes | 0 (frames are delivered without FCS) |
| Header-translated frames | 0 (raw 802.11 headers although translation is enabled in `MDP_DCR0`) |
| RSSI | −80 … −41 dBm |
| RX ring 2 (256 descriptors) seen full | 0 times |
| Shutdown | sniffer off, BSS/DEV off, MAC off all acknowledged; vendor driver rebound |

**Memory**

- Two attempts failed in the pre-flight (no chip access, Wi-Fi untouched): after 40 h of uptime no 2 MiB block below 4 GiB could be had even with 6.9 GiB free. "Shut down" with Fast Startup does not reset this; after a real restart 64 low blocks were seen.

### Conclusion

1. **Hypothesis confirmed with firmware B: the user-mode driver receives raw frames with a correct per-frame channel.** First half of the M3 acceptance criterion is met (beacons). AWDL frames from the iPhone are the remaining half.
2. **The Windows vendor firmware does not answer UNI commands** (ASSUMPTION about the cause: a different firmware branch without the unified command interface; FACT: silence). The project therefore needs the linux-firmware image, to be fetched from its official repository at install time; F-006's conclusion "vendor files are compatible" holds for download and post-boot only.
3. Open points of the specification now settled for firmware B: responses on ring 4 after boot; CNM path tunes the radio through the sniffer-config message; frames come without FCS and without header translation.
4. Session start needs low memory that long uptimes destroy: the service must reserve its DMA block early after boot.

### Follow-ups

- Same test on channels 6 and 44 with the iPhone's share sheet open: AWDL action frames (address 3 = AWDL BSSID). Record iOS version.
- Channel hop latency (`--hop`).
- Licence and source URL of the linux-firmware MT7961 files; add the fetch step to the installer plan.
- TODO-VERIFY: whether firmware A supports monitor reception through non-UNI commands (not needed if B is used).

---

## F-008 – DMA memory from scattered 4 KiB pages instead of one 2 MiB block

- **Date:** 2026-11-11
- **Milestone:** M3 (robustness of session start)

### Hypothesis

Free 2 MiB blocks below 4 GiB are too scarce to rely on, but single locked 4 KiB pages below 4 GiB are always available; the chip's rings and buffers can be laid out so that no object crosses a page, so a region built from scattered pages is sufficient.

### What would falsify it

Fewer than the ~231 pages the layout needs can be obtained below 4 GiB; or a descriptor array or buffer does not fit in one page.

### Method

Observed the block allocator across sessions; then changed the layout rules and the Windows HAL and ran the device-free self-test (`radiod dma-probe`) as SYSTEM in the state where the block allocator had just failed.

### Raw result (FACT)

| Situation | 2 MiB blocks below 4 GiB |
|---|---|
| 11 h uptime, 2.9 GiB free | 4–16 |
| 22 h uptime, 0.8 GiB free | allocation failed outright (error 1450) |
| 40 h uptime, 3.7–6.9 GiB free (also after a Fast Startup "shut down") | 0 |
| 4 min after a real restart | 64 |
| 14 min after a real restart | **0** |
| 20 min after a real restart, 8.5 GiB free | block allocator not retried; **paged allocator: 512 of 512 pages below 4 GiB**, none of them adjacent |

- Layout facts that make paging possible: every descriptor array is at most 4096 bytes (256 × 16), RX buffers are 2048 bytes and TX buffers 4096 bytes, all areas start on a page boundary. Enforced by compile-time assertions.
- The library takes an optional page table (`hal.dma_page_bus`); `mt792x_dma_bus` refuses any object that crosses a page. The simulated chip resolves bus addresses through the same table and flags anything outside a page. 75 tests pass, including full bring-up and sniffer start on a shuffled page table.

### Conclusion

1. Hypothesis confirmed in the device-free test. Sessions no longer depend on uptime or on a restart.
2. Not yet exercised on the chip: a bring-up with a paged region (next hardware session does that).
3. F-005's design conclusion ("fit everything into one low 2 MiB block") is superseded.

---

## F-009 – AWDL action frames from the iPhone received on Windows; channel hop measured (M3 acceptance)

- **Date:** 2026-11-11
- **Milestone:** M3 acceptance, part 2
- **iPhone:** iPhone 15 Pro, iOS 26.5.2, AirDrop "Everyone for 10 Minutes", Personal Hotspot off, share sheet open on the AirDrop screen
- **Firmware:** linux-firmware MT7961 image (RAM `____010000`, build 20231109). **DMA region:** 512 scattered 4 KiB pages (F-008), first use on the chip.

### Hypothesis

With the sniffer tuned to the channels the phone used under Linux (F-001: 44 and 6), the user-mode driver receives the phone's AWDL action frames (address 3 = `00:25:00:ff:94:73`, category 127, OUI `00:17:f2`), and a channel switch costs about as much as under Linux (~8 ms).

### What would falsify it

No frame with the AWDL BSSID on either channel while the share sheet is open; per-frame channel different from the tuned channel; switch latency of several slots (≫ 64 TU).

### Method

One driver-swap session as SYSTEM, three `radiod sniff` runs: channel 44 for 15 s, channel 6 for 15 s, then hopping 44 ↔ 6 every 300 ms for 20 s.

### Raw result (FACT)

| Run | Frames | AWDL action frames | Per-frame channel | Strongest RSSI bucket |
|---|---|---|---|---|
| Channel 44, 15 s | 153 | **149** | 44 for all | −40 … −31 dBm |
| Channel 6, 15 s | 389 | **127** | 6 for all | −40 … −31 dBm |
| Hop 44 ↔ 6, 20 s | 224 | **185** (106 while on 44, 118 while on 6) | always equal to the tuned channel | −40 … −31 dBm |

- Every frame carrying the AWDL BSSID was an AWDL action frame. No FCS errors, no malformed or fragmented packets, RX ring never full.
- The remaining frames: probe requests, CTS, a few data frames. No beacons: no access point nearby uses channels 6 or 44 (the tool's PASS rule counts beacons, so it printed FAIL; the rule is wrong for this test, the data is not).
- **Channel switch (sniffer-config command, time until its response): min 8.8 ms, median 10.1 ms, max 132 ms** over 62 switches; to channel 44 ≈ 10.5 ms, to channel 6 ≈ 9.0 ms. Two outliers (21 ms, 132 ms).
- Bring-up, post-boot and sniffer start worked unchanged on the paged DMA region (page bus addresses between 0x010EE000 and 0xC99B3000, no two pages adjacent).
- After the session the vendor driver rebound and Wi-Fi reconnected by itself.

### Conclusion

1. **M3 acceptance is met**: beacons (F-007) and AWDL action frames from the iPhone with the share sheet open, each verified by the per-frame channel.
2. The phone is heard on both channel 44 and channel 6 at roughly the same rate (≈ 8–10 action frames per second per channel), consistent with the F-001 sequences.
3. Channel switching costs ≈ 10 ms, the same order as Linux (7.7 ms median) – about 15 % of a 64 TU slot. Outliers exist and must be tolerated by the scheduler.
4. The paged DMA design (F-008) is confirmed on hardware.

### Follow-ups (M4)

- Raw TX of an 802.11 frame with a fixed rate, and TX status.
- **Firmware ACK for unicast frames addressed to our MAC in sniffer mode** – the main open risk (mt76 marks MT792x `no_active_monitor`, yet kernel 7.0 offered active monitor in F-001; find what it programs).
- The 2 trailing bytes on about half of the phone's action frames (F-001): inspect now that we can capture them.
- Fix the `radiod sniff` PASS rule: allow "AWDL frames on the tuned channel" as a success criterion.

---

## F-010 – First transmission: our probe requests are answered by access points; ACK still undecided

- **Date:** 2026-11-11
- **Milestone:** M4 (raw TX)
- **Firmware:** linux-firmware MT7961 image (RAM `____010000`, build 20231109). Channel 1 (2.4 GHz), OFDM 6 Mbit/s, power limits capped at 14 dBm.

### Hypothesis

A host-built 802.11 frame placed on TX ring 0 with mt76's TXD/TXP layout is radiated: a broadcast probe request with our MAC as source makes access points send probe responses addressed to our MAC. If the firmware acknowledges frames addressed to our MAC, those responses are not retransmitted.

### What would falsify it

The chip does not consume the TX descriptor or never returns the token; no probe response addressed to our MAC arrives. For the ACK part: every response sequence number is retransmitted up to the access point's retry limit.

### Method

`radiod txtest 1 --count 5` twice in one session: default interface setup (our MAC in own-MAC slot 0, sniffer on), then `--pair` (slot 0 = zero address with the sniffer, slot 1 = our MAC, as Linux had it in F-001). Five probe requests each, 1 s apart, 300 ms listening after each.

### Raw result (FACT)

| | Default (single) | `--pair` |
|---|---|---|
| Probe requests sent / completed | 5 / 5 | 5 / 5 |
| TX-free returned for every token | yes, status 0, 1 transmission, 0.7–2.8 ms after submit | yes, 0.7–9.4 ms |
| Lost / quarantined | 0 / 0 | 0 / 0 |
| Probe responses addressed to our MAC | **9**, from 1 access point | **7**, from 2 access points |
| Distinct response sequence numbers | 5 | 5 |
| …of which also seen with the Retry bit | 3 | 2 |
| Responses without Retry bit | 5 of 9 | 4 of 7 |

- `SET_RATE_TX_POWER` (8 messages) was sent without awaited responses; the chip transmitted afterwards.
- The frame used the management queue (ALTX), station index 19 (single) / 18 (pair), own-MAC index 0 / 1.
- Channel 1 is busy: ~300–1300 frames/s were seen there in the listening runs.
- Shutdown and vendor-driver rebind were normal after transmitting.

### Conclusion

1. **Transmission works**: frames built by the host leave the antenna and are decoded by access points; token completion works.
2. **ACK is undecided.** About half of the response sequence numbers had a retransmitted copy in both interface setups. That is fewer retransmissions than "never acknowledged" would suggest (an access point usually retries several times) but more than a clean link shows; on a busy channel collisions cause retries too. The two setups do not differ measurably.
3. The test needs a control: the same probe sent from an address the chip cannot acknowledge, to compare retransmission counts from the same access points in the same run.

### Follow-ups

- Control arm in `txtest` (`--control`), then repeat with more probes per arm.
- Unicast TX with ACK request and TX status report (TXS): does the access point acknowledge *our* unicast frames, and is that reported?
- Repeat on channel 44/6 with the iPhone once the AWDL layer can provoke unicast frames.

---

## F-011 – ACK control arm says the firmware acknowledges our MAC; the next run hung the whole system

- **Date:** 2026-11-11 (session), 2026-11-17 (analysis and mitigation)
- **Milestone:** M4 (raw TX)
- **Firmware:** linux-firmware MT7961 image (RAM `____010000`, build 20231109). Channel 1, OFDM 6 Mbit/s, power capped at 14 dBm. No iPhone involved.

### Hypothesis (written before the session, F-010 follow-up)

Probe responses addressed to our MAC are retransmitted less often than responses addressed to a random locally administered address that nobody acknowledges, if and only if the firmware ACKs frames to our MAC.

### What would falsify it

Equal retransmission counts (copies per (transmitter, sequence number) pair) in both arms, from the same access points in the same run.

### Method

One session, two commands: `txtest 1 @alt --count 15 --control` (single vif: our MAC in own-MAC slot 0), then the same with `--pair` (Linux plain + active vif pair). Probes alternate between the two arms, 1 s apart.

### Raw result, run 1 (FACT)

| | own MAC | control MAC |
|---|---|---|
| probe requests sent / TX-free returned | 15 / 15 | 15 / 15 |
| probe responses | 25 | 62 |
| distinct (transmitter, SN) pairs | 19 | 24 |
| copies per pair 1 / 2 / 3 | 15 / 2 / 2 | 3 / 4 / 17 |
| mean copies per pair | **1.32** | **2.58** |
| responses with the Retry bit | 44 % | 69 % |

Both arms were answered by the same 2 access points. Orderly shutdown afterwards (DMA stopped, WFSYS reset, exit code 0).

### Raw result, run 2 – system hang (FACT)

| time | event |
|---|---|
| 19:29:39 | run 2 (`--control --pair`) starts – the last line in `driver-swap.log` |
| — | no further line in any log; the script collected radiod's output and wrote it only after the process ended |
| 19:40 | Kernel-Power 41 after a forced power-off: **BugcheckCode 0**, no dump – a freeze, not a blue screen |
| 19:40–21:22 | four boots, all in Safe Mode: every normal boot hung before the event log started |
| 21:36 | first normal boot after the maintainer disabled the card in Device Manager (Safe Mode) |
| later | recovery: dedicated dump file configured, full power-off (`shutdown /s /f /t 0`, charger unplugged 30 s), leftover proxy package deleted, card re-enabled. Since then: no unexpected shutdown, card on the MediaTek driver, Wi-Fi connected (checked 2026-11-17) |

Also FACT, from the end-of-bring-up snapshots of all 16 radiod runs logged since F-006: in all 12 runs where the RAM firmware was running (`CONN_ON_MISC` = 3), `HOST_INT_ENA` read `0x20000000` (bit 29, "MCU command" interrupt); in the 4 runs that stopped before the firmware started it read 0. The host never writes a non-zero value there – the RAM firmware enables it itself. Every session wrote `MT_PCIE_MAC_INT_ENABLE = 0xff` (step S6, copied from mt76's probe). The proxy INF does not request MSI and the proxy has no interrupt handler.

### Conclusion

1. **ACK (run 1): supported.** Responses to our MAC needed half as many copies (1.32 vs 2.58) from the same access points in the same run, so the firmware acknowledges frames addressed to our MAC in the single-vif configuration. One run of 15 probes per arm; the `--pair` arm never ran. The question left open in F-010 is answered for the single-vif setup only.
2. **The hang cannot be located**: no line of run 2 survived. Run 2 was the first time `--pair` and `--control` were combined, but nothing shows that this combination, rather than chance, triggered it.
3. **Main hypothesis (ASSUMPTION): an unacknowledged hardware interrupt.** Every session armed both interrupt gates (S6 = 0xff, the firmware's bit 29). If the firmware raised the MCU-command interrupt (for example after an internal error) on a line-based (INTx) interrupt that no driver acknowledges, the line stays asserted and the CPU re-enters the interrupt path forever: a freeze without a bugcheck. This is open risk R1 of `docs/MT7921-BRINGUP.md`. Not proven: we do not know that an interrupt fired, nor whether Windows routes this device's INTx at all while the proxy is bound (TODO-VERIFY).
4. **Why later warm boots hung too (ASSUMPTION):** the chip kept a broken state across restarts (the M.2 slot stays powered on a warm restart) and the vendor driver hung on it at boot; a full power-off cleared it.

### Changes made before the next hardware run (2026-11-17)

- **Interrupts off at the source.** S6 now writes `MT_PCIE_MAC_INT_ENABLE = 0` (the value mt76 itself writes on suspend, `mt7921/pci.c:522`); option `pcie_mac_int` keeps mt76's 0xff for comparison. An interrupt guard reads `HOST_INT_ENA`, `HOST_INT_STA`, `MT_MCU_CMD` and `MT_PCIE_MAC_INT_ENABLE` after the firmware boots (S23), after post-boot, after sniffer start and every 250 ms while receiving, forces the enables back to 0 and logs what it found. Shutdown turns both gates off before stopping DMA; the proxy's stop list does the same.
- **Hand-back unchanged for the vendor driver:** after the WFSYS reset (RAM firmware gone, DMA stopped, `HOST_INT_ENA` 0) shutdown restores `MT_PCIE_MAC_INT_ENABLE` to the value found at S0 (0xff, MEASURED in F-004), and the stop list ends with the same write.
- **The stop list carries mt76's 50 ms WFSYS reset hold itself** (new op kind `MT792X_STOP_DELAY`); radiod no longer inserts it by position.
- **Crash-safe log.** `radiod --live-log <file>` writes every line except DEBUG messages through to disk before continuing (`FILE_FLAG_WRITE_THROUGH`); `s3-bringup.ps1` passes `<PrivateRoot>\radiod-live.log` and streams radiod's output into `driver-swap.log` line by line. After another hang the last line of `radiod-live.log` names the last step.
- radiod prints an "interrupt guard" summary for every command. 96 library tests (7 new for the guard; the fake chip now enables bit 29 at firmware start, as the hardware does), MSVC and GCC.

### Predictions for the re-entry runs (written before running them)

Each step only after the previous one passed, with a full power-off before the first.

| step | command(s) | prediction |
|---|---|---|
| A | `sniff 1 10 @alt` | PASS as in F-007; S6 logs 0; guard finds `HOST_INT_ENA` = `0x20000000` once (S23) and later only if the firmware re-arms it; `PCIE_MAC_INT_ENABLE` found 0 every time; Wi-Fi works after the give-back (the vendor driver starts with 0xff restored) |
| B | `txtest 1 @alt --count 5` | as F-010 (TX PASS) |
| C | `txtest 1 @alt --count 5 --pair` | as F-010 |
| D | the session that hung: `--count 15 --control`, then `--count 15 --control --pair` | no hang. If the hypothesis is right, `HOST_INT_STA` or `MCU_CMD` may show a cause bit during run 2 without any effect on the system |

What would falsify the hypothesis: a hang with the guard working (its last report in `radiod-live.log` shows both enables 0). Then the cause is elsewhere (DMA into memory we do not own, a PCIe completion timeout on a register read, a firmware state the vendor driver cannot survive) and the next step is to record which line came last.

---

## F-012 – Re-entry step A: no hang, but the chip stopped answering right after SET_OWN

- **Date:** 2026-11-17
- **Milestone:** M4 (F-011 re-entry ladder, step A)
- **Firmware:** linux-firmware MT7961 image (`@alt`). No iPhone involved.

### Prediction (F-011, step A)

`sniff 1 10 @alt` passes as in F-007; S6 logs 0; the interrupt guard finds `HOST_INT_ENA` = `0x20000000` once; Wi-Fi works after the give-back.

### Raw result (FACT)

- Preconditions first failed three times: "no System Restore point". Cause: drive C: had 0.15 GB free, so Windows had deleted the restore points. Fixed by the maintainer (space freed, restore point created); the script now names the free space in that error.
- The fourth start bound the proxy normally. S0 read `MT_HIF_REMAP_L1` = `0x18451844` (seen once before, 2026-11-10, in a session that passed). **S1 fw-own** read OWN_SYNC = 1 as always.
- **S2 drv-own:** the first LPCTL read after the CLR_OWN write returned `0xffffffff` (55 ms after S1). The library treated it as a dead bus and stopped after 1 attempt. Shutdown read all-ones from every register for another 90 ms.
- The proxy close ops and the give-back ran normally. The MediaTek driver came back, the card reports OK and lists 25 networks. **No system hang**; no new error events.
- Before this session Wi-Fi was not connected (the laptop was on iPhone USB tethering). In the 11.11 sessions it was connected right up to the swap. ASSUMPTION: an idle, disconnected vendor driver leaves the chip in a deeper power-save state, and after our SET_OWN the chip went to sleep with the link unreachable.

### Conclusion

1. In 17 earlier runs S2 succeeded at the first attempt in 4–9 ms; this is the first all-ones read at S2. Whether the chip was asleep or had dropped off the bus cannot be told from one read.
2. Our S2 was stricter than mt76: mt76 writes CLR_OWN up to 10 times (50 ms poll each), we gave up at the first all-ones read.
3. Nothing in step A tested the interrupt changes: the run ended before S6.

### Changes

- S2 (and the shutdown's drv-own) retry the CLR_OWN write after an all-ones read, inside mt76's own budget (10 attempts, about 0.5 s); no other register is written until the chip answers. The fake chip can now sleep after SET_OWN; tests for "wakes after 120 ms" and "never wakes" (gives up after 10 attempts, about 0.5 s).
- `MT_PCIE_MAC_INT_ENABLE` is no longer read at S0 but at S6, where it is changed, so S0–S3 touch exactly the registers they touched in the 17 runs that passed. A shutdown after a failure before S6 records the value first and hands it back unchanged.

### Prediction for the repeat of step A (written before running it)

With Wi-Fi connected before the session (as on 11.11): S2 at the first attempt, then the F-011 step-A prediction. If S2 again reads all-ones: either the log shows "chip answered again after N all-ones read(s)" and the run continues, or ten all-ones attempts end it in about 0.5 s. In that second case the chip is unreachable after SET_OWN and S1 itself has to be revisited.
