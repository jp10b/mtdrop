# Protocol notes – what we implement

Our working understanding of the AirDrop stack, bottom to top, for the **receive** path in "Everyone for 10 Minutes" mode. Sending (M8) is summarised at the end.

Status: 2026-11-09. This is an independent interoperability description. It contains no Apple code or keys.

## How to read this document

Every statement carries its evidence:

| Label | Meaning |
|---|---|
| **FACT (capture)** | Measured by us on iPhone 15 Pro / iOS 26.5.2 – `docs/FINDINGS.md` F-001, F-002 |
| **FACT (source)** | Read in open-source code; file and line given, relative to `third_party/<clone>` at the commit listed in `docs/RESEARCH.md` §1 |
| **REPORTED** | Measured by another project and written in its notes; not reproduced by us |
| **ASSUMPTION** | Our inference |
| **TODO-VERIFY** | Unknown; needs an experiment |

Source abbreviations: `owl/` = `seemoo-lab_owl`, `owl-fork/` = `jedbillyb_owl`, `opendrop/` = `seemoo-lab_opendrop/opendrop`, `opendrop-fork/` = `jedbillyb/opendrop` @ `787f694` (not vendored; the version M0 ran), `mt/` = `jedbillyb_airdrop-mt7921`, `wa/` = `raph559_winawdl`, `fg/` = `Foglio24_linux-airdrop`, `s21/` = `21Suspect_AirDrop-for-Windows`, `a4e/` = `BenItBuhner_AirDrop-for-Everything`.

---

## 1. Overview of one transfer

```
iPhone                                                   PC (receiver)
  │  share sheet opened → AWDL radio comes up                 │
  │── AWDL action frames (PSF/MIF) on ch 6 / 44 ─────────────▶│  sync to the phone's schedule,
  │◀──────────────── AWDL action frames ──────────────────────│  advertise ourselves as a peer
  │── mDNS query  PTR _airdrop._tcp.local ───────────────────▶│
  │◀── mDNS response PTR/SRV/TXT/AAAA (repeated) ─────────────│
  │── TLS 1.3 + POST /Discover ──────────────────────────────▶│
  │◀── 200 {ReceiverComputerName, …} ─────────────────────────│  PC appears in the share sheet
  │   user taps the PC                                        │
  │── TLS 1.3 + POST /Ask  (file list, thumbnail) ───────────▶│  toast: Accept / Decline
  │◀── 200 {…} ────────────────────────────────────────────────│  (Accept)
  │── POST /Upload (same connection; chunked dvzip) ─────────▶│  decode, extract, save
  │◀── 200 ────────────────────────────────────────────────────│
```

All IP traffic is IPv6 link-local inside AWDL data frames. Nothing touches the normal Wi-Fi network.

---

## 2. Bluetooth LE

- **Receive:** not needed. Opening the share sheet is what brings the phone's AWDL up. REPORTED: Control Centre alone, or a locked phone, produces no AWDL frames (`mt/README.md:74-75`).
- **Send (M8):** needed, see §9.

---

## 3. AWDL link layer

AWDL (Apple Wireless Direct Link) is a peer-to-peer mode on top of ordinary 802.11 frames. Peers agree on a time grid and a per-slot channel list, and exchange IPv6 in special data frames. Everything below is FACT (source) from OWL unless marked.

### 3.1 Time grid

| Term | Value | Source |
|---|---|---|
| TU (time unit) | 1024 µs | `owl/src/ieee80211.h:161-167` |
| AW (availability window) | 16 TU = 16.384 ms | `owl/src/sync.c:26-27` |
| Slot (extended AW) | 4 AW = 64 TU = 65.536 ms | presence mode 4 |
| Channel sequence | 16 slots = 1024 TU ≈ 1.049 s | `owl/src/channel.h:25` |

- Current slot = `(AW counter / 4) mod 16`.
- A slave copies the master's phase from two fields of the master's Synchronization Parameters TLV: `tx_down_counter` (TU until the next slot boundary) and `next_aw_seq` (AW counter). OWL overwrites its phase on **every** action frame from the master, using the time the frame was *processed in user space* (`owl/src/sync.c:67-72`, `owl/src/rx.c:53`). Capture-path latency therefore becomes phase error.
- FACT (capture): after lock, OWL's reported sync error stayed within ±10 TU.

### 3.2 Channels and sequences

- Each peer advertises 16 entries, one per slot. Entry 0 means "absent in this slot" (`owl/daemon/core.c:295`). REPORTED: peers really are absent in those slots (`mt/docs/FINDINGS.md:1361-1376`).
- Encodings of one entry (`owl/src/channel.h:27-53`): simple (1 byte: channel), legacy (2 bytes: flags, channel), **opclass (2 bytes: channel, operating class)**. Known opclass pairs: ch 6 = `06 51`, ch 44 = `2c 80`, ch 149 = `95 80`, ch 36 = `24 80`.
- FACT (capture): our phone used opclass encoding and these sequences:

  | Phase | Sequence (slot 0 … 15) |
  |---|---|
  | Idle | `1,0,44,0,0,0,0,0,6,0,44,0,0,0,0,0` |
  | Peer known | `44,44,44,0,0,0,0,0,6,44,44,0,0,0,0,0` |
  | During `/Ask` and `/Upload` | `44,44,44,44,44,44,44,44,6,44,44,44,44,44,44,44` |

- FACT (capture): slot 8 was always channel 6. Channels 36 and 149 never appeared. REPORTED for other phones/regions: 149 and 36 instead of 44 (`mt/docs/FINDINGS.md:1384-1396`, `3114-3120`). ASSUMPTION: the 5 GHz channel depends on regulatory region; in PL it is 44.
- Slot 0 in the idle sequence carried channel 1 = the home AP's channel. REPORTED the same way elsewhere ("slot 0 holds the infra channel", `mt/docs/FINDINGS.md:1378-1382`).
- The phone widens its sequence when it has data for us and narrows it again afterwards, re-advertising about once per second. FACT (capture): one narrowing in the middle of an upload coincided with a stalled transfer.

**What we may advertise** (REPORTED, `mt/docs/FINDINGS.md:1744-1873`, iOS 26):

| Our sequence | Phone's reaction |
|---|---|
| Verbatim copy of the phone's | Works (FACT (capture) for us too) |
| Phone's sequence with empty slots filled by its dominant channel ("widen") | Accepted up to 16/16 |
| One channel in all 16 slots ("pin") | Phone stops sending unicast to us |

Windows projects nevertheless run on a single fixed channel (winawdl ch 44, 21Suspect ch 6). TODO-VERIFY on our phone which advertised sequence a fixed-channel radio needs; candidates: verbatim advertisement while the radio stays on 44, or `intersect` (keep only the phone's ch-44 slots).

### 3.3 Election

- Every peer advertises a metric and a counter; the highest (counter, then metric) becomes master; the rest follow its timing (`owl/src/election.c:60-120`).
- OWL advertises fixed values metric 60, counter 0 and therefore always loses to an Apple device (`owl/src/election.h:26-27`). REPORTED example: phone metric 65, counter 223.
- FACT (capture): the phone was master for the whole session.

### 3.4 Frames

All AWDL frames use a 24-byte, 3-address 802.11 header with **address 3 = `00:25:00:ff:94:73`** (the AWDL BSSID) and duration 0 (`owl/src/frame.h:30`, `owl/src/tx.c:321-333`). The radio appends the FCS.

**Action frames** (management/action, frame control `0x00d0`, destination broadcast). 16-byte fixed header (`owl/src/frame.h:43-57`):

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | category | 127 (vendor specific) |
| 1 | 3 | OUI | `00 17 f2` |
| 4 | 1 | type | 8 |
| 5 | 1 | version | `0x10` |
| 6 | 1 | subtype | 0 = PSF (periodic sync frame), 3 = MIF (master indication frame) |
| 7 | 1 | reserved | 0 |
| 8 | 4 | PHY TX time | LE32 µs |
| 12 | 4 | target TX time | LE32 µs |

Then TLVs: type (1 byte), length (LE16), value.

- OWL sends a PSF every 110 TU and one MIF per slot, 32 TU after the slot boundary (`owl/daemon/core.c:184-200,410-415`).
- OWL rejects a whole action frame if any bytes are left over after the last TLV (`owl/src/rx.c:295-311`). FACT (capture): 2132 of our phone's action frames were rejected for 2 trailing bytes. winawdl trims trailing zero bytes from iPhone MIFs for this reason (`wa/awdl/win/frame_fixup.c`). TODO-VERIFY which subtype is affected and port the trim.

**TLVs that matter** (`owl/src/frame.h:62-224`; all little-endian unless noted):

| ID | Name | We parse | We send | Key content |
|---|---|---|---|---|
| 4 | Synchronization Parameters | yes | yes | next AW channel, `tx_down_counter`, master channel, AW period 16, action frame period 110, master address, presence mode 4, AW sequence number, embedded channel sequence |
| 5 | Election Parameters (v1) | yes | yes | distance to master, master address, master metric, own metric |
| 24 | Election Parameters v2 | yes | yes | master address, sync address, master counter, distance, master metric, own metric, own counter |
| 18 | Channel Sequence | yes | yes | count−1 (=15), encoding, duplicate count 0, step count 3, fill `0xffff`, 16 entries |
| 12 | Data Path State | partly | yes | flags, country code, social channel bitmap, own address |
| 16 | Arpa | yes | MIF only | host name + DNS pointer `c0 0c` |
| 21 | Version | yes | yes | AWDL version (`0x34` in OWL) and device class (1 macOS, 2 iOS, 8 tvOS) |
| 6 | Service Parameters | no | yes (zeros) | – |
| 7 | HT Capabilities | no | MIF only | fixed bytes; winawdl clears OWL's 40 MHz bits |
| 2 | Service Response | no | no | DNS-SD records inside action frames, see §9 |
| 0, 1, 8–11, 13–15, 17, 20, 22, 23 | various | no | no | ignored |

A peer becomes usable only after it has sent a MIF and a Version TLV (`owl/src/peers.c:62-64`).

**Data frames** (frame control `0x0008`, no QoS on TX; QoS and A-MSDU accepted on RX):

```
802.11 header (24)  addr1 = peer MAC, addr2 = own MAC, addr3 = AWDL BSSID
LLC/SNAP (8)        aa aa 03 | 00 17 f2 | 08 00
AWDL data hdr (8)   03 04 | sequence LE16 | 00 00 | EtherType BE16 (0x86dd)
IPv6 packet
```

(`owl/src/tx.c:33-50,345-355,387-402`, `owl/src/rx.c:353-418`.)

- Unicast is sent only in slots where our own and the peer's advertised channel are equal and non-zero, staying 3 TU away from slot edges (`owl/src/schedule.c:30-80`).
- Multicast (mDNS) is sent only in slots 0 and 10, in the middle 32 TU (`owl/src/schedule.c:43-46`). Slot 0 and 10 were both channel 44 for our phone during transfers.

### 3.5 Acknowledgements – the critical radio requirement

- OWL has no retransmission, no ACK tracking and never asks for "no ACK" (`owl/daemon/io.c:361-371`, `owl/src/tx.c:297-319`). It depends on the radio to:
  1. **ACK received unicast frames addressed to our MAC** ("active monitor", `owl/daemon/netutils.c:225`), and
  2. **wait for the ACK and retry** our own unicast frames.
- REPORTED: without (1) the path is one-way – the phone never gets our TCP ACKs through (`mt/docs/FINDINGS.md:840-863`); winawdl measured ~10 kB/s and 85 % retries without firmware ACKs versus ~16 MB/s with them (`wa/DESIGN-NOTES.md`).
- FACT (capture): with active monitor on Linux the full transfer ran at ~71 kB/s.

### 3.6 IPv6 addressing

- Link-local address = `fe80::` + modified EUI-64 of the MAC: bytes `mac[0]^0x02, mac[1], mac[2], ff, fe, mac[3], mac[4], mac[5]` (`owl/daemon/netutils.c:607-619`).
- No neighbour discovery happens on air. OWL installs a static neighbour entry per valid peer (`owl/daemon/core.c:310-318`). On Windows with Wintun (a layer-3 adapter) the MAC is derived from the address instead (`wa/awdl/win/host_frame.c`).
- MTU of the host interface: 1450 (`owl/daemon/io.c:182`). FACT (capture): the phone's TCP MSS was 1440; typical segments carried 1378 bytes.
- REPORTED: iOS answers ICMPv6 echo over AWDL (`mt/docs/FINDINGS.md:1553-1572`) → `ping` is a valid link test for M5.

### 3.7 Radio requirements derived from this section

| Requirement | Why |
|---|---|
| Receive all frames with address 3 = AWDL BSSID, with per-frame RSSI and FCS status | OWL drops frames below −65/−70 dBm unless told not to, and reads radiotap FLAGS and signal on every frame (`owl/src/rx.c:273-278,470-475`) |
| Report the channel each frame was actually received on | Reported channel state has been wrong on this chip (REPORTED; FACT (capture): the rig logged "NA") |
| ACK unicast to our MAC in firmware | §3.5 |
| Transmit with a fixed rate, ACK wait and retries; honest status | §3.5; OWL sends everything at 12 Mbit/s |
| Change channel in well under one slot | Linux: 6.4–9.1 ms, median 7.7 ms (FACT (capture)) |
| Never sleep | REPORTED: runtime power management silently stops reception |

---

## 4. Service discovery (mDNS)

Standard multicast DNS over IPv6 on the AWDL interface: `ff02::fb`, UDP 5353, hop limit 255.

### 4.1 Records we publish

FACT (capture) – this exact set made the phone list the receiver:

| Name | Type | Class flag | TTL | Data |
|---|---|---|---|---|
| `_airdrop._tcp.local` | PTR | – | 4500 | `<id>._airdrop._tcp.local` |
| `<id>._airdrop._tcp.local` | SRV | cache-flush | 120 | priority 0, weight 0, port, `<host>.local` |
| `<id>._airdrop._tcp.local` | TXT | cache-flush | 4500 | `flags=<decimal>` |
| `<host>.local` | AAAA | cache-flush | 120 | our link-local address |

- `<id>` = 12 lowercase hex characters, random (`opendrop/config.py:83-85`). Keep it stable across restarts, otherwise the phone shows a stale duplicate (`opendrop-fork/config.py:83-100`).
- `<host>` should not be the real machine name; winawdl uses `<id>.local` (`wa/airdrop/receiver.py:484`). The name shown to the user comes from `/Discover`, not from mDNS.
- Port: 8771 by convention; the phone uses whatever SRV says.
- FACT (capture): the phone also asks for an **A** record of our host. winawdl adds an NSEC record (types AAAA + NSEC) so that senders stop waiting for an IPv4 address (`wa/airdrop/mdns.py:264-267`). We publish it too.

### 4.2 `flags`

Bit meanings recovered by the OpenDrop authors (`opendrop/config.py:32-51`): `0x01` URL, `0x02` dvzip, `0x04` pipelining, `0x08` mixed types, `0x40` iris, `0x80` supports `/Discover`, `0x200` asset bundle; `0x10`, `0x20`, `0x100` unknown.

| Value | Used by | Evidence |
|---|---|---|
| `0x88` (136) | OpenDrop and fork | FACT (capture): works with iOS 26.5.2; the phone still uploads dvzip |
| `0x3fb` (1019) | macOS, winawdl | REPORTED working |
| `0x8a`, `0x89` | 21Suspect, AirDrop-for-Everything | REPORTED / untested |

We start with `0x88`. TODO-VERIFY whether the URL bit is needed for the phone to offer link shares.

### 4.3 Announce and answer

- The phone must *hear* us during the few slots we share. REPORTED: announcing once is not enough; the fork re-announces every 5 s (`opendrop-fork/server.py:152-192`), winawdl every 2 s.
- FACT (capture): the phone **does** send `PTR _airdrop._tcp.local` queries (12 in 180 s, with known-answer lists). The receiver sent 35 unsolicited 4-record responses in the same time. We do both: periodic unsolicited responses and immediate answers to matching queries (rate-limited).
- Goodbye on shutdown: the same records with TTL 0.
- Bind to the AWDL interface **by interface index**, never by address. Windows' own mDNS responder also holds port 5353, so the socket needs address reuse (`wa/airdrop/mdns.py:272-288`).

---

## 5. TLS

FACT (capture) unless marked:

- TLS 1.3, `TLS_AES_256_GCM_SHA384`.
- The server presents a self-signed certificate: RSA-2048, CN free text, valid 365 days (`opendrop/config.py:113-155`). The phone accepts it. Check expiry and regenerate; nothing else does.
- No client certificate is requested and none is sent.
- No ALPN, no SNI requirement (FACT (source): no implementation sets either).
- The phone opens several connections; one of them completed the server's handshake flight and was then abandoned without a request. Handshakes must run per connection, never inside the accept loop (`wa/airdrop/receiver.py:153-163`).
- ECDSA server certificates: TODO-VERIFY. Not needed.

---

## 6. HTTP

Three endpoints, all `POST`, HTTP/1.1, bodies are Apple **binary property lists** (`bplist00`).

Properties common to every request from the phone (FACT (capture)):

- `User-Agent: AirDrop/1.0`.
- **`Transfer-Encoding: chunked`. Never `Content-Length`.**
- **No `Host` header**, no `Content-Type` on `/Discover` and `/Ask`, no `Expect` header in any request we captured. (REPORTED: `Expect: 100-continue` on `/Upload` with other phones – support it, do not rely on it.)

Our responses use `Content-Length`.

### 6.1 `POST /Discover`

`Connection: close`. One request per connection.

Request body:

| Key | Type | Notes |
|---|---|---|
| `SenderRecordData` | data (~3.8 kB, CMS/PKCS#7 DER) | Apple-signed record of the sender's account. Ignored in Everyone mode. Never stored. |
| `DeviceSupportFlags` | integer | `111611` (`0x1B3FB`). Meaning TODO-VERIFY. |

Response `200`:

| Key | Type | Notes |
|---|---|---|
| `ReceiverComputerName` | string | The name shown in the share sheet. Without it the sender treats us as not discoverable (`opendrop/client.py:144-145`). |
| `ReceiverModelName` | string | Free text worked (`OpenDrop`). REPORTED: a Mac model identifier selects the icon the sender draws (`wa/airdrop/receiver.py:34`). |
| `ReceiverMediaCapabilities` | data | UTF-8 JSON. `{"Version": 1}` makes the phone convert to widely readable formats (JPEG rather than HEIF) – FACT (capture): we received `public.jpeg`. |

Do **not** send `ReceiverRecordData`.

### 6.2 `POST /Ask`

`Connection: keep-alive`. New connection (not the `/Discover` one).

Request body (157 kB for one photo, mostly the thumbnail) – keys in the order sent:

| Key | Type | Notes |
|---|---|---|
| `TransferID` | dict `{ id: string }` | Upper-case UUID; new for every attempt, including the phone's own retries |
| `TransferType` | dict | `{ files: {} }` for files; a `links` key for URL shares (REPORTED) |
| `SenderRecordData` | data | as in `/Discover` |
| `SenderID` | string | 12 hex characters |
| `BundleID` | string | Sending app, e.g. `com.apple.mobileslideshow` (Photos) |
| `SenderComputerName` | string | Sender's device name – self-reported, unverified. Show it, do not trust it. |
| `SenderModelName` | string | e.g. `iPhone` |
| `Items` | array | Empty for files; URL strings for link shares (REPORTED) |
| `Files` | array of dict | see below |
| `FileIcon` | data | JPEG 2000 thumbnail (signature `00 00 00 0c 6a 50 20 20`), ~150 kB here. Untrusted image data. |
| `ConvertMediaFormats` | bool | false |
| `SenderIdentityAuthTag` | data, 3 bytes | Differs per attempt. Meaning TODO-VERIFY. |

Per file:

| Key | Type | Notes |
|---|---|---|
| `FileName` | string | May contain path parts on other senders – use the last component only |
| `FileType` | string | Uniform Type Identifier, e.g. `public.jpeg` |
| `FileSize` | integer | Exact size in bytes. REPORTED: 0 for a Live Photo bundle |
| `FileBomPath` | string | `./<FileName>` – the member name inside the archive |
| `FileIsDirectory` | bool | |
| `ShouldConvertMediaFormats` | bool | |

Response:

- **Accept:** `200` with `ReceiverComputerName` and `ReceiverModelName`. FACT (capture): sufficient. winawdl also repeats `ReceiverMediaCapabilities`.
- **Decline:** unsettled. Four projects answer `403`, winawdl `401`; the one on-device note about `403` says the phone stayed on "Waiting" (`mt/docs/FINDINGS.md:3347`). FACT on our phone (F-025, iOS 26.5.2): `403` with `Connection: close` leaves the iPhone on "Waiting". **TODO-VERIFY** `401` and other answers; the tray app takes `--decline-status <code>` and `--decline-keep-open` for this.
- **How long may we take?** FACT (capture): 1.5 s and 3.2 s worked. REPORTED: 15 s worked (`wa/DESIGN-NOTES.md:608-611`). Upper bound TODO-VERIFY. While waiting, watch the connection: if the sender closes it, the user cancelled → dismiss the prompt (`wa/airdrop/receiver.py:138-151`).
- Link shares end here: there is no `/Upload` (REPORTED). Not in scope for v0.1; answer as decline or ignore safely.

### 6.3 `POST /Upload`

FACT (capture): arrives on the **same connection** as its `/Ask`, 1–2 s after our `200`.

Headers, in the order sent:

```
User-Agent: AirDrop/1.0
TotalBytes: <decimal>            exact byte size of the file(s), NOT the archive size
Content-Type: application/x-dvzip
SenderPseudonym: pseud:<22 chars>
SenderPushToken: <64 hex>
TransferID: <UUID from /Ask>
Connection: keep-alive
Transfer-Encoding: chunked
```

- `SenderPseudonym` and `SenderPushToken` are stable identifiers of the sender. We ignore them and never log them.
- Chunks are 16 371 bytes (first and last smaller).
- **End of body.** FACT (capture): for a photo the terminating `0\r\n\r\n` chunk was sent. REPORTED: iOS 26 often withholds it until it gets a reply (always, per `opendrop-fork/server.py:625-634`; only for video/Live Photo, per `wa/DESIGN-NOTES.md:392-398`). Rule: the body is complete when **either** the 0-chunk arrives **or** the decoded archive has reached its `TRAILER!!!` member. Never treat a timeout as completion (`s21/opendrop/windows/archive.py:212-217`).
- Response: `200`, `Content-Length: 0`, `Connection: close`.
- Other content types: `application/x-cpio` (older senders; gzip-compressed cpio, signature `1f 8b`) – REPORTED, not needed for iOS 26.

### 6.4 dvzip container

FACT (capture):

```
stream  := block*
block   := header(4 bytes, big-endian) payload
header  := bit 31: 1 = payload stored, 0 = payload is one zlib stream (78 9c …)
           bits 30..0: payload length in bytes
```

- Each block inflates to 131 072 bytes, except the first (169 bytes here – the `.` directory and the first file header) and the last.
- A zlib payload can be **longer** than 131 072 bytes (up to 131 118 seen). The sender switches to stored blocks at some point; both kinds mix in one stream (12 zlib, then 8 stored).
- No trailer. FACT (source): several decoders also stop at a zero header; we treat a zero header as an error unless seen in a capture (TODO-VERIFY).
- Bound every block: reject headers above 1 MiB and inflated sizes above 1 MiB (`fg/secure_receiver.py:426-435`).

### 6.5 Inner archive: cpio "odc"

FACT (capture) + FACT (source) for the layout (`opendrop-fork/server.py:775-792`):

```
header (76 ASCII bytes, octal digits):
  magic "070707" | dev 6 | ino 6 | mode 6 | uid 6 | gid 6 | nlink 6 | rdev 6 | mtime 11 | namesize 6 | filesize 11
name   (namesize bytes, NUL-terminated)
data   (filesize bytes, no padding)
```

Members sent for one photo, in order:

| # | Name | Mode | Content |
|---|---|---|---|
| 1 | `.` | `040700` directory | – |
| 2 | `./<FileName>` | `100644` | the photo, `FileSize` bytes |
| 3 | `./._<FileName>` | `100644` | AppleDouble sidecar (signature `00 05 16 07`, "Mac OS X"), extended attributes |
| 4 | `TRAILER!!!` | 0 | end marker |

After the trailer the archive is zero-padded to a multiple of 512 bytes.

REPORTED variants on other iOS versions: a wrapper directory `NSIRD_AirDrop_<random>/`, Live Photo bundles `IMG_xxxx.pvt/` (still + `.mov` + `metadata.plist`), sandbox path prefixes (`mt/tools/airdrop-tidy:21-49`). Our extraction must not depend on the directory shape.

Reference implementation with tests: `tools/proto/airdrop_codec.py`.

---

## 7. Receiver rules (what `airdropd` must enforce)

Derived from the above and from the hardening in Foglio24, winawdl and 21Suspect.

**Consent and binding**

1. No file is written before the user accepts the `/Ask` on that connection.
2. `/Upload` is accepted only on a connection with an accepted `/Ask`, and only with the same `TransferID`. One upload per accepted ask.
3. An accepted ask expires (default 120 s) if no upload follows.
4. The prompt shows the sender name as *claimed*; it is not authenticated in Everyone mode.

**HTTP robustness**

5. One thread/task per connection; TLS handshake on that task. The phone keeps idle keep-alive connections open and opens several at once (REPORTED: a single-threaded server leaves the phone on "Waiting…").
6. A request without `Host` is valid. Reject requests carrying both `Content-Length` and `Transfer-Encoding`. Validate chunk-size lines strictly.
7. Limits: plist bodies ≤ 4 MiB; archive members ≤ 512; total size ≤ announced `TotalBytes` plus a small allowance; free-disk check before accepting.
8. Timeouts on every read. During upload, tolerate gaps: data arrives in bursts with pauses of ~0.5 s and occasional multi-second stalls (FACT (capture)). Do not enforce a minimum average rate – winawdl's 64 KiB/s floor would abort a normal MT7921 transfer.
9. A stalled upload is abandoned without saving a partial file, but the service stays up: the phone retries by itself with a new `TransferID` (FACT (capture)). The retry needs a new accept decision – or the previous decision is reused for the same sender and file list within a short window (design choice for M2).
10. Sender resets at the end of a keep-alive session are normal, not errors.

**Extraction**

11. Streaming decode: chunked → dvzip → cpio, no whole-file buffering in memory.
12. Only regular files are written. Directories are flattened or recreated under the destination only; links and special files are rejected.
13. Member names: take the path relative to `.`; reject absolute paths, `..`, backslashes, drive letters, control characters; neutralise Windows reserved names (`CON`, `NUL`, `COM1`…), trailing dots and spaces; cap length.
14. Members whose last path component starts with `._` are discarded. Live Photo `metadata.plist` is discarded.
15. Never overwrite: create with "fail if exists" and append ` (1)`, ` (2)`…
16. Write into a temporary file in the destination directory, rename on completion, delete on failure.
17. Mark saved files as coming from another computer (`Zone.Identifier`, zone 3), as browsers do.
18. The thumbnail (`FileIcon`, JPEG 2000) is untrusted input. v0.1 does not decode it.

**Privacy**

19. Never log or persist `SenderRecordData`, `SenderPseudonym`, `SenderPushToken`, `SenderID`. Debug dumps of requests are off by default and go outside the repository.

---

## 8. Not supported (by design)

- "Contacts Only": needs an Apple-issued certificate and signed record that cannot be created outside Apple devices. Permanent.
- Wi-Fi Aware / NAN based AirDrop paths.
- A possible six-digit pairing-code flow mentioned by 21Suspect (`s21/LIMITATIONS.md`). FACT (capture): not requested by iOS 26.5.2 in our test. TODO-VERIFY if it ever appears.

---

## 9. Sending PC → iPhone (M8, summary only)

All REPORTED unless marked; none verified by us yet.

- **Wake-up:** a BLE advertisement with Apple's company ID `0x004c`, Continuity type `0x05`, 18 payload bytes. Zero or random "contact hashes" suffice in Everyone mode. On Windows: WinRT `BluetoothLEAdvertisementPublisher`. The phone must have its share sheet **closed** and be unlocked.
- **Finding the phone:** a query for `_airdrop._tcp` every ~1.5 s. winawdl found that an iPhone's service records travel inside AWDL action-frame Service Response TLVs and must be converted to mDNS (`wa/awdl/win/service_bridge.c`); the Linux rig found the phone by plain mDNS. TODO-VERIFY.
- **Requests:** `/Discover`, then on a **new** connection `/Ask` with `TransferID = { id: UUID }` and `TransferType = { files: {} }`, then `/Upload` on the same connection repeating `TransferID`. Without the announced `TransferID` the phone closes the TLS session without an HTTP status (`mt/docs/FINDINGS.md:2505-2578`).
- **Body:** dvzip of an odc cpio starting with a `.` directory member, padded to 512 bytes.
- **Thumbnail:** optional; if present keep it small (~45 kB).

---

## 10. Open questions (ordered by when they block us)

| # | Question | Blocks | How to answer |
|---|---|---|---|
| 1 | What does the phone show for a `403` / `401` decline? | M2 toast, M6 | First end-to-end run on any working link |
| 2 | How long does the phone wait for the `/Ask` answer? | M2 toast timeout | Delay the answer in steps |
| 3 | Is the 0-chunk withheld for video / Live Photo / multiple files? | M2 completeness rule (already handles both) | Send those from the phone |
| 4 | Archive shape for multiple files, HEIC originals, Live Photos, videos | M2 extraction | Same |
| 5 | Which advertised channel sequence keeps unicast flowing when our radio never leaves ch 44? | M4/M5 | Linux rig, strategies `verbatim` (radio pinned), `intersect`, `widen` |
| 6 | How is the MT7921 firmware told which MAC to ACK in sniffer mode? | M3/M4 | Read kernel 7.0 mt76 active-monitor code; test in spike S5 |
| 7 | Why do ~half of the phone's action frames carry 2 extra bytes? | M5 sync quality | Dump one frame from a radiotap capture |
| 8 | Meaning of `DeviceSupportFlags`, `SenderIdentityAuthTag` | nothing yet | – |
| 9 | Does the URL flag bit enable link shares? | post-v0.1 | – |
