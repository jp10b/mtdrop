cmake_minimum_required(VERSION 3.20)

project(mt792x VERSION 0.0.1 LANGUAGES C)

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
)
target_include_directories(mt792x PUBLIC include)
target_compile_options(mt792x PRIVATE ${MT792X_WARNINGS})

# ---- unit tests ----
enable_testing()

add_executable(mt792x_tests
  test/test_main.c
  test/test_mock.c
  test/hal_mock.c
)
target_include_directories(mt792x_tests PRIVATE test)
target_link_libraries(mt792x_tests PRIVATE mt792x)
target_compile_options(mt792x_tests PRIVATE ${MT792X_WARNINGS})
if(MSVC)
  # The mock reads MT_TEST_VERBOSE with getenv(), which MSVC flags as deprecated.
  target_compile_definitions(mt792x_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_test(NAME mt792x_tests COMMAND mt792x_tests)
