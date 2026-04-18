cmake_minimum_required(VERSION 3.13)
project(picoscope2204a C)

set(CMAKE_C_STANDARD 11)

option(PS2204A_BUILD_TEST "Build test program" ON)
option(PS2204A_BUILD_JNI "Build JNI wrapper for Android" OFF)

# Find libusb
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB libusb-1.0)
endif()

if(NOT LIBUSB_FOUND)
    # Fallback: try to find it directly
    find_path(LIBUSB_INCLUDE_DIRS libusb-1.0/libusb.h
              PATHS /usr/include /usr/local/include)
    find_library(LIBUSB_LIBRARIES usb-1.0
                 PATHS /usr/lib /usr/local/lib)
    if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
        set(LIBUSB_FOUND TRUE)
    endif()
endif()

if(NOT LIBUSB_FOUND)
    message(FATAL_ERROR "libusb-1.0 not found. Install libusb-1.0-0-dev")
endif()

# Firmware is loaded at runtime from disk (see docs/firmware-extraction.md),
# not embedded, so there is nothing to generate at configure time.

# Shared library
add_library(picoscope2204a SHARED picoscope2204a.c)
target_include_directories(picoscope2204a PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUSB_INCLUDE_DIRS}
)
target_link_libraries(picoscope2204a ${LIBUSB_LIBRARIES} pthread m)
target_compile_options(picoscope2204a PRIVATE -Wall -Wextra -O2)

# Static library (useful for Android)
add_library(picoscope2204a_static STATIC picoscope2204a.c)
target_include_directories(picoscope2204a_static PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUSB_INCLUDE_DIRS}
)
target_link_libraries(picoscope2204a_static ${LIBUSB_LIBRARIES} pthread m)
target_compile_options(picoscope2204a_static PRIVATE -Wall -Wextra -O2)

# Test program
if(PS2204A_BUILD_TEST)
    add_executable(test_driver test_driver.c)
    target_link_libraries(test_driver picoscope2204a)
endif()

# JNI wrapper
if(PS2204A_BUILD_JNI)
    find_package(JNI REQUIRED)
    add_library(picoscope_jni SHARED jni/picoscope_jni.c)
    target_include_directories(picoscope_jni PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${JNI_INCLUDE_DIRS}
    )
    target_link_libraries(picoscope_jni picoscope2204a_static)
endif()

# Install
install(TARGETS picoscope2204a LIBRARY DESTINATION lib)
install(FILES picoscope2204a.h DESTINATION include)
