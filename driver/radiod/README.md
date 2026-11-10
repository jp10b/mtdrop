# radiod

User-mode host of the `mt792x` chip library on Windows (ADR-0002, option A).

- `hal_win.c`: HAL backend. Registers go through the UMDF proxy (`driver/proxy`); DMA memory is one locked 2 MiB physical block below 4 GiB.
- `main.c`: bring-up commands.
  - `radiod dma-probe` allocates, checks and frees the DMA block without touching any device.
  - `radiod bringup <stage 1-5> <patch file> <ram file>` runs the staged firmware bring-up of `docs/MT7921-BRINGUP.md` (1 ownership, 2 reset, 3 rings, 4 MCU, 5 firmware). Order: read and validate both firmware files, allocate the DMA block, open the proxy, arm the proxy's stop list (`mt792x_stop_ops`, replayed by the proxy when the handle closes) **before** `mt792x_bringup`, run to the requested stage, print `RESULT: PASS/FAIL`, then always `mt792x_shutdown` and close the proxy before the DMA block is freed. **Touches real hardware: only with the maintainer's explicit go-ahead, the proxy bound and the safety checklist of `docs/DEV-SETUP.md` done.**

Needs `SeLockMemoryPrivilege`, so it runs as SYSTEM (`scripts/run-as-system.ps1` + `scripts/radiod-run.ps1` during development; the elevated service later).

Build (MSVC + Ninja, out of tree): `cmake -S driver/radiod -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build <build dir>`.

Known gap: `sleep_us` for waits of 2 ms and more uses `Sleep()`, which rounds up to the system timer tick (measured 14.6 ms for 5.5 ms). Fine for bring-up polling (all library timeouts are elapsed-time based); must be replaced by a high-resolution timer before channel timing matters.
