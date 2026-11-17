# SPDX-License-Identifier: GPL-3.0-or-later
# awdld: OWL's AWDL core (owl/, jedbillyb/owl @ 57b8678) on a Windows I/O layer (win/, compat/, adapted from
# raph559/winawdl awdl/ @ 0db7031), talking to `radiod serve` over the radio link (driver/radiod/radio_link.*).
# OWL is GNU C, hence MinGW-w64 GCC (docs/adr/0003-awdld-on-windows.md); radiod stays on MSVC.
#
#   cmake -S awdld -B <build dir> -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build dir> && ctest --test-dir <build dir>
#
# Origin of this file: raph559/winawdl awdl/CMakeLists.txt @ 0db7031 (GPL-3.0-or-later); paths, the radio link
# and the test list changed for mtdrop.
cmake_minimum_required(VERSION 3.15)
project(mtdrop_awdld C)

if(NOT MINGW)
  message(FATAL_ERROR "awdld builds with MinGW-w64 GCC (OWL is GNU C); see docs/adr/0003-awdld-on-windows.md")
endif()

get_filename_component(REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# File names in log lines start at the repository, not at the folder it was cloned into.
add_compile_options("-ffile-prefix-map=${REPOSITORY}=.")

set(OWL "${CMAKE_CURRENT_SOURCE_DIR}/owl")
set(RADIO_LINK "${REPOSITORY}/driver/radiod")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# compat/ comes first so OWL's Unix includes resolve to the shims.
# -mno-ms-bitfields lays packed structs out as GCC does on Linux.
add_library(owl_build INTERFACE)
target_include_directories(owl_build INTERFACE
    compat "${OWL}/src" radiotap win "${RADIO_LINK}")
target_compile_options(owl_build INTERFACE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/owl_win.h" -mno-ms-bitfields)

add_library(radiotap STATIC radiotap/radiotap.c)
target_link_libraries(radiotap PRIVATE owl_build)
# Its bounds checks subtract pointers through unsigned long, which is 32 bits on Windows. The differences are
# small, so the truncation cancels out.
target_compile_options(radiotap PRIVATE -Wno-pointer-to-int-cast)

set(OWL_SOURCES log state election sync channel schedule tx rx frame crc32 wire
    peers version hashmap siphash24 circular_buffer)
list(TRANSFORM OWL_SOURCES PREPEND "${OWL}/src/")
list(TRANSFORM OWL_SOURCES APPEND ".c")
# compat/ether.c: ether_ntoa / ether_aton, which OWL itself calls.
add_library(awdl STATIC ${OWL_SOURCES} compat/ether.c)
target_link_libraries(awdl PUBLIC owl_build radiotap)

# The radio link is plain C11 shared with radiod (MSVC); built here without owl_build's packed-struct options.
add_library(radio_link STATIC "${RADIO_LINK}/radio_link.c")
target_include_directories(radio_link PUBLIC "${RADIO_LINK}")
target_compile_options(radio_link PRIVATE -Wall -Wextra -Werror -pedantic)

# The host adapter is kept clear of owl_build: -mno-ms-bitfields must not reach the Windows networking
# structures it fills in.
add_library(host_if STATIC win/host_if.c win/host_frame.c)
target_include_directories(host_if PUBLIC win)
target_compile_options(host_if PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_if PRIVATE iphlpapi)

set(WIN_SOURCES
    win/main.c win/ev_win.c win/io_win.c win/netutils_win.c win/radio_client.c
    win/redact.c win/frame_fixup.c win/tcp_merge.c win/service_bridge.c)
add_executable(awdld "${OWL}/daemon/core.c" ${WIN_SOURCES})
set_source_files_properties(${WIN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
# daemon/ only here: its io.h would shadow the system <io.h> for everything else (googletest needs that one).
target_include_directories(awdld BEFORE PRIVATE "${OWL}/daemon")
target_link_libraries(awdld PRIVATE awdl host_if radio_link)
# Static, so the elevated daemon does not need MinGW runtime DLLs beside it.
# The wrap routes OWL's log calls through main.c, which can redact them.
target_link_options(awdld PRIVATE -static -Wl,--wrap=log_log)

enable_testing()
foreach(t host_frame tcp_merge frame_fixup service_bridge redact)
  add_executable(${t}_test tests/${t}_test.c)
  target_include_directories(${t}_test PRIVATE win)
  target_compile_options(${t}_test PRIVATE -Wall -Wextra -Werror)
  target_link_options(${t}_test PRIVATE -static)
  add_test(NAME ${t} COMMAND ${t}_test)
endforeach()
target_link_libraries(host_frame_test PRIVATE host_if)
target_sources(tcp_merge_test PRIVATE win/tcp_merge.c)
target_sources(frame_fixup_test PRIVATE win/frame_fixup.c)
target_sources(service_bridge_test PRIVATE win/service_bridge.c win/frame_fixup.c)
target_sources(redact_test PRIVATE win/redact.c)

# The radio link's own tests, here built with GCC (radiod builds them with MSVC).
add_executable(radio_link_test "${RADIO_LINK}/radio_link_test.c")
target_link_libraries(radio_link_test PRIVATE radio_link)
target_compile_options(radio_link_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_options(radio_link_test PRIVATE -static)
add_test(NAME radio_link COMMAND radio_link_test)

# OWL's own googletest suite (owl/tests, jedbillyb/owl @ 57b8678), against the same OWL build as the daemon.
option(MTDROP_OWL_TESTS "Build OWL's googletest suite (downloads googletest 1.15.2 at configure time)" ON)
if(MTDROP_OWL_TESTS)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 14)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_executable(owl_tests
    owl/tests/test_wire.cpp owl/tests/test_awdl_sync.cpp owl/tests/test_awdl_peers.cpp
    owl/tests/test_awdl_election.cpp owl/tests/test_awdl_channel.cpp owl/tests/test_awdl_txgate.cpp
    owl/tests/test_awdl_rx.cpp)
  target_link_libraries(owl_tests PRIVATE awdl gtest gtest_main)
  target_link_options(owl_tests PRIVATE -static)
  add_test(NAME owl COMMAND owl_tests)
endif()
