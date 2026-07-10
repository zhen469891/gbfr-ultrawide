cmake_minimum_required(VERSION 3.21)

project(GBFRUltrawide LANGUAGES C CXX)

# x64 only
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "GBFRUltrawide is x64 only. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Development build: honors the [Debug - Span HUD] ini section (Probe element
# logging + EdgeSnapIds/MoveIds per-element overrides). OFF = release build,
# which compiles all of that out and ignores the section entirely.
option(GBFR_DEV "Enable development/debug features ([Debug - Span HUD] ini section)" OFF)

# Static CRT
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(GBFRUltrawide SHARED
    src/dllmain.cpp
    vendor/safetyhook/safetyhook.cpp
    vendor/safetyhook/Zydis.c
)

# ASI plugin: plain "GBFRUltrawide.asi"
set_target_properties(GBFRUltrawide PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
)

target_include_directories(GBFRUltrawide PRIVATE
    src
    vendor
    vendor/safetyhook
    vendor/inipp
)

target_compile_definitions(GBFRUltrawide PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

if(GBFR_DEV)
    target_compile_definitions(GBFRUltrawide PRIVATE GBFR_DEVBUILD)
endif()
