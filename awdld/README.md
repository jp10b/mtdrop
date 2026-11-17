# awdld

The AWDL daemon (docs/adr/0003-awdld-on-windows.md): OWL's protocol core on a Windows I/O layer, talking to the
radio server `radiod serve` over the radio link (`driver/radiod/radio_link.h`) and to Windows through a Wintun
adapter `awdl0`.

| directory | what | origin (details in `third_party/NOTICE`) |
|---|---|---|
| `owl/` | AWDL core (`src/`), event core (`daemon/core.c`), OWL's googletest suite (`tests/`) | jedbillyb/owl @ 57b8678, GPL-3.0-or-later, unchanged |
| `radiotap/` | radiotap parser OWL uses | radiotap-library @ 94984dd, ISC, unchanged |
| `compat/` | POSIX / libev / libpcap shims OWL needs on Windows | raph559/winawdl @ 0db7031, GPL-3.0-or-later |
| `win/` | Windows I/O: event loop, radio client, Wintun adapter, Ethernet ↔ IPv6, frame fixups, log redaction | winawdl (adapted) + `radio_client.c` (own) |
| `tests/` | tests of the Windows layer | winawdl |

## Build (MinGW-w64 GCC only: OWL is GNU C)

```
cmake -S awdld -B <build> -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build <build>
ctest --test-dir <build> --output-on-failure
```

Configure downloads googletest 1.15.2 (pinned SHA-256) for OWL's tests; `-DMTDROP_OWL_TESTS=OFF` skips them. On
this machine GCC comes from winget `BrechtSanders.WinLibs.POSIX.UCRT`; use the official cmake, not the one bundled
with WinLibs (no CA certificates, the download fails). Tests: the Windows layer (5), the radio link built with GCC,
OWL's 68 googletest cases.

## Run

```
awdld [-c 6|44|149] [-S verbatim|rotate|pin|widen|intersect] [-W n] [-w s] [-f] [-n] [-r] [-s 1|2] [-l] [-j us] [-v] [-v]
```

Start `radiod serve <channel> ... --mac random` first (or after: awdld waits `-w` seconds, default 60). awdld
takes its AWDL address from the server's HELLO, sets the AWDL BSSID filter, and checks that the server is on
`-c`. `-n` runs without the Wintun adapter. `-r` replaces device addresses and names in the log by `mac-N` /
`(hidden)`: use it for every log that may be shared.

Offline, with no hardware: `radiod serve 44 - - --loopback --mac random` and `awdld -c 44 -n -w 5`.

The Wintun adapter needs the official signed `wintun.dll` (amd64) next to `awdld.exe` and an elevated process;
it is not part of this repository.
