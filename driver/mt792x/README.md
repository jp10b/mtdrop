# mt792x

Portable chip logic for the MediaTek MT7921 (and, later, its MT792x relatives),
written in plain C11 behind a small hardware abstraction layer (HAL).

The library does not know how registers are mapped, how time passes or where
DMA memory comes from. The host - a UMDF/KMDF driver, a user-mode test rig or a
unit test with a mock - supplies those through `struct mt_hal`
(`include/mt792x/hal.h`). This keeps the chip-specific code unit-testable on any
machine without hardware. See `docs/adr/0002-driver-strategy.md` for why the
project is structured this way.

## Rules

- **No OS headers in `src/` or `include/`.** No `<windows.h>`, no `<wdf.h>`,
  no POSIX headers. Only freestanding-friendly C headers such as `<stdint.h>`
  and `<stddef.h>`. Everything platform-specific arrives through the HAL.
- Warnings are errors (`/W4 /WX` on MSVC, `-Wall -Wextra -Werror -pedantic`
  elsewhere). No compiler extensions.
- Code under `test/` may use the C standard library (the mock HAL allocates
  memory and reads `MT_TEST_VERBOSE`).

## Layout

```
include/mt792x/hal.h      HAL interface
include/mt792x/mt792x.h   public API
src/                      library sources (listed explicitly in CMakeLists.txt)
test/test.h               minimal single-header test framework
test/hal_mock.[ch]        mock HAL: sparse register space, hooks, trace,
                          virtual clock, log capture, DMA region
test/test_*.c             tests (listed explicitly in CMakeLists.txt)
```

New source files must be added to `CMakeLists.txt` by hand (no globbing).

## Build and test

### Windows (MSVC + Ninja, out-of-tree)

From a "x64 Native Tools" prompt, or after running
`vcvars64.bat` from your Visual Studio installation
(`VC\Auxiliary\Build\vcvars64.bat`):

```bat
cmake -S driver\mt792x -B D:\build\mt792x -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build D:\build\mt792x
ctest --test-dir D:\build\mt792x --output-on-failure
```

Pick any build directory outside the source tree. Without Ninja you can use the
default Visual Studio generator instead:

```bat
cmake -S driver\mt792x -B build\mt792x
cmake --build build\mt792x --config Release
ctest --test-dir build\mt792x -C Release --output-on-failure
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
build/mt792x/mt792x_tests mock_trace   # only tests whose name contains "mock_trace"
MT_TEST_VERBOSE=1 build/mt792x/mt792x_tests   # also print captured HAL log messages
```

## Writing a test

```c
#include "test.h"
#include "hal_mock.h"

TEST(example)
{
    struct mock m;
    CHECK(mock_init(&m, 4096) == 0);          /* 4 KiB zeroed DMA region */
    mock_hal(&m)->wr(mock_hal(&m)->ctx, 0x1000, 0x1u);
    CHECK_EQ_U32(mock_get_reg(&m, 0x1000), 0x1u);
    CHECK_EQ_U32(m.bad_access, 0);            /* no out-of-range accesses */
    mock_free(&m);
}
```

Tests register themselves; `test/test_main.c` provides `main()`.
