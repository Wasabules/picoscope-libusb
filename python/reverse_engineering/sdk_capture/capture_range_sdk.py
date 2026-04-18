#!/usr/bin/env python3
"""Capture SDK USB commands for different voltage ranges.

Run with: LD_PRELOAD=./usb_interceptor.so python3 capture_range_sdk.py
"""
import ctypes
from ctypes import c_int16
import time

lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")

print("Opening PicoScope via SDK...")
handle = lib.ps2000_open_unit()
print(f"Handle: {handle}")

if handle <= 0:
    print("Failed to open device")
    exit(1)

time.sleep(0.5)

# Test different ranges with clear markers
ranges = [
    (8, "5V"),
    (10, "20V"),
    (3, "100mV"),
    (6, "1V"),
    (0, "10mV"),
]

for range_val, range_name in ranges:
    print(f"\n=== Setting Channel A to {range_name} (range={range_val}) ===")
    # Marker: flash LED to create a visible separator in the trace
    lib.ps2000_flash_led(c_int16(handle))
    time.sleep(0.1)

    result = lib.ps2000_set_channel(
        c_int16(handle),
        c_int16(0),      # Channel A
        c_int16(1),      # Enabled
        c_int16(1),      # DC coupling
        c_int16(range_val)
    )
    print(f"  Result: {result}")
    time.sleep(0.3)

# Also set Channel B to see if it differs
print(f"\n=== Setting Channel B to 5V ===")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.1)
result = lib.ps2000_set_channel(
    c_int16(handle),
    c_int16(1),      # Channel B
    c_int16(1),      # Enabled
    c_int16(1),      # DC coupling
    c_int16(8)       # 5V
)
print(f"  Result: {result}")
time.sleep(0.3)

print(f"\n=== Setting Channel B to 100mV ===")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.1)
result = lib.ps2000_set_channel(
    c_int16(handle),
    c_int16(1),      # Channel B
    c_int16(1),      # Enabled
    c_int16(1),      # DC coupling
    c_int16(3)       # 100mV
)
print(f"  Result: {result}")
time.sleep(0.3)

print("\nClosing...")
lib.ps2000_close_unit(c_int16(handle))
print("Done")
