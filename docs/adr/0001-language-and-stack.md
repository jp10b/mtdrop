# ADR-0001: Languages and stack per component

- **Status:** Accepted (2026-11-09, maintainer)
- **Deciders:** maintainer
- **Inputs:** `docs/RESEARCH.md` §3, §4, §6, §8; `docs/FINDINGS.md` F-001, F-002; `docs/PROTOCOL.md`

## Context

The project has five parts (CLAUDE.md §6): a radio driver, the AWDL daemon, the AirDrop protocol service, a tray app and an elevated helper. CLAUDE.md proposed C for the driver and the AWDL daemon, "patched OpenDrop (Python) on Windows" as the protocol MVP, and C#/.NET for the app and helper.

Research and M0 changed two premises:

1. **Stock or patched OpenDrop does not run on Windows without a rewrite** (interface lookup by name, native libarchive, shelling out to `openssl`, POSIX-only hardening, private zeroconf API – RESEARCH §4.1).
2. **We now know exactly what iOS 26.5.2 sends** (F-002). The receive path is small: three POST endpoints, binary plists, chunked bodies, dvzip, ODC cpio. Its difficulty is in the quirks, not in the volume.

Forces:

- Fewer runtimes and fewer processes mean fewer install and debugging problems for a first-time driver project.
- The protocol layer must be testable on CI without a radio (M2).
- GPL-3.0 is fixed by OWL. MIT and BSD-3-Clause-Clear code may be included; GPL-2.0-only code may not.
- The maintainer's machine already has .NET SDK 8 and 10 and Rust; it has no C toolchain (no CMake, GCC, Clang or WDK) yet.

## Decision

| Component | Language / stack | Basis |
|---|---|---|
| `driver/` | C, WDF. Driver model decided in ADR-0002 | mt76 is C; WDK samples are C |
| `awdld/` | C. OWL core (`src/` of jedbillyb/owl, GPL-3.0) unmodified where possible + our Windows backend | Reuse ~3.5 kLOC of proven protocol code and its tests |
| `airdropd/` | **C# / .NET 10**, class library `Mtdrop.Protocol` + thin host | See options below |
| `app/` | C# / .NET 10, WPF tray app, Windows App SDK toast notifications | As proposed in CLAUDE.md |
| `service/` | C# / .NET 10 Windows service (elevated) | Same runtime as the app; `pnputil`/SetupAPI via P/Invoke |
| `tools/` | Python 3 (analysis, capture decoding, fixture generation) | Already used for F-001/F-002; never shipped to users |

Build: `dotnet` for the C# parts; CMake + MSVC for `awdld/` (TODO-VERIFY that OWL builds with MSVC/clang-cl; winawdl uses MinGW-w64 – fall back to that if not); WDK + Visual Studio for `driver/`.

### `airdropd` specifics

- **Own minimal HTTP/1.1 server on `SslStream`, not Kestrel.** The phone sends no `Host` header (F-002). Kestrel rejects HTTP/1.1 requests without `Host` (TODO-VERIFY, expected 400). We also need exact control over chunked reads, early completion at `TRAILER!!!`, `Expect: 100-continue` and keep-alive between `/Ask` and `/Upload`.
- **TLS:** .NET `SslStream`, TLS 1.3 (`TLS_AES_256_GCM_SHA384` is what the phone negotiated), self-signed RSA-2048 certificate generated in-process with `CertificateRequest`. No shelling out.
- **Binary plist:** small own reader/writer (the subset in F-002: dict, array, string, data, int, bool) with tests. No dependency.
- **dvzip + ODC cpio:** own streaming decoder using `System.IO.Compression.ZLibStream`. Safe extraction rules from Foglio24 and AirDrop-for-Everything (reject absolute paths, `..`, reserved Windows names; no clobber; drop `._*` AppleDouble entries; size budget from `/Ask`).
- **mDNS:** own minimal responder bound to the `awdl0` interface index (one service, fixed record set, periodic re-announce). Windows' built-in DNS-SD registration is not interface-restricted in a way we can rely on (TODO-VERIFY) and third-party libraries announce on every interface.
- **Process model:** `airdropd` is a library hosted in the unelevated tray app process for v0.1. Accept/Decline is then a direct call, not IPC. The elevated `service/` owns the radio and `awdld`. A separate `airdropd` process stays possible later because the library has no UI dependency.

## Options considered for `airdropd`

### A. Patched OpenDrop (Python) on Windows – rejected

- (+) Exactly the code that worked in M0.
- (−) Needs a rewrite of interface binding, archive extraction, certificate generation and hardening anyway.
- (−) Ships a Python runtime plus native `archive.dll`; second runtime next to .NET.
- (−) Accept/Decline needs an IPC channel into a Python process.

### B. Rust, based on AirDrop-for-Everything (MIT) – not chosen

- (+) Has dvzip/cpio codecs with tests, safe file naming, WinRT BLE advertiser.
- (−) Never validated over AWDL against iOS 26; RESEARCH §4.2 lists defects that would break our case (header name lookup, waiting for the withheld 0-chunk, no re-announce, ECDSA certificate unproven).
- (−) Third language in the project; FFI or IPC boundary to the C# app.
- Keep as reference for codecs and, in M8, for the BLE advertiser.

### C. C# / .NET – **proposed**

- (+) One runtime for protocol, app and service; single self-contained publish; xUnit tests run on a stock GitHub Windows runner.
- (+) TLS 1.3, zlib, X.509 generation and toast notifications are in the platform.
- (+) Accept/Decline is an in-process call in v0.1.
- (−) No existing C# AirDrop receiver to copy; everything is written against `docs/PROTOCOL.md` and the F-002 shapes. Mitigated by the small scope and by fixtures.
- (−) Own mDNS responder and own plist codec to maintain (each a few hundred lines).

### D. winawdl's stdlib-only Python layer (MIT) – fallback

- (+) Proven on Windows over AWDL (their iPhone, iOS version unstated); already handles the missing 0-chunk and periodic announces.
- (−) Same second-runtime and IPC cost as A.
- Use as the behavioural reference while writing option C, and as an emergency fallback for M6 if the C# layer blocks the demo.

## Consequences

- `tests/fixtures/` holds **synthetic** requests shaped like F-002. No captured sender data.
- M2 can start immediately and needs no driver, no admin rights and no changes to the machine.
- CI: `dotnet build` + `dotnet test` on `windows-latest`; Python tool tests on the same runner.
- A C toolchain is still required for `awdld/` (M5) and the driver (M3). Installing it is a separate, explicit step in `docs/DEV-SETUP.md`.
- If the maintainer prefers Rust or Python for `airdropd`, only `airdropd/` changes; the PROTOCOL document and fixtures stay valid.

## Open points

- TODO-VERIFY: Kestrel behaviour without `Host` (only matters if someone proposes Kestrel later).
- TODO-VERIFY: OWL core builds with MSVC or clang-cl.
- TODO-VERIFY: iOS accepts our generated RSA-2048 certificate exactly as it accepted OpenDrop's.
- TODO-VERIFY: .NET multicast socket bound to a Wintun adapter by interface index.
