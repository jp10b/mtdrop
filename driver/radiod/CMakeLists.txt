cmake_minimum_required(VERSION 3.20)
project(radiod C)

if(NOT WIN32)
  message(FATAL_ERROR "radiod is the Windows host of the chip library")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_subdirectory(../mt792x mt792x)

add_executable(radiod main.c hal_win.c)
target_link_libraries(radiod PRIVATE mt792x cfgmgr32 advapi32)
target_compile_definitions(radiod PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(radiod PRIVATE /W4 /WX /utf-8)
