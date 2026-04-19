cmake_minimum_required(VERSION 3.22.1)
project(picoscope_jni C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# libusb-1.0 — fetched and built from source so the AAR is self-contained
# and JitPack doesn't need any extra setup.
#
# libusb-cmake is the official CMake-friendly packaging of libusb/libusb.
# ---------------------------------------------------------------------------
include(FetchContent)

set(LIBUSB_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_TESTING    OFF CACHE BOOL "" FORCE)
set(LIBUSB_INSTALL_TARGETS  OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG        v1.0.27
)
FetchContent_MakeAvailable(libusb)

# ---------------------------------------------------------------------------
# PicoScope 2204A driver — single source of truth in ../../../../driver/
# ---------------------------------------------------------------------------
set(PS_DRIVER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../driver)

add_library(picoscope2204a_static STATIC
    ${PS_DRIVER_ROOT}/picoscope2204a.c
)

target_include_directories(picoscope2204a_static PUBLIC
    ${PS_DRIVER_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
)

target_link_libraries(picoscope2204a_static PUBLIC
    usb-1.0
    log         # Android __android_log_print (and __ANDROID__ is set
                # automatically by the NDK toolchain)
)

target_compile_options(picoscope2204a_static PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -Wno-sign-compare -Wno-missing-field-initializers
)

# ---------------------------------------------------------------------------
# JNI shim — loaded by PicoScope2204A.java via System.loadLibrary()
# ---------------------------------------------------------------------------
add_library(picoscope_jni SHARED
    picoscope_jni.c
)

target_link_libraries(picoscope_jni PRIVATE
    picoscope2204a_static
)

# Android 15+ requires 16 KB page-size alignment for shared libraries on
# devices that use 16 KB pages (Pixel 8/9, certain tablets). Without this
# the OS refuses to load the .so on those devices.
target_link_options(picoscope_jni PRIVATE
    "-Wl,-z,max-page-size=16384"
)
