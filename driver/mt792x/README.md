# mt792x

Portable chip logic for the MediaTek MT7921 (and, later, its MT792x relatives),
written in plain C11 behind a small hardware abstraction layer (HAL).

The library does not know how registers are mapped, how time passes or where
DMA memory comes from. The host - a UMDF/KMDF driver, a user-mode test rig or a
unit test with a mock - supplies those through `struct mt_hal`
(`include/mt792x/hal.h`). This keeps the chip-specific code unit-testable on any
machine without hardware. See `docs/adr/0002-driver-strategy.md` for why the
project is structured this way.

What it does today: **firmware bring-up** of an MT7921 from a polling,
interrupt-less host, following `docs/MT7921-BRINGUP.md` (steps S0-S23, derived
from Linux mt76), plus an orderly shutdown and the "stop all DMA" list a proxy
replays when the client dies.

## Staged bring-up

Bring-up on real hardware is risky (the chip DMAs into host RAM), so it can stop
after any of five stages and be tested one gate at a time:

| stage | steps | what happens | left behind |
|---|---|---|---|
| 1 `OWNERSHIP` | S1-S3 | fw-own / drv-own handshake, chip id check | driver owns the chip; no reset, no DMA |
| 2 `RESET` | + S4-S7 | WFSYS reset, `HOST_INT_ENA` = 0, DMA disabled (force) | DMA off, rings not programmed |
| 3 `RINGS` | + S8-S13 | rings programmed and filled, DMA enabled, second driver-own, L0s off | DMA on, no command sent |
| 4 `MCU` | + S14-S16 | `NIC_POWER_CTRL`, wait for FW state 1, patch semaphore get **and release** | nothing held |
| 5 `FIRMWARE` | + S17-S23 | ROM patch + RAM download, `FW_START`, wait for `N9_RDY` | firmware running |

```c
static struct mt792x dev;
mt792x_init(&dev, hal);                         /* S0: layout + zero the DMA region, no register access */
/* optional: dev.opt.* switches (see mt792x.h), set after init */
err = mt792x_bringup(&dev, &fw, MT792X_STAGE_FIRMWARE);
mt792x_shutdown(&dev);                          /* always; idempotent; spec 7.3 */
```

Every step logs at INFO through `hal->log` with its step id and register proofs
(e.g. `S4 WFSYS reset: INIT_DONE after 20 ms (0x18000140 = 0x00000011)`); a failure
logs the step, the registers involved and the elapsed time, and is returned as a
specific `MT792X_E_*` code (`mt792x_strerror`). DEBUG adds every MCU command and
response header. Steps S11-S23 are not retried (mt76 retries 10 times after a
full reset; here a human reads the log first).

## Safety rules built into the code

- All DMA memory is the single HAL region. `src/layout.c` places every ring and
  buffer (table in `include/mt792x/layout.h`, 924 KiB of the 2 MiB block), and
  `mt792x_dma_bus()` is the **only** function that produces a bus address for the
  chip. It refuses (logs, returns `MT792X_E_DMA_ADDR`, nothing is written) any range
  outside the region, any misalignment, and any region ending above 4 GiB.
- The region is zeroed and every ring (descriptors and RX buffers) is fully
  initialised before the first ring register write; every descriptor always holds a
  valid address inside the region (unused TX descriptors point at an all-zero page).
- `HOST_INT_ENA` is never written non-zero; `SET_OWN` is only written in S1.
- An all-ones register read, or a ring index >= ring size, is a fatal "bus dead":
  no further writes during bring-up, and read-modify-write never writes a value
  derived from an all-ones read.
- All timeouts are elapsed-time based (`hal->now_us`).
- `MT_HIF_REMAP_L1` is written only by the L1 access path; its value at S0 is
  restored on shutdown.
- Firmware files are untrusted input and are validated completely before the first
  register access (`include/mt792x/fw.h`; usable without a device).

## Layout

```
include/mt792x/hal.h      HAL interface
include/mt792x/mt792x.h   public API: init / bringup / shutdown / stop ops / status
include/mt792x/fw.h       firmware file parsing and validation
include/mt792x/layout.h   ring sizes and DMA region map (the one place to change them)
src/regs.h                register map (from mt76)
src/layout.c              layout + mt792x_dma_bus()
src/regio.c               register access, L1 remap window, time-based polls
src/ring.c                descriptor rings (prepare, program, TX kick/wait, RX take/re-arm)
src/mcu.c                 MCU command header, sequence numbers, response matching
src/fw.c                  patch / RAM file parsing
src/bringup.c             steps S0-S23, shutdown, stop list, status
src/util.c                freestanding helpers and a tiny log formatter
test/test.h               minimal single-header test framework
test/hal_mock.[ch]        mock HAL: sparse register space, hooks, trace,
                          virtual clock, log capture, DMA region
test/fake_chip.[ch]       simulated MT7921 on top of the mock (see below)
test/fw_builder.[ch]      synthetic firmware images in the real file formats
test/rig.[ch]             mock + fake + device + default firmware, shared checks
test/test_*.c             tests (listed explicitly in CMakeLists.txt)
LICENSE.mt76              licence of the mt76 code the register map and sequence derive from
```

New source files must be added to `CMakeLists.txt` by hand (no globbing).

## Rules

- **No OS headers in `src/` or `include/`.** No `<windows.h>`, no `<wdf.h>`,
  no POSIX headers, not even `<stdio.h>`/`<string.h>`: only `<stdint.h>`,
  `<stddef.h>`, `<stdarg.h>`. Everything platform-specific arrives through the HAL.
- Warnings are errors (`/W4 /WX` on MSVC, `-Wall -Wextra -Werror -pedantic`
  elsewhere). No compiler extensions. Wire formats are built byte by byte
  (explicit endianness), never with packed structs.
- Code under `test/` may use the C standard library.

## Tests

The simulated chip (`test/fake_chip.c`) behaves as the spec says the chip does
and records a *violation* for every host mistake it can see: ownership handshake,
the L1 window (chip id / rev / bound, WFSYS reset with INIT_DONE after a virtual
delay, released < 50 ms is a violation), `GLO_CFG` enable/busy, all ring
registers, `CONN_ON_MISC` (1 after reset, 3 after `FW_START`). When a TX ring is
kicked it reads descriptors from ring memory by translating bus addresses back
into the HAL region (outside / misaligned / malformed = violation), parses MCU
commands byte-exactly (TXD fields, lengths, queue, cid, seq including the chunks
that consume sequence numbers, set_query, s2d), tracks the patch semaphore and
every download (address, length, mode, command id, chunk sizes, received bytes),
and writes events into RX ring 0 or 4 only if the host armed that ring correctly.

Fault knobs: bus dead after N accesses, fw-own / drv-own never, wrong chip id,
MT7920 bound bit, WFSYS never done, DMA busy stuck, second driver-own never, FW
state never ready, semaphore results, no response to command K, short responses,
patch-finish result, response on the other ring, RX ring not armed, TX ring
stalled, firmware never reaches N9_RDY, unsolicited / stale events.

The tests cover: layout and address checks for several bus bases (including a
region ending exactly at 4 GiB and one crossing it); each stage stopping where it
should with the promised chip state; a full bring-up delivering byte-identical
firmware at the right addresses, modes and command ids in mt76 order; the
`PATCH_IS_DL` branch; every fault knob yielding its specific error with no
DMA-enabling write afterwards (except the semaphore release mt76 mandates); a
bus-dead sweep across the whole bring-up; shutdown after every stage; the stop
list equal to spec 7.2 and leaving DMA disabled when replayed; firmware parser
rejection cases, all truncations and a deterministic fuzz loop on exactly sized
buffers.

### Windows (MSVC + Ninja, out-of-tree)

From a "x64 Native Tools" prompt, or after running
`vcvars64.bat` from your Visual Studio installation
(`VC\Auxiliary\Build\vcvars64.bat`):

```bat
cmake -S driver\mt792x -B D:\build\mt792x -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build D:\build\mt792x
ctest --test-dir D:\build\mt792x --output-on-failure
```

### Linux / macOS

```sh
cmake -S driver/mt792x -B build/mt792x -DCMAKE_BUILD_TYPE=Release
cmake --build build/mt792x
ctest --test-dir build/mt792x --output-on-failure
```

### Running tests directly

```sh
build/mt792x/mt792x_tests              # all tests
build/mt792x/mt792x_tests fault        # only tests whose name contains "fault"
MT_TEST_VERBOSE=1 build/mt792x/mt792x_tests full_firmware_bringup   # print the library log and violations
MT792X_FW_DIR=/path/to/firmware build/mt792x/mt792x_tests real_firmware   # optional, local only
```

`real_firmware_metadata` parses `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and
`WIFI_RAM_CODE_MT7961_1.bin` from `MT792X_FW_DIR` and prints their section and
region tables; it is skipped when the variable is not set. Never commit firmware.

## Provenance

Register definitions, constants and the bring-up sequence in `src/` are derived
from Linux mt76 (openwrt/mt76 @ 0936617, BSD-3-Clause-Clear; see `LICENSE.mt76`
and the file headers). Our code is GPL-3.0-or-later.
