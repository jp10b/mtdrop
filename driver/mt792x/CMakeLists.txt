cmake_minimum_required(VERSION 3.20)

project(mt792x VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
  set(MT792X_WARNINGS /W4 /WX /utf-8)
else()
  set(MT792X_WARNINGS -Wall -Wextra -Werror -pedantic)
endif()

# ---- portable library: no OS headers allowed in src/ or include/ ----
add_library(mt792x STATIC
  src/version.c
  src/util.c
  src/regio.c
  src/layout.c
  src/ring.c
  src/mcu.c
  src/fw.c
  src/bringup.c
  src/rx.c
  src/postboot.c
  src/sta.c
  src/tx.c
)
target_include_directories(mt792x PUBLIC include)
target_compile_options(mt792x PRIVATE ${MT792X_WARNINGS})

# ---- unit tests ----
enable_testing()

add_executable(mt792x_tests
  test/test_main.c
  test/test_mock.c
  test/hal_mock.c
  test/fake_chip.c
  test/fw_builder.c
  test/rig.c
  test/test_layout.c
  test/test_fw.c
  test/test_bringup.c
  test/test_faults.c
  test/test_shutdown.c
  test/test_realfw.c
  test/rx_builder.c
  test/test_sniffer.c
  test/test_rx.c
  test/test_paged.c
  test/test_tx.c
  test/test_irq.c
  test/test_wake.c
)
target_include_directories(mt792x_tests PRIVATE test)
target_link_libraries(mt792x_tests PRIVATE mt792x)
target_compile_options(mt792x_tests PRIVATE ${MT792X_WARNINGS})
if(MSVC)
  # The mock reads MT_TEST_VERBOSE with getenv(), the real-firmware test uses fopen().
  target_compile_definitions(mt792x_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_test(NAME mt792x_tests COMMAND mt792x_tests)
