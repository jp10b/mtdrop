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
