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

### Repeat of step A, Wi-Fi connected before the session (FACT, 2026-11-17 09:13)

- S2 drv-own at the first attempt in 4 ms (no all-ones read). One run: consistent with the assumption that an idle, disconnected vendor driver left the chip asleep, not proof of it.
- S6 wrote `PCIE_MAC_INT_ENABLE` = 0 (found 0xff). The interrupt guard found `HOST_INT_ENA` = `0x20000000` once, at S23, and never again in 41 checks over 10 s; `PCIE_MAC_INT_ENABLE` was never found non-zero. `MCU_CMD` stayed 0. `HOST_INT_STA` showed pending causes (`0x0c400005`, OR of all reads), which reached nobody because both gates were closed.
- **Reception unchanged:** `RESULT: PASS`, 4559 frames in 10 s, 480 beacons with good FCS, every per-frame channel = 1, no FCS errors (as F-007).
- Shutdown restored `PCIE_MAC_INT_ENABLE` to 0xff after the WFSYS reset. Give-back normal; the MediaTek driver reconnected to Wi-Fi; no error events.

**Conclusion:** keeping the interrupt gate closed does not disturb firmware download, post-boot, sniffer start or reception. Step A passed exactly as predicted. Next: step B (`txtest 1 @alt --count 5`).

---

## F-013 – Re-entry step B: raw TX works with interrupts gated off

- **Date:** 2026-11-17 (12:32)
- **Milestone:** M4 (F-011 re-entry ladder, step B)
- **Firmware:** linux-firmware MT7961 image (`@alt`). Channel 1, OFDM 6 Mbit/s, power capped at 14 dBm. No iPhone involved.

### Prediction (F-011, step B)

`txtest 1 @alt --count 5` behaves as in F-010 (TX PASS), no hang. Wi-Fi was connected before the session (F-012 precondition).

### Raw result (FACT)

- S2 drv-own at the first attempt in 7 ms. S6 wrote `PCIE_MAC_INT_ENABLE` = 0 (found 0xff). Bring-up to stage 6 in 994 ms; sniffer on channel 1, single vif.
- 5 probe requests: 5 TX-free records with status 0 and 1 transmission each, none lost, no unknown tokens.
- **31 probe responses to our MAC** from 8 transmitters (2 access points answered every probe); 23 without the Retry bit; 29 distinct sequence numbers, 7 with a Retry copy, 1 seen more than once. `TX: PASS`, `ACK: INCONCLUSIVE` (expected: this command has no control arm).
- Interrupt guard: 22 checks; `HOST_INT_ENA` found on once (`0x20000000`, at S23, forced to 0), `PCIE_MAC_INT_ENABLE` never found non-zero, `MCU_CMD` stayed 0; `HOST_INT_STA` OR of all reads `0x0c400015` (bit 4 new compared to step A, appeared right after the first TX).
- Orderly shutdown in 367 ms, `PCIE_MAC_INT_ENABLE` restored to 0xff. Give-back normal; the MediaTek driver reconnected (802.11ac, 5 GHz). Only system event: NDIS 10317 at the swap, which appears in every session since 2026-11-10 (informational).

### Conclusion

Step B passed as predicted: raw TX with both interrupt gates closed works as in F-010 (more responses than F-010 because more access points answered), and the system stayed responsive. The pending `HOST_INT_STA` bit 4 after TX reached nobody (TODO-VERIFY its meaning in mt76 headers; ASSUMPTION: TX-done/TX-free cause). Next: step C (`txtest 1 @alt --count 5 --pair`).

---

## F-014 – Re-entry step C: raw TX with the Linux vif pair works with interrupts gated off

- **Date:** 2026-11-17 (12:48)
- **Milestone:** M4 (F-011 re-entry ladder, step C)
- **Firmware:** linux-firmware MT7961 image (`@alt`). Channel 1, OFDM 6 Mbit/s, power capped at 14 dBm. No iPhone involved. Wi-Fi connected before the session.

### Prediction (F-011, step C)

`txtest 1 @alt --count 5 --pair` behaves as in F-010 (TX PASS), no hang.

### Raw result (FACT)

- S2 drv-own at the first attempt in 7 ms; S6 wrote `PCIE_MAC_INT_ENABLE` = 0 (found 0xff); bring-up to stage 6 in 1005 ms. Sniffer vifs: Linux pair (OMAC 0 = zero address, OMAC 1 = our MAC); `DEV_INFO_UPDATE 1` / `BSS_INFO_UPDATE 1` answered with result 0; frames went out on wcid 18, OMAC 1.
- 5 probe requests: 5 TX-free records with status 0 and 1 transmission each, none lost.
- **25 probe responses to our MAC** from 8 transmitters (3 access points this time); 17 without the Retry bit; 23 distinct sequence numbers, 7 with a Retry copy, 2 seen more than once. `TX: PASS`, `ACK: INCONCLUSIVE` (no control arm).
- Interrupt guard: identical to step B – 22 checks, `HOST_INT_ENA` on once at S23 (forced to 0), `PCIE_MAC_INT_ENABLE` never non-zero, `MCU_CMD` 0, `HOST_INT_STA` OR `0x0c400015`.
- Shutdown tore vif 1 down first, then vif 0; done in 418 ms, `PCIE_MAC_INT_ENABLE` restored to 0xff. Give-back normal, Wi-Fi reconnected (802.11ac). System events: NDIS 10317 and WLAN-AutoConfig 10002 at the swap (both informational, seen at earlier swaps).
- Preflight clock check measured 5.5 ms of sleeps as 20.0 ms (7.1 ms in step B): coarser system timer at that moment; no effect on the run.

### Conclusion

Step C passed as predicted. `--pair` alone, with both interrupt gates closed, neither disturbs TX nor hangs the system. Next: step D, the exact session that hung in F-011 (`--count 15 --control`, then `--count 15 --control --pair`).

---

## F-013 – Re-entry steps B, C, D: transmission and the session that hung, now with interrupts gated off – no hang

- **Date:** 2026-11-17 (12:32, 12:48, 12:51; three sessions, each with its own swap and give-back)
- **Milestone:** M4 (F-011 re-entry ladder, steps B–D)
- **Firmware:** linux-firmware MT7961 image (`@alt`). Channel 1, OFDM 6 Mbit/s, power capped at 14 dBm. No iPhone involved.

### Predictions (F-011, written before running)

B and C: as F-010 (TX PASS). D (the exact session that hung on 2026-11-11): no hang; `HOST_INT_STA` or `MCU_CMD` may show a cause bit without any effect on the system.

### Raw result (FACT)

| step | command | probes sent / TX-free returned | responses to our MAC | TX | ACK verdict |
|---|---|---|---|---|---|
| B | `txtest 1 @alt --count 5` | 5 / 5, lost 0 | 31 (8 transmitters) | PASS | INCONCLUSIVE (no control arm) |
| C | `txtest 1 @alt --count 5 --pair` | 5 / 5, lost 0 | 25 (8 transmitters) | PASS | INCONCLUSIVE (no control arm) |
| D1 | `txtest 1 @alt --count 15 --control` | 30 / 30, lost 0 | 88 | PASS | **WORKING** |
| D2 | `txtest 1 @alt --count 15 --control --pair` | 30 / 30, lost 0 | 96 | PASS | **WORKING** |

ACK control arms (all transmitters; the "answered in both arms" subset differs by at most one response):

| | D1 own MAC | D1 control | D2 own MAC | D2 control |
|---|---|---|---|---|
| responses | 88 | 158 | 96 | 212 |
| (transmitter, SN) pairs | 73 | 63 | 83 | 85 |
| copies per pair 1 / 2 / 3 / 4 / 5+ | 62 / 8 / 2 / 1 / 0 | 11 / 21 / 19 / 12 / 0 | 74 / 7 / 0 / 2 / 0 | 19 / 28 / 16 / 21 / 1 |
| mean copies per pair | **1.21** | **2.51** | **1.16** | **2.49** |
| responses with the Retry bit | 43 % | 75 % | 42 % | 76 % |
| transmitters answering both arms | 9 | | 10 | |

Interrupt guard, identical pattern in all four runs: `HOST_INT_ENA` = `0x20000000` found once (S23, firmware ready, forced to 0) and never again (22 checks in B and C, 119 / 121 in D1 / D2); `PCIE_MAC_INT_ENABLE` never found non-zero; `MCU_CMD` always 0; `HOST_INT_STA` OR of all reads `0x0c400015` (bit 4 appears once transmission starts; step A without TX showed `0x0c400005`).

Every run: exit code 0, orderly shutdown (DMA stopped, WFSYS reset, `PCIE_MAC_INT_ENABLE` restored to 0xff as found), vendor driver back (`S3 RESULT: PASS`). After B and C the give-back reported "not connected yet"; after D, "Wi-Fi connected". The only System-log warnings/errors since 12:00 are NDIS 10317 "Network Interface deleted while PNP Device still exists" at each swap – the same informational event seen at every swap since 2026-11-11. **No hang**, no unexpected shutdown.

### Conclusion

1. **TX works with interrupts gated off.** Both vif setups radiate and get answers, as in F-010. All tokens came back through TX-free on a polled ring – the interrupt gate is not needed for TX completion.
2. **ACK: confirmed for both vif setups.** Own MAC needs about half the copies of the control MAC (48 % single, 46 % pair), reproducing F-011 run 1 (1.32 vs 2.58) with 5× more data. This answers the `--pair` arm that never ran on 11.11. The firmware ACKs frames addressed to our MAC in both configurations; for AWDL this means the single-vif setup is sufficient (ASSUMPTION for AWDL data frames from an iPhone until seen).
3. **The hang did not repeat.** The exact session that hung (D) ran cleanly with both gates closed. This is consistent with hypothesis R1 (an unacknowledged interrupt) but **does not prove it**: the 11.11 hang happened once, so one clean run is weak evidence. The hypothesis was not falsified (the falsifier was a hang with the guard working). `HOST_INT_STA` does carry pending causes during TX that nobody services – on 11.11, with the gates open, those could have reached the CPU.
4. Decision: keep `MT_PCIE_MAC_INT_ENABLE = 0` as the default while the user-mode library owns the chip (polling only). Option `pcie_mac_int` stays for experiments; do not use it without a reason and a full power-off plan.

### Next

- Unicast TX with ACK request and a TX status record (TXS), F-010 follow-up: is a TXS delivered on a polled ring, and does it report the ACK from an access point? (H14/H15 of `docs/MT7921-BRINGUP.md`.)
- Then M5: OWL core on Windows over radiod.

---

## F-014 – Unicast TX with ACK request and TX status report (`radiod acktest`)

- **Date:** 2026-11-17 (predictions written before the run)
- **Milestone:** M4 (F-010 / F-013 follow-up)
- **Firmware:** linux-firmware MT7961 image (`@alt`). Channel 1, OFDM 6 Mbit/s, power capped at 14 dBm. No iPhone involved. iOS not relevant.

### Why

AWDL sends unicast data to a peer and needs to know whether it arrived. So far every frame we sent was broadcast (`NO_ACK`); whether the chip retries a unicast frame, and whether the firmware reports the outcome in a TX status record (TXS), is untested on hardware. The library implements both after mt76 (`TX_STATUS_HOST` with a PID >= 3, TXS0 bits 18:16 clear = ACKed; spec 12.7) and the fake chip behaves that way, which only shows the host side is consistent with our reading of mt76.

### Method

New command `radiod acktest 1 @alt` (default 5 frames per arm, 15 transmissions allowed per frame): listen 2 s for beacons, pick the BSSID with the best mean RSSI, then alternate one directed probe request per second between that access point (AP arm) and a random locally administered "ghost" address nobody owns (ghost arm). Every frame asks for an ACK and a TXS. Probe requests are management frames: fixed rate, management queue (ALTX), the vif's wcid (19 single / 18 pair); no association or keys are needed for an access point to ACK them. New library test `tx_directed_probe_ack_status_single_and_pair` checks the TXD fields and both outcomes against the fake chip.

### Predictions (written before running)

| quantity | prediction |
|---|---|
| `TX:` | PASS – the access point answers most directed probes with a probe response |
| `TXS:` | DELIVERED for every frame (ASSUMPTION; whether as `PKT_TYPE_TXS` packets or `MCU_EVENT_TX_DONE` events is open – the summary counts both) |
| AP arm | ACKed for >= 4 of 5 frames, transmissions 1 for most |
| ghost arm | NOT ACKed 5 of 5, transmissions 15 each (`RETRIES: AS REQUESTED`) |
| verdict | `UNICAST ACK: WORKING`; no hang, interrupt guard as in F-013 |

What would falsify it: no TXS at all (then ACK outcome can only be read from the TX-free COUNT, and AWDL must not rely on TXS); a ghost frame reported ACKed (TXS bits misread); NOT ACKed for frames the access point answered; ghost transmissions other than 15 (firmware applies its own retry limit for frames without a station record).

### Raw result (FACT, 2026-11-17 13:03, `acktest 1 @alt`, single vif, Wi-Fi connected before the session)

- Scan: 14 BSSIDs beaconing on channel 1 in 2 s; target = best mean RSSI (20 beacons, -58 dBm). Its address is locally administered (a virtual BSSID).

| frame | arm | TX-free status | transmissions | time to record | TXS word 0 | verdict |
|---|---|---|---|---|---|---|
| 1, 3, 5, 7, 9 | AP | 0 | 2, 1, 2, 1, 1 | 5.9–15.7 ms | `0x9c00804b` | ACKed |
| 2, 4, 6, 8, 10 | ghost | 1 | 15, 15, 15, 15, 15 | 14.0–57.9 ms | `0x9c01804b` | NOT ACKed |

- TXS records 10, unmatched 0 – one per frame. They arrived by **both** routes: 5 as `PKT_TYPE_TXS` packets and 5 inside `MCU_EVENT_TX_DONE` (0x0f) events. Which frame used which route was not logged (DEBUG only).
- TXS word 0 differs only in bit 16 (`ACK_TIMEOUT`); `TX_RATE` = `0x04b` = OFDM 6 Mbit/s (mode 1, idx 11) as requested.
- TX-free `STATUS` = 1 for every unacknowledged frame, 0 for every acknowledged one – a second, independent ACK indicator.
- The target answered **none** of the directed (wildcard-SSID) probes with a probe response.
- Summary lines: `TXS: DELIVERED - 10 of 10`, `UNICAST ACK: WORKING - access point ACKed 5 of 5, ghost NOT ACKed 5 of 5`, `RETRIES: AS REQUESTED`, exit code 0. The `TX:` line said FAIL because it only counted probe responses (fixed afterwards, see Changes).
- Interrupt guard as in F-013 (`HOST_INT_ENA` once at S23, 48 checks, `PCIE_MAC_INT_ENABLE` never non-zero). Give-back normal; the vendor driver reconnected (to a 5 GHz network on channel 104); no new error events apart from the usual NDIS 10317 at the swap. No hang.

### Conclusion

1. **Unicast TX works, with retries and an honest TX status.** All predictions held except the probe-response one. The firmware retries an unacknowledged unicast frame exactly `REM_TX_COUNT` times (15 of 15) and reports the outcome three ways that agree: TXS `ACK_TIMEOUT` bit, TX-free `STATUS`, TX-free `COUNT`. The ghost arm rules out a TXS that always says "ACKed".
2. **TXS needs no station record.** The frames went out on the monitor vif's wcid (19) without `STA_REC`; TXS still arrived for every frame. Both delivery routes occur, so the host must keep handling both (it does).
3. The AP needed a second transmission for 2 of 5 frames: normal on a busy channel; it shows the retry path also helps acknowledged traffic.
4. A virtual / hidden BSSID may never answer a wildcard probe; the AP's ACK (with a never-ACKed ghost as control) is the reliable proof of reception.
5. Open: rate choice for unicast **data** frames without `FIX_RATE` (H14) – only management frames were tested here; AWDL data frames will exercise it in M5.

### Changes

- `radiod acktest`: `TX: PASS` now also when the access point ACKed and the ghost never was (reasoning in point 4).

---

## F-015 – M5.1: the radio server (`radiod serve`) and its pipe on hardware

- **Date:** 2026-11-17 (predictions written before the run)
- **Milestone:** M5.1 (ADR-0003)
- **Firmware:** linux-firmware MT7961 image (`@alt`). Channel 44, 14 dBm cap. iPhone 15 Pro, iOS 26.5.2, share sheet open (AirDrop, "Everyone for 10 Minutes").

### Why

ADR-0003 splits the radio (`radiod serve`, MSVC, owns the chip) from AWDL (`awdld`, MinGW, OWL). Before OWL sits on top, the pipe must carry real AWDL traffic with the right per-frame channel and return honest TX statuses.

### Offline evidence already in hand (FACT)

- `radio_link_test` passes with MSVC (`/W4 /WX`) and GCC 16.2 (`-std=c11 -Wall -Wextra -Werror -pedantic`): header and struct round trips, radiotap layout byte by byte (alignment of TSFT / CHANNEL, BADFCS, MCS), OWL's own TX radiotap header (RATE only, length 9) parsed, malformed inputs rejected.
- `radiod serve --loopback` (synthetic radio, no hardware) with `radioctl`: HELLO, 81 of 161 synthetic frames forwarded (the 80 foreign beacons filtered by BSSID), 6/6 TX statuses as expected (broadcast: done, no ACK requested; ghost: NOT ACKed, 15 transmissions), channel switch answered, time limit ends the session with BYE, a silent client is dropped after the 5 s lease, an unelevated client is refused by the pipe ACL (error 5) outside loopback.

### Method

`s3-bringup.ps1 -Commands serve:44:@alt:--max-seconds:60 -Client --seconds:40:--tx-test`: `radioctl` starts first and waits for the pipe; `radiod serve` brings the radio up on 44 with the chip's own MAC, accepts the client, serves until `radioctl` disconnects after 40 s. During those 40 s the maintainer keeps the iPhone's AirDrop share sheet open next to the laptop.

### Predictions (written before running)

| quantity | prediction |
|---|---|
| `radioctl` RX | PASS: AWDL action frames (PSF and MIF) from the iPhone through the pipe, every per-frame channel 44 (F-008 saw 149 in 15 s on 44 with the share sheet open) |
| `radioctl` TX | PASS 6/6: broadcast probes done with no ACK requested; ghost probes NOT ACKed after 15 transmissions (as F-014) |
| server | `RESULT: PASS`, pipe full 0, no TX lost, interrupt guard as F-013/F-014 |
| system | no hang; vendor driver back afterwards |

What would falsify it: AWDL frames seen by `radiod sniff` but not through the pipe (filter or radiotap bug); per-frame channel other than 44; TX statuses missing or different from F-014 when sent through the pipe; pipe full on a quiet channel (server loop too slow).

### Raw result (FACT, 2026-11-17 17:56, `serve:44:@alt:--max-seconds:60` + `radioctl --seconds 40 --tx-test`)

- `radioctl` connected 0.6 s after the radio was up on 44 (chip MAC, AWDL filter). In 40 s: **367 AWDL action frames through the pipe (276 PSF, 91 MIF), all from one transmitter (the iPhone), every per-frame channel = 44 (5220 MHz)**, RSSI -42..-23 dBm; no data frames (no AWDL peer yet).
- Server: 391 frames taken from the chip, 368 forwarded, 23 filtered (not the AWDL BSSID), 0 bad FCS, 0 own, **0 pipe full**; client messages 46 (heartbeats, TX, STATS).
- TX through the pipe, 6/6 as expected: broadcast probes done, no ACK requested, 1 transmission, 0.6–2.2 ms; ghost probes **NOT ACKed after 15 transmissions**, 9.6–9.9 ms (on channel 1 in F-014: 14–58 ms – a quieter channel).
- `RESULT: PASS` on both sides; session ended by the client's disconnect after 40 s; orderly shutdown; interrupt guard as F-013 (162 checks, `HOST_INT_ENA` only at S23); vendor driver back and Wi-Fi connected 26 s after the end. No hang.

### Conclusion

All predictions held. The radio server delivers the iPhone's AWDL traffic with the correct per-frame channel and radiotap, keeps up easily (~9 AWDL frames/s, no queue overflow), and TX statuses survive the pipe unchanged. M5.1 gate passed. Next: M5.2 (does the firmware ACK a random locally administered address set as the vif address?) and M5.3 (build `awdld` with MinGW, offline against `radiod serve --loopback`).

---

## F-016 – M5.2: does the firmware ACK a random locally administered vif address?

- **Date:** 2026-11-17 (predictions written before the run)
- **Milestone:** M5.2 (ADR-0003 decision 7)
- **Firmware:** linux-firmware MT7961 image (`@alt`). Channel 1, 14 dBm cap. No iPhone needed.

### Why

Apple devices use a fresh random locally administered address for AWDL, and so should we (privacy: the chip's burnt-in MAC would be broadcast in every AWDL frame). The firmware must then ACK unicast frames to that address, or every AWDL data frame from the iPhone is retried to death. F-013/F-014 proved ACK for the chip's own MAC only.

### Method

New shared option `--mac random` (`txtest`, `acktest`, `serve`): a random locally administered unicast address is given to the sniffer start (`cfg.mac` → `DEV_INFO` of the vif; the library's TX address check uses it too). One session, `txtest 1 @alt --count 15 --control --mac random`, then the same with `--pair`: probe requests alternate between our (random) address and a second random control address nobody acknowledges; the access points' retransmission of their probe responses tells whether our address was ACKed (F-011 method).

### Predictions (written before running)

| run | prediction |
|---|---|
| single vif, random address | `TX: PASS`; `ACK: WORKING` – mean copies per pair about 1.2 (own) vs about 2.5 (control), ratio <= 60 % (as F-013 with the chip MAC) |
| pair, random address | the same |

What would falsify it: `ACK: NOT WORKING` or own ≈ control (the firmware ACKs only its efuse address, or needs more than `DEV_INFO` – e.g. a `STA_REC`/`BSS_INFO` address – to learn a new one). Then AWDL falls back to the chip MAC (ADR-0003 decision 7) and the README states the privacy cost.

### Run 1 (FACT, 2026-11-17 18:00, channel 1) – inconclusive: no access point on the channel

- Random address `fe:ae:c7:…` accepted: `DEV_INFO_UPDATE` / `BSS_INFO_UPDATE` result 0, sniffer running. 30/30 probe requests completed (TX-free status 0, 1 transmission; probe 6 completed after its 300 ms window).
- **0 probe responses in both arms** – including the control arm, which got 158 in F-013. The second command (`--pair`) did not run (the script stops at the first failure).
- The laptop had moved: after the give-back Windows connected on **channel 2**, and the scan listed only 4 BSSIDs (2 on channel 2, 1 on 11, 1 on 36) – **none on channel 1**. A missing answer from nobody says nothing about our TX or ACK.
- Change: `txtest` now prints how many beacons (and BSSIDs) it heard on the channel, so "no access point" and "TX broken" can be told apart.

### Prediction for the repeat (channel 2, where 2 BSSIDs beacon; both commands, `-KeepGoing`)

As above: `ACK: WORKING` for the random address in single and pair mode. With only 2 access points the verdict may be `INCONCLUSIVE - too little data` (< 5 pairs per arm from common transmitters); that would call for a run with `--count 20` or a busier place, not a conclusion.

### Repeat on channel 2 (FACT, 2026-11-17 18:17, at home, `--count 20`, `-KeepGoing`)

| | single vif, random `5a:bd:98:…` | pair, random `22:a6:8d:…` |
|---|---|---|
| beacons heard on channel 2 | 739 from 2 BSSIDs | 741 from 2 BSSIDs |
| responses own / control | 39 / 106 | 39 / 117 |
| (transmitter, SN) pairs own / control | 33 / 34 | 34 / 38 |
| copies per pair own: 1 / 2 / 3 / 4 / 5+ | 31 / 1 / 0 / 0 / 1 | 33 / 0 / 0 / 0 / 1 |
| copies per pair control: 1 / 2 / 3 / 4 / 5+ | 1 / 6 / 26 / 0 / 1 | 0 / 3 / 34 / 0 / 1 |
| **mean copies own vs control** | **1.18 vs 3.12 (38 %)** | **1.15 vs 3.08 (37 %)** |
| verdict | `ACK: WORKING` | `ACK: WORKING` |

Both access points answered both arms. Interrupt guard as F-013; give-back normal, Wi-Fi connected; no hang.

### Conclusion

**The firmware ACKs a random locally administered address given to the vif through `DEV_INFO`**, in both vif setups, as clearly as the chip's own MAC (F-013: 48 % / 46 %; here 38 % / 37 %, on a quieter channel where the control arm shows the access points' full retry count of 3). ADR-0003 decision 7 holds: `awdld` uses a fresh random address per session, set with `serve --mac random`. M5.2 gate passed.

---

## F-017 – M5.3: awdld builds with MinGW and runs against the loopback radio (offline)

- **Date:** 2026-11-17
- **Milestone:** M5.3 (ADR-0003). No hardware.

### Result (FACT)

- `awdld/` = jedbillyb/owl @ 57b8678 (`src/`, `daemon/core.c`, tests) + radiotap-library @ 94984dd + winawdl's Windows layer @ 0db7031 adapted to our radio link + own `radio_client.c`. Builds with MinGW-w64 GCC 16.2 (WinLibs, UCRT), `-Wall -Wextra -Werror` on our and winawdl's files, statically linked.
- Tests: winawdl's 5 Windows-layer tests, `radio_link_test` built with GCC, **OWL's own googletest suite: 68 of 68 pass** after two portability fixes outside OWL: `<string.h>` added to the force-included `compat/owl_win.h` (OWL's `core.c` relied on glibc including it), and `compat/ether.c` printing addresses like glibc (`%x`, no leading zeros), which two OWL tests assert. OWL's `daemon/` include path is limited to the `awdld` target (its `io.h` shadowed the system `<io.h>` that googletest needs).
- `radiod serve 44 - - --loopback --mac random` + `awdld -c 44 -n -w 5`: awdld takes its AWDL address from HELLO (the random one), sets the AWDL BSSID filter, requests channel 44, receives the synthetic PSFs (the fake peer appears, correctly never valid: the frames carry no TLVs), and transmits ~27 action frames/s (PSF/MIF) through the pipe – 216 in 8 s, none refused; the server ends cleanly when awdld exits.
- Found and avoided: winawdl's log redaction hides everything after an address up to the line's **last** bracket (deliberately – peer names may contain brackets). Our longer status line therefore lost its radio counters under `-r`; the radio counters now come first and the sync tree last, and `redact_test` has a case for that line.

### Not covered offline

Wintun (`awdl0`) – needs the signed `wintun.dll` and elevation; OWL against a real iPhone (sync, peer validity, data frames). Both are M5.4.

---

## F-018 – M5.4: awdld + radiod serve with the iPhone (sync, peer, ping over awdl0)

- **Date:** 2026-11-17 (predictions written before the run)
- **Milestone:** M5.4 (ADR-0003; M5 acceptance criterion of CLAUDE.md §7)
- **Setup:** `radiod serve 44 @alt --max-seconds 120 --mac random`; `awdld -c 44 -S intersect -v` (script: `-Awdl +c:44:+S:intersect:+v`) with the signed `wintun.dll` 0.14.1 (zip SHA-256 `07c25618…ef51`, Authenticode valid, WireGuard LLC) beside it; the Wintun driver 0.14.0.0 is already installed (by Tailscale), awdld creates its own adapter `awdl0` (tunnel type `mtdrop`, own GUID) and removes it on exit. iPhone 15 Pro, iOS 26.5.2, AirDrop "Everyone for 10 Minutes", share sheet open for the whole session. The session script pings the first peer awdld reports reachable (`ping -6 -n 20`).

### Why `-S intersect` and not the fork's default `verbatim`

`verbatim` copies the sync master's whole sequence, so OWL would switch the radio every ~16 ms across 6, 44 and 149 (we may not transmit on 149: TX allow-list) at ~10 ms per switch. `intersect` advertises only the iPhone's slots on our channel 44 and leaves the others empty, so the radio never leaves 44 (`awdl_switch_channel` skips empty slots) and the sequence never claims a slot we cannot serve (ADR-0002 §5 / ADR-0003 decision 6: fixed 44 first). The jedbillyb rig measured intersect working with iOS 26 behind a P2P-GO (~73 kB/s, their FINDINGS §47).

### Predictions (written before running)

| quantity | prediction |
|---|---|
| connect | awdld takes the random address from HELLO, Wintun adapter `awdl0` created, BSSID filter set |
| peer | the iPhone appears as a peer within seconds of opening the share sheet and becomes **valid** (winawdl's MIF trailing-zero fix is in `frame_fixup.c`) |
| sync | a sync master is elected (the iPhone or us) and logged |
| sequence | `intersect` keeps >= 1 of 16 slots on 44 (winawdl: an idle iPhone lists 44 for one slot), no "NO OVERLAP" for the whole session |
| radio | `rx dropped 0`, `refused 0`, no channel switches after the first |
| ping | **uncertain** (TODO-VERIFY whether iOS answers ICMPv6 over AWDL at all): ASSUMPTION some replies, with loss (jedbillyb: 60 % loss with verbatim on Linux) |
| system | no hang; vendor driver and Wi-Fi back; `awdl0` gone afterwards |

What would falsify it: the iPhone never becomes a valid peer (frame format or MIF parsing), "NO OVERLAP" throughout (iPhone never lists 44 → hopping needed), radio TX refused or dropped frames, `awdl0` left behind.

### Raw result (FACT, 2026-11-17 18:33, home, channel 44, 120 s; device addresses and names omitted)

- Radio server: 1233 frames taken, 1002 forwarded (AWDL), 231 filtered, 0 bad FCS, **0 pipe full**; 2840 TX submitted, **0 refused, 0 lost**; one channel switch (44, at start); interrupt guard as F-013; `RESULT: PASS`.
- awdld: random address from HELLO; **Wintun adapter `awdl0` created (interface index 13)** and gone after the session (no leftover device); BSSID filter set.
- **The iPhone appeared as a peer 1 s after awdld started, was reported reachable (`fe80::…%13`) at once, became the elected sync master, and awdld followed its sequence** (`following channel sequence of sync master …, enc 3`). `intersect: 2/16 slots overlap … on ch 44` throughout – never NO OVERLAP. The peer timed out once (18:34:03, 2 s without a frame) and was re-added 3 s later (new advertised name UUID, same address).
- awdld sent ~21 action frames/s and 227 data frames: 35 unicast (ICMPv6 echo requests of the ping and neighbour traffic), the rest multicast. **All 35 unicast frames: NOT ACKed after 15 transmissions. 0 ACKed.** Received data frames from the iPhone: **0**.
- `ping -6 -n 20` to the peer: **20 of 20 lost**.
- Give-back normal (Wi-Fi reconnected after the log's 30 s window); no hang.

### Conclusion

1. Held: the whole chain radio → pipe → OWL works with a real iPhone – peer discovery, peer validity (winawdl's fixes), sync to the iPhone as master, sequence adoption with intersect on 44, Wintun `awdl0`, no frame lost on our side.
2. **Failed: unicast to the iPhone.** None of 35 unicast frames was acknowledged, and the iPhone sent us no data. The firmware ACK/TXS path is proven (F-014, F-016), so a NOT ACKed after 15 tries means the iPhone did not receive the frame on 44 at that moment.
3. Main hypothesis (ASSUMPTION): **our idea of the iPhone's slot timing is off.** OWL does not use the radiotap TSFT for synchronisation: `owl/src/rx.c:493` sets `tsft = clock_time_us()` when the frame is processed (a TODO in OWL). On Linux that is within ~1 ms of reception; here the frame first crosses radiod's poll loop, the pipe, the client's reader thread and the event loop, so every PSF/MIF is dated late by an unknown, variable amount, and OWL places the iPhone's 44 slots (2 × 16 TU of a 16-slot period) later than they are. Unicast is sent only in those slots (`txgate`), so a phase error of more than a slot misses all of them. Unicast TX latency (awdld → pipe → radiod → TX ring) adds to the error in the same direction.
4. Alternatives not excluded: the iPhone does not stay on 44 for its whole 44 slots; the firmware's rate choice for unicast data (H14) is not decodable; the iPhone ignores a peer whose own sequence (intersect: only 2 slots) it finds unacceptable – but a NOT ACKed is a MAC-level miss, which points at timing or channel, not at peer policy.

### Next (M5.4b, measure before changing anything)

- awdld: per received frame, `clock_time_us()` at processing minus radiotap TSFT (radiod's QPC stamp when the frame left the RX ring), as count / mean / max in the status line – the RX dating error, directly.
- radiod serve `--pcap <file>` (private directory only): every forwarded RX frame and every TX with QPC timestamps, plus a TX status log, so the iPhone's slot phase (from its sync parameters) can be compared offline with when our unicast frames went out and whether they were ACKed.
- Then decide between: feeding the radiotap TSFT to OWL's synchronisation (a one-line change in `rx.c`, recorded in NOTICE), reducing latency, or both.

---

## F-019 – M5.4b: is it the clock or the data frames? (RX dating measured; unicast DATA TX tested)

- **Date:** 2026-11-17
- **Milestone:** M5.4b (follow-up of F-018)

### RX dating error, offline (FACT, loopback)

awdld now prints, per status period, `clock_time_us()` at processing minus radiod's radiotap TSFT. Against `radiod serve --loopback`: min 26 µs, mean 43–142 µs, max 2.0 ms over ~20 frames per 2 s. Both clocks are QPC-based (small positive values) and the pipe path adds a fraction of a millisecond – far below a 16 TU (16.4 ms) slot. **This weakens the F-018 timing hypothesis**; the same numbers from a hardware run will confirm or refute it (the line is in every hardware log from now on). awdld's TX cookie now carries the low 32 bits of the QPC time of sending, for the same measurement on the TX side.

### New suspect

F-014 / F-016 proved ACK and TX status for unicast **management** frames, which carry a fixed rate. awdld's unicast frames are **data** frames: mt76 gives those no `FIX_RATE`, so the firmware picks the rate from the WLAN table entry of the vif's wcid – an entry we never fill (no `STA_REC`; open point H14). Unicast data TX has never been tested on hardware.

### Method

`acktest` gained `--data` (unicast data frames shaped like OWL's AWDL data frames: data, no QoS, address 3 = AWDL BSSID, AWDL data header) and the shared option `--data-fixed-rate` (`opt.tx_data_fixed_rate`: FIX_RATE = `--rate` on unicast data too; also accepted by `serve`). One session at home on channel 2, `-KeepGoing`, all with a random address as awdld uses:
1. `acktest 2 @alt --mac random` – probe requests, baseline (F-014);
2. `acktest 2 @alt --mac random --data` – data, firmware rate (what awdld sent in F-018);
3. `acktest 2 @alt --mac random --data --data-fixed-rate` – data, OFDM 6 fixed.
The access point ACKs any unicast frame addressed to it with a good FCS, associated or not (it may answer data from a stranger with a deauthentication, which is harmless). Each frame's TXS shows the rate actually used.

### Predictions (written before running)

| run | prediction |
|---|---|
| 1 probes | AP ACKed >= 4/5, ghost 0/5 (as F-014) |
| 2 data, firmware rate | ASSUMPTION (the suspect): AP **NOT** ACKed, TXS rate unusual |
| 3 data, fixed rate | AP ACKed >= 4/5 like run 1 |

If run 2 is ACKed like run 1, the data path is fine and the timing hypothesis returns (next: `radiod serve --pcap` and an offline slot-phase analysis). If run 2 fails and run 3 passes, `serve` gets `--data-fixed-rate` and M5.4 is repeated.

### Raw result (FACT, 2026-11-17 18:43, home, channel 2, random address, `-KeepGoing`)

| run | AP ACKed | ghost NOT ACKed (15 tries) | TXS rate | time from submit to completion, AP frames | ghost frames |
|---|---|---|---|---|---|
| 1 probe requests | 5/5 (+5 probe responses) | 5/5 | `0x04b` OFDM 6 | 5.7–14 ms (one 94 ms) | 9.9–12.2 ms |
| 2 data, firmware rate | **5/5** | 5/5 | **`0x000` CCK 1 Mb/s** | **148–390 ms** | **166–558 ms** |
| 3 data, fixed rate | 5/5 | 5/5 | `0x04b` OFDM 6 | **134–415 ms** | **116–412 ms** |

TXS word 0 of data frames: `0x0000800x` (firmware rate) / `0x8000804b` (fixed) against `0x9c00804b` for management frames. Give-back normal, no hang.

### Conclusion

1. **Prediction for run 2 falsified:** the access point acknowledges our unicast data at the firmware's rate. The data path itself works, with TX status and retries as for management frames.
2. **Found instead: unicast data waits 115–560 ms in the chip** before its first transmission, at any rate (runs 2 and 3), against 6–14 ms for management frames. Data goes to an AC queue (BE, Q_IDX 1), management to ALTX0. For AWDL this alone explains F-018: the iPhone is on 44 for 2 windows of 16 TU in a 16-slot period (~262 ms), so a frame leaving at a random moment hundreds of ms after OWL chose the right window mostly finds nobody listening → NOT ACKed after 15 quick retries, for all 35 frames. (ASSUMPTION for why: the firmware schedules AC queues per station, and the vif's wcid has no `STA_REC`.)
3. **Also found: the firmware's rate for unicast data is CCK 1 Mb/s** (`0x000`) – fine on 2.4 GHz, impossible on 5 GHz (channel 44). What the firmware does with it on 5 GHz is untested.
4. The F-018 timing hypothesis (RX dating) is not needed to explain F-018; the measured offline dating error is ~0.1 ms anyway.

### Change

Library option `opt.tx_data_altx` ([HOST], not mt76): data frames on ALTX0 with their TID, ACK and TX status unchanged (test `tx_data_on_altx_queue`); `radiod --data-altx` for `acktest`, `txtest` and `serve`.

---

## F-020 – Unicast data on the ALTX queue, and the firmware's data rate on 5 GHz

- **Date:** 2026-11-17 (predictions written before the run). Home: channel 2 (2 BSSIDs), channel 36 (1 BSSID). No iPhone.

### Method

One session, `-KeepGoing`, random address, `acktest --data` against the strongest access point:
1. channel 2, `--data-altx` (firmware rate)
2. channel 2, `--data-altx --data-fixed-rate` (OFDM 6)
3. channel 36, `--data` (firmware rate, AC queue: what rate does the firmware use on 5 GHz?)
4. channel 36, `--data-altx --data-fixed-rate`

### Predictions

| run | prediction |
|---|---|
| 1 | AP ACKed 5/5, completion within ~6–20 ms (like probes), rate still `0x000` |
| 2 | AP ACKed 5/5, ~6–20 ms, rate `0x04b` |
| 3 | ASSUMPTION: the firmware's CCK choice on 5 GHz fails – AP NOT ACKed, or a rate other than CCK in TXS; completion slow (AC queue) |
| 4 | AP ACKed 5/5, ~6–20 ms, rate `0x04b` |

If 2 and 4 hold, `serve` runs with `--data-altx --data-fixed-rate` and M5.4 is repeated with the iPhone.

### Raw result (FACT, 2026-11-17 18:49, home, random address, `-KeepGoing`)

| run | AP ACKed | ghost | TXS rate | completion, AP frames | ghost frames |
|---|---|---|---|---|---|
| 1 ch 2, ALTX, firmware rate | 5/5 (1 transmission each) | NOT ACKed 5/5, 15 tries | `0x000` CCK 1 | **27–117 ms** | 49–102 ms |
| 2 ch 2, ALTX, fixed OFDM 6 | 5/5 (1 each) | NOT ACKed 5/5 | `0x04b` | **5.9–12.7 ms** | 10.2–14.9 ms |
| 3 ch 36, AC, firmware rate | **0/5 – NOT ACKed after 15 tries** | NOT ACKed 5/5 | **`0x000` CCK 1** | 106–158 ms | 147–223 ms |
| 4 ch 36, ALTX, fixed OFDM 6 | **5/5 (1 each)** | NOT ACKed 5/5 | `0x04b` | **5.9–6.5 ms** | 10.4–18.3 ms |

The access point on 36 was weak (-79 dBm) – the same in runs 3 and 4, so run 4's first-try ACKs rule out distance as the cause of run 3. Give-back normal, no hang.

### Conclusion

1. **Unicast data needs both changes:** with `--data-altx --data-fixed-rate` data frames leave in ~6 ms and are ACKed at the first try on 2.4 and 5 GHz, exactly like management frames.
2. **On 5 GHz the firmware's own rate choice for unicast data is unusable:** it picks CCK 1 Mb/s there too, and nothing is received (run 3; prediction held). This alone makes every unicast frame to the iPhone on channel 44 fail.
3. ALTX alone shortens the wait (27–117 ms vs 115–560 ms on the AC queue) but the firmware-rate path stays slow; with a fixed rate the wait disappears. (ASSUMPTION: the firmware's rate lookup for an empty WLAN entry costs the extra time.)
4. Together with F-019 this explains F-018 completely: awdld's unicast frames to the iPhone went out at CCK 1 Mb/s on a 5 GHz channel, and hundreds of milliseconds late.

---

## F-021 – M5.4 repeated with unicast data fixed (ALTX + fixed rate)

- **Date:** 2026-11-17 (predictions written before the run)
- **Setup:** as F-018, but `radiod serve 44 @alt --max-seconds 120 --mac random --data-altx --data-fixed-rate`. Unicast data then goes out at the rate in OWL's radiotap header (12 Mb/s, OFDM) from ALTX0. iPhone share sheet open for the whole session.

### Predictions

| quantity | prediction |
|---|---|
| peer, sync, intersect | as F-018 (valid peer within seconds, iPhone as sync master, 2/16 slots on 44) |
| RX dating error (new line in the status) | mean below 1 ms, max a few ms (confirms the offline measurement on hardware) |
| unicast TX to the iPhone | **most frames ACKed** (some NOT ACKed where OWL's window and the iPhone's presence do not overlap) |
| data from the iPhone | **> 0 data frames received** (neighbour discovery / echo replies) |
| ping over `awdl0` | **at least some replies** (TODO-VERIFY that iOS answers ICMPv6 at all; jedbillyb saw 40 % replies on Linux) |

What would falsify the fix: still 0 ACKed unicast frames with TXS rate `0x04b`/OFDM 12 and ~6 ms completion – then the remaining cause is timing (back to slot phase analysis).

### Raw result (FACT, 2026-11-17 18:56, home, channel 44, 120 s; device addresses omitted)

- **Ping over `awdl0` to the iPhone's link-local address: 17 of 20 replies (15 % loss), RTT 46–415 ms.** First reply to the first request.
- Unicast TX to the iPhone: **18 ACKed, 3 NOT ACKed** (F-018: 0 of 35). **18 data frames received from the iPhone** (F-018: 0).
- Peer added 1 s after start, iPhone sync master within 2 s, **no peer timeout during the whole session** (F-018 lost it once), intersect on 44, one channel switch (at start).
- Radio server: 1371 frames taken, 1318 forwarded, 0 bad FCS, 0 pipe full; 2936 TX submitted, 0 refused, 0 lost.
- **RX dating error on hardware** (status periods of 2 s, ~20 frames each): mean 0.17–1.3 ms and max 0.36–12.6 ms in most periods, but spikes of mean 3.8–11.8 ms / max 56–131 ms in three periods, and 0.8 s mean / 2.5 s max in the first period (frames queued while awdld was starting).
- Give-back normal; Wi-Fi reconnected after the log's window; no hang; `awdl0` removed.

### Conclusion

1. **The M5 acceptance criterion (CLAUDE.md §7) is met:** AWDL sync with the iPhone, peer discovered and valid, and `ping` over `awdl0` link-local to the iPhone works – and iOS does answer ICMPv6 over AWDL (TODO-VERIFY closed).
2. The F-019/F-020 fix was the missing piece: with unicast data on ALTX0 at a fixed OFDM rate, the iPhone ACKs our frames and talks back. `radiod serve` now uses both by default (`--data-fw-rate-ac` restores mt76's behaviour for experiments).
3. Open, for throughput later: RTTs grow in runs of 5–10 pings (78 → 415 ms, then back to ~160 ms), i.e. the echo waits for one of our two 44-slots per 16-slot period; occasional RX dating spikes up to 131 ms (the event loop was busy – ASSUMPTION – which can skew OWL's sync readings); OFDM 12 Mb/s fixed rate. None of these blocks M6.

---

## F-022 – M6a: first AirDrop receive over our own stack (auto-accept)

- **Date:** 2026-11-17 (predictions written before the run)
- **Milestone:** M6 (first half: the transfer itself; the tray app's toast comes next)
- **Setup:** as F-021 (`radiod serve 44 … --mac random`, ALTX + fixed-rate data by default; `awdld -c 44 -S intersect -v`) plus `airdropd` (M2 console host, published to `D:\mtdrop-tools\build\airdropd`) started by the session script with `--interface awdl0 --wait-interface 90 --yes --dir <PrivateRoot>\received`: it waits for `awdl0`, then announces `_airdrop._tcp` by mDNS on it (4 records + NSEC, every 2 s and on queries, `flags=0x88`) and serves `/Discover`, `/Ask`, `/Upload` over TLS on port 8771. Two temporary inbound firewall rules for airdropd (TCP 8771, UDP 5353), removed at the end of the session. Shown name: the Windows computer name. Session 240 s. iPhone: Photos → one photo → Share → AirDrop, then tap the PC.

### Predictions

| step | prediction |
|---|---|
| airdropd | finds `awdl0` within seconds of awdld creating it, starts announcing |
| share sheet | **the PC appears in the iPhone's AirDrop sheet** within ~10–20 s (mDNS rides AWDL multicast in our two 44-slots per period) |
| `/Discover`, `/Ask` | logged; the ask is auto-accepted |
| `/Upload` | **the photo arrives** in `received\`, intact (opens), at tens of kB/s (2 of 16 slots, OFDM 12 Mb/s; jedbillyb measured ~45 kB/s on Linux) |
| system | no hang; firewall rules and `awdl0` gone afterwards; Wi-Fi back |

What would falsify: the PC never appears (mDNS not reaching the phone, or the phone needs more than the 4 records over AWDL); it appears but `/Discover` never arrives (TCP over AWDL not working); the upload stalls (throughput or loss).

### Raw result (FACT, 2026-11-17 21:00, home, channel 44, 240 s; device addresses omitted)

- **The PC did not appear in the share sheet; no `/Discover`, no file.** airdropd found `awdl0` and started announcing at once (its only other log line is one failed mDNS send when `awdl0` went away at the end).
- Peer added and valid within 1 s, iPhone sync master within 2 s, no peer timeout, intersect 2/16 slots on 44, as F-021. The iPhone kept sending action frames for the whole session (2533 forwarded), so its AWDL stayed up.
- **0 data frames from the iPhone** (F-021: 18). **Ping 0 of 20.** Unicast TX: 28 ACKed, 7 NOT ACKed – all 35 in the first 45 s (the pings); after that only multicast data went out (418 frames, airdropd's announcements among them), 0 TX refused or lost.
- Radio server: 2569 frames taken, 2538 forwarded, 31 filtered, 0 bad FCS, 0 pipe full.
- **RX delay in awdld (TSFT → OWL processing) much larger than in F-021:** over 118 status periods the median of the period means is 12.7 ms (F-021: 0.17–1.3 ms in most periods); 26 periods have a mean above 100 ms, 13 a maximum above 1 s (up to 1.38 s). The first period: 3.8–5.6 s (frames queued while awdld was starting).
- The machine was short of memory: Claude Code had stopped a background job shortly before because the system was critically low on memory, and right after the session 1.7 of 15.4 GB RAM were free (Chrome alone ~3.3 GB).
- Give-back normal, firewall rules and `awdl0` removed, no hang.

### Conclusion

1. **M6a failed below airdropd:** the iPhone never sent us a single data frame, so neither mDNS nor TCP could have worked. airdropd itself was not reached by anything.
2. What changed against F-021 is the delay with which OWL processes received frames (×10–1000). Upstream OWL dates every sync reading with the time of *processing* (`rx.c`: `tsft = clock_time_us()`), so a frame processed 100 ms late shifts our estimate of the master's slot phase by about six 16 TU slots. **ASSUMPTION:** with our phase that far off, the schedule we announce no longer overlaps the iPhone's 44-slots as the iPhone sees it, so it sends nothing to us – while it still ACKs what we send in the first seconds (ACKs need no schedule). The cause of the larger delay is not proven either; memory pressure is the likeliest (TODO-VERIFY).
3. Fix independent of the cause: `radiod` already writes into radiotap TSFT the host QPC microseconds at which the frame left the RX ring, on the same clock as MinGW's `clock_gettime(CLOCK_MONOTONIC)` (min delay 86–281 µs in this session, never negative). `awdld` now builds OWL with `OWL_RADIOTAP_TSFT_IS_HOST_CLOCK`: `rx.c` dates frames with that TSFT (falls back to processing time if missing, in the future or older than 10 s). Four new tests in `owl_tests` (`RxTsft.*`). The delay line in awdld's status now means "how late OWL reacts", no longer the dating error.

---

## F-023 – M6a repeated with OWL dating frames by radiod's TSFT

- **Date:** 2026-11-17 (predictions written before the run)
- **Setup:** as F-022, with the rebuilt `awdld` (TSFT dating). Before the session: close Chrome, Discord, Spotify, Slack and other large apps (aim: at least 4 GB RAM free). iPhone: AirDrop receiving set to "Everyone for 10 Minutes", Photos → one photo → Share → AirDrop, sheet kept open.

### Predictions

| quantity | prediction |
|---|---|
| RX delay in awdld | smaller than F-022 with less memory pressure; no longer decisive for sync either way |
| data from the iPhone | > 0 (as F-021) |
| ping | some replies (as F-021) |
| share sheet | the PC appears within ~20 s |
| `/Upload` | photo arrives in `received\` |

What would falsify the dating hypothesis: still 0 data frames from the iPhone although the RX delay stays small – then the difference to F-021 lies elsewhere (airdropd's traffic, the iPhone's state), and the next step is a run without `-Airdrop` (exact F-021 conditions) to split the two.

### Raw result (FACT, 2026-11-17 21:13, home, channel 44, 240 s; device names and addresses omitted)

- **The PC appeared in the iPhone's AirDrop sheet and received the photo – three times.** `/Ask` at 21:13:57, 21:14:06 and 21:14:23 (16 s after airdropd started announcing), each auto-accepted; one JPEG of 1,764,159 bytes saved 3–5 s after each ask. All three copies are byte-identical (same SHA-256), begin with `FFD8` and end with `FFD9`.
- Effective rate: ~1.76 MB in 3–5 s from ask to saved, i.e. **~350–590 kB/s** – roughly 10× what jedbillyb measured on Linux (45–67 kB/s; CLAUDE.md §2). One sample, home, iPhone close to the laptop.
- **Ping 16 of 20** (RTT 6–793 ms, mean 248 ms). 4183 data frames from the iPhone (1635 of them joined from aggregates); 1358 unicast frames sent, radio: 1177 ACKed, 71 NOT ACKed.
- RX delay in awdld still large at times (median of period means 5.7 ms; 12 of 119 periods with a mean above 100 ms, 20 with a maximum above 1 s), but sync now uses the TSFT dating.
- Radio server: 8088 frames taken, 6896 forwarded, **1192 filtered** (F-022: 31), 0 bad FCS, 0 pipe full; 6136 TX submitted, **141 refused** (TX ring or status map full – during the transfer), 0 lost.
- Large apps were closed before the session (maintainer). Give-back normal, no hang.

### Conclusion

1. **First end-to-end AirDrop receive on Windows over our own stack** (MT7921 + user-mode radio + OWL + airdropd), iOS 26.5.2, no app on the iPhone. The M6 transfer part works; what remains for M6 acceptance is the tray app's toast in place of `--yes` and the screen recording.
2. The F-022 failure is fixed, but **which change fixed it is not separated**: the TSFT dating and less memory pressure came together. The dating is right either way and stays; a deliberate run under memory pressure would tell whether it is sufficient (low priority).
3. Open, for throughput and robustness (none blocks M6): the 1192 filtered frames appeared only during the transfer – TODO-VERIFY whether these are iPhone aggregates larger than `RL_MAX_PAYLOAD` (4096) or header-translated frames, either of which costs TCP retransmissions; the 141 refused TX frames (TX queue depth in `radiod serve`); awdld's processing delay spikes.

---

## F-024 – M6b: the full user story with the tray app (prompt, toast, recording)

- **Date:** 2026-11-17 (predictions written before the run)
- **Milestone:** M6 acceptance (CLAUDE.md §7: screen recording – share sheet shows the PC, prompt/toast on Windows, photo opens)
- **Setup:** as F-023, but the tray app (`D:\mtdrop-tools\build\app\mtdrop.exe --minutes 5 --name <neutral name>`) receives instead of airdropd's console host: started by the maintainer in the desktop session, **Start receiving** clicked before the radio session; it waits for `awdl0` (new in this change), then announces. Session script with `-Airdrop tray` (temporary firewall rules for `mtdrop.exe`, no airdropd). Files go to `%USERPROFILE%\Downloads`. Large apps closed. iPhone name changed to a neutral one for the recording (CLAUDE.md §9: no real device names in the repo). Recording: Windows Snipping Tool screen recording + iPhone screen recording.

Offline (FACT, 2026-11-17): with an existing adapter the app listens on 8771 within 4 s and stops after its 15 s window; with `awdl0` missing it waits without listening and stays alive.

### Predictions

| step | prediction |
|---|---|
| tray | amber "waiting for awdl0" until awdld creates it, then green within ~1–2 s |
| share sheet | the PC appears under the neutral name within ~20 s, as F-023 |
| prompt | the Accept/Decline window appears bottom-right with the iPhone's (neutral) name and the photo's name/size |
| Accept | photo saved in Downloads, toast with **Open folder**; the photo opens |
| Decline (second try) | the iPhone shows the transfer as declined; nothing saved |

### Raw result (FACT, 2026-11-17 21:29, home, channel 44, 240 s; no recording yet – maintainer's choice; names omitted)

- Tray app started by the maintainer, **Start receiving** clicked; the session script found it running (`tray app running`), added the firewall rules for `mtdrop.exe`.
- iPhone peer added 20 s after awdld started (the maintainer opened the share sheet then), iPhone sync master 2 s later.
- **The PC appeared in the share sheet, the Accept/Decline window appeared on Windows, the maintainer clicked Accept, and the photo (2,642,945 bytes, `FFD8`…`FFD9`) was saved to Downloads at 21:30.**
- Toast with **Open folder**: not noticed by the maintainer (TODO-VERIFY next time). Decline: not tried.
- Ping 16 of 20 (RTT 8–630 ms). 2139 data frames from the iPhone (1245 joined); unicast 484 ACKed, 17 NOT ACKed; 55 TX refused, 0 lost; 494 frames filtered.
- After the transfer the maintainer closed the share sheet: from 21:30:25 the radio took **no frame at all** for the rest of the session and OWL removed the peer 30 s later. Consistent with airdrop-mt7921's finding that the iPhone's AWDL goes quiet without the share sheet (an RX stall of ours would look the same – F-023 kept receiving after its transfers while the sheet stayed open, so a stall is unlikely; TODO-VERIFY by reopening the sheet in a later session).

### Conclusion

1. The user story works with the real consent window: the share sheet shows the PC, Windows asks, Accept saves the photo to Downloads. Still open for the M6 acceptance: the toast (look for it), Decline, the recording.
2. The session still needs an elevated script started by hand; the tray's "Receive" cannot start the radio itself yet. That is M7's service.

## F-025 – M7.3: one-click receive session through the installed service

- **Date:** 2026-11-18 (predictions written before the run)
- **Milestone:** M7.3 (ADR-0004): the tray app's **Start receiving** alone runs the whole session – no script, no elevation by the user.
- **Setup:** service installed by `scripts/install-service.ps1 -Apply` (M7.2), updated the same day; card on MediaTek 3.5.0.1349, no proxy package, state clear, Wi-Fi connected (FACT, `mtdrop-service.exe --check`). Uptime ~14 h, 4.9 GB RAM free. Tray app from the Start menu (`C:\Program Files\mtdrop\app\mtdrop.exe`, default 2 min window). iPhone 15 Pro, iOS 26.5.2, AirDrop "Everyone for 10 Minutes". Two sessions back to back: the first accepts a photo, the second declines one (also checks that a second session works after a give-back).

### Predictions

| step | prediction |
|---|---|
| Start clicked | menu line runs through "reserving memory for the radio" → "switching the Wi-Fi card to mtdrop" → "starting the radio" → "waiting for the AWDL adapter"; amber icon; Wi-Fi drops |
| radio up | green icon "Receiving" within ~30 s of the click (dma-probe + swap ~5–10 s, radiod firmware + awdl0 ~10 s) |
| share sheet | the PC appears within ~20 s of opening the sheet, as F-023/F-024 |
| Accept | Accept/Decline window, photo in Downloads, toast with **Open folder** |
| window end (2 min) | "Turning Wi-Fi back on..." with Start disabled, then "Not receiving"; Wi-Fi reconnects within ~60 s of the end; no error notice |
| afterwards | `--check`: MediaTek driver, no proxy package, state clear; no task `mtdrop\watchdog`; no rules `mtdrop-receiver-*` |
| second session, Decline | same start; the iPhone shows the transfer declined; nothing new in Downloads; Wi-Fi back again |

What would falsify M7.3: anything needing a manual step (script, elevation) to receive, or Wi-Fi not back without `wifi-giveback.ps1`.

### Raw result (2026-11-18, maintainer at the PC, logs copied to the private folder)

Times from the service log (`C:\ProgramData\mtdrop\logs\service.log`), the receiver's saved file and the maintainer's report.

| | session 1 (Accept) | session 2 (Decline) |
|---|---|---|
| Start clicked → `Preparing` | 11:07:57 | 11:11:54 |
| dma-probe | PASS, 512 runs of 4 KiB | PASS, 512 runs of 4 KiB |
| saved Wi-Fi networks exported | **0** | 1 |
| proxy on the card (`oem121.inf`) | 11:08:01 (+4 s) | 11:11:58 (+4 s) |
| radiod + awdld started, firewall rules | 11:08:04 | 11:12:00 |
| service state `Receiving` | **11:08:07 (+10 s)** | **11:12:04 (+10 s)** |
| transfer | photo (3.19 MB) saved to Downloads 11:08:29, **+32 s after Start** | Decline clicked; nothing saved |
| end | 11:09:57 service deadline (`time window ended`) | 11:13:55 app's stop (`stopped on request`) |
| proxy package removed / MediaTek back | 11:10:14 / 11:10:19 (+22 s) | 11:14:11 / 11:14:15 (+20 s) |
| profiles after give-back | "all present" (nothing was exported) | **re-added 1** |
| Wi-Fi connected | **not within 30 s** (11:10:54 "back but not connected yet") | 11:14:24 (+29 s after the end) |
| watchdog task removed, Idle | 11:10:55 | 11:14:25 |

- Afterwards (FACT, `--check` at ~11:20): MediaTek 3.5.0.1349 (`oem12.inf`), no proxy packages, state clear, `awdl0` down, Wi-Fi connected; service Running; no task under `\mtdrop\`; no `mtdrop-receiver-*` firewall rules.
- awdld (session 2): 5 AWDL peers at start, sync master followed within 1 s, iPhone data frames from +4 s after awdld started (59 joined by +8 s), unicast ACKed 66 / not 9 over the first 40 s.
- **Decline:** the iPhone stayed on **"Waiting"** after Decline (maintainer). Our answer to `/Ask` was `403` with `Connection: close` (`AirDropServer.DeclineStatus`).
- **Green icon:** the maintainer reports ~1–2 min from the click to the green icon. That does not fit session 1: the receiver (which only runs once the icon is green) had saved the photo 32 s after the click, and the service reported `Receiving` at +10 s in both sessions. TODO-VERIFY: what was timed (the tray app writes no log yet, so its side cannot be checked).
- Toast with **Open folder** after Accept: **did not appear** (maintainer). FACT (registry): Windows notifications are switched off for the whole user (`HKCU\...\PushNotifications\ToastEnabled = 0`); the toast registration of the installed app exists (`HKCU\Software\Classes\AppUserModelId\…/mtdrop/app/mtdrop.exe`). The app swallows toast failures and has no other way to say a file arrived.
- Profile export as SYSTEM gave 0 and 1 files, while the same `netsh wlan export profile folder=…` run unelevated as the maintainer exports 31 files and prints one "The filename, directory name, or volume label syntax is incorrect." line (netsh exit code 1). The service does not log netsh's output or exit code, so the cause is unknown. After session 1 Wi-Fi did **not** reconnect by itself; the maintainer connected by hand.
- FACT (`C:\ProgramData\Microsoft\Wlansvc\Profiles\Interfaces\`, folder and file times): Windows keeps saved networks per WLAN **interface GUID**, and every time the MediaTek driver binds again the adapter gets a **new** GUID with no profiles. 29 interface folders exist, one per give-back since 2026-11-10 (each holding the 32–35 profiles our scripts re-added). Today: the interface folder after session 1 was created at 11:11:10 with **1** profile – the maintainer's manual connect; the one after session 2 at 11:14:20 – the service's re-add. So session 1's missing reconnect was ours (nothing to re-add after the 0-file export), not the network's, and session 2 exported exactly the 1 profile that interface had. At 11:16:25, with the service idle, 34 profiles of the 2026-11-17 interface appeared in the current folder as copies (original write times kept); what copied them is unknown.

### Conclusion (preliminary)

- **M7.3 core: PASS, except Wi-Fi reconnect after session 1 (see below).** Two sessions from one click each, no script and no elevation; the service took the card, ran the radio, ended on time (its own deadline in session 1, the app's stop in session 2), gave the card back through the C# give-back and cleaned up the task and firewall rules. The second session ran straight after the first give-back. Wi-Fi was off ~2.5 min per session; the radio was up 10 s after the click.
- **Decline is not usable yet:** `403` leaves the iPhone on "Waiting" (now FACT on our phone, as `mt/docs/FINDINGS.md` reported). Next: try `401` (winawdl's untested choice) and other answers; make the status selectable without a rebuild.
- **Wi-Fi does not come back without the profile re-add**, because every swap gives the adapter a new interface GUID with no saved networks. The SYSTEM export gave 0 files in session 1 although that interface had ~35 profiles (cause unknown; netsh output is not logged), so Wi-Fi stayed off until a manual connect. This fails the "Wi-Fi back automatically" criterion. Next: read and write profiles through the WLAN API (`WlanGetProfileList` / `WlanGetProfile` / `WlanSetProfile`) instead of `netsh` files, log counts and errors, and refuse to take the card when no profile could be saved while the adapter is connected.
- **Toasts are off in Windows** on this machine: the app must not rely on them. Next: show the saved-file message in the app's own card when toasts are disabled.
- **The tray app needs its own log** (state changes, `/Ask`, decision, answer, timings) so the next session can settle the green-icon timing and the Decline behaviour from data instead of memory.

## F-026 – M7.3 follow-up: Wi-Fi networks through the WLAN API, notice card, Decline with 401

- **Date:** 2026-11-18 (predictions written before the run)
- **Change under test (code, F-025 conclusion):** the service exports the card's saved networks through the Native Wifi API (`WlanGetProfileList` / `WlanGetProfile`, keys stay encrypted) as numbered files and re-adds the missing ones with `WlanSetProfile` after the give-back; it refuses to take the card when Wi-Fi is connected but nothing could be exported. The tray app writes `%LOCALAPPDATA%\mtdrop\logs\app.log`, shows its own card when Windows toasts are off, and takes `--decline-status` / `--decline-keep-open`.
- **Offline checks (FACT):** service tests 58/58, protocol tests 77/77; `mtdrop-service.exe --check` unelevated reads "35 on the card, 35 readable" through the new API; `mtdrop.exe --test-notice` and `--test-prompt` open and close their windows without errors.
- **Setup:** re-install with `scripts/install-service.ps1 -Apply`; tray app started by hand as `mtdrop.exe --decline-status 401`. One 2-minute session: first send → **Decline**, second send in the same session → **Accept**. Same iPhone, iOS 26.5.2, "Everyone for 10 Minutes", same network as F-025.

### Predictions

| step | prediction |
|---|---|
| export | service log: `wifi: ~35 saved network(s) on the card, ~35 exported` |
| green | `app.log`: `awdl0 ready … s after Start` ≤ 20 s |
| Decline (401) | `app.log`: `/Ask answered 401 Unauthorized, Connection: close`; iPhone: **unknown** – either "Declined" (then 401 is the answer) or "Waiting" again (then try `--decline-keep-open`, then other codes) |
| Accept | photo in Downloads; the app's own card "Received from …" with **Open folder** (toasts are off), which opens Explorer on the file |
| give-back | `re-added N of N missing saved network(s)` with N ≈ 35 (fewer if Windows copies them first, as seen at 11:16:25 in F-025); `Wi-Fi is connected` within 30 s; **no manual connect** |

What would falsify the fix: Wi-Fi needing a manual connect again, or the export failing/refusing while Wi-Fi is connected.

### Result (2026-11-18, 18:15–18:18)

- **Install (FACT):** the first `-Apply` stopped the service and the tray app but left the old files in place with the service stopped (the old app then showed no green icon and wrote no `app.log`). Probable cause (ASSUMPTION, the installer output was not kept): `Stop-Process` / a service reporting `Stopped` return before the process releases its image files, so the immediate `Remove-Item` of the install folder failed on a locked file. Fixed in the installer (wait for processes running from the folder, retry the delete); the second `-Apply` succeeded.
- **Green (FACT, `app.log`):** Start 18:15:57.9 → service `Receiving` 18:16:08.3 → `awdl0 ready (index 58) 10.9 s after Start` → tray green 18:16:08.9; `/Discover` answered 1 s later. The "1–2 min to green" from F-025 is not reproduced; the app log now makes it measurable.
- **Decline with 401 (FACT):** `/Ask: 1 file(s), 446836 bytes` 18:16:18.8, user declined after 3.5 s, `/Ask answered 401 Unauthorized, Connection: close`. **The iPhone showed "Declined"** (maintainer). With `403` it stayed on "Waiting" (F-025). → 401 is the decline status iOS 26.5.2 understands; `--decline-keep-open` was not needed.
- **Accept / notice card (FACT, second session 18:25–18:28):** green 9.1 s after Start; `/Ask` accepted after 2.0 s, `/Upload` received in 1.0 s (446,836 bytes), `notice (card)` shown because toasts are off; **"Open folder" opened Explorer on the photo** (maintainer). Give-back 26 s, Wi-Fi back by itself again. The log line of that notice contained the sender's device name; fixed – `app.log` now records only the kind of notice (`saved N file(s)`), never its title or text.
- **Give-back (FACT):** window ended 18:17:58.1, service `RestoringWifi` 18:17:58.9 → `Idle` 18:18:25.0 (26 s). **Wi-Fi reconnected by itself** (maintainer; `netsh` afterwards: connected, channel 2). Service log with the export / re-add counts: TODO (to be copied to the private folder).

### Conclusion

- The WLAN API export + re-add fixes the "no saved networks after the swap" problem from F-025 (falsifier not hit: no manual connect).
- Decline must answer **401**, not 403: make 401 the default (`AirDropServer.DeclineStatus`, `ReceiveSession`, tray app); `--decline-status` stays for experiments.
- The notice card replaces toasts when they are off; Accept → save → "Open folder" works end to end.
- Open: export/re-add counts from the service log (not copied yet).
