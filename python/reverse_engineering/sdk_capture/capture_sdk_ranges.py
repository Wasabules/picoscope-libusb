#!/usr/bin/env python3
"""Capture SDK USB commands for different ranges, channels, and coupling.

Run with: LD_PRELOAD=./usb_interceptor.so python3 capture_sdk_ranges.py
This captures the EXACT bytes the SDK sends for each setting.
"""
import ctypes
from ctypes import c_int16, c_int32, c_uint32, POINTER, byref
import time

lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")

print("=" * 60)
print("SDK Range/Channel/Coupling Command Capture")
print("=" * 60)

print("\nOpening PicoScope via SDK...")
handle = lib.ps2000_open_unit()
print(f"Handle: {handle}")

if handle <= 0:
    print("FAILED to open device!")
    exit(1)

time.sleep(0.5)

# ============================================================
# Phase 1: Different ranges on Channel A (DC coupling)
# ============================================================
print("\n" + "=" * 60)
print("PHASE 1: Channel A, DC coupling, different ranges")
print("=" * 60)

ranges = [
    (0, "10mV"), (1, "20mV"), (2, "50mV"), (3, "100mV"),
    (4, "200mV"), (5, "500mV"), (6, "1V"), (7, "2V"),
    (8, "5V"), (9, "10V"), (10, "20V"),
]

for range_val, range_name in ranges:
    print(f"\n--- MARKER: Channel A, {range_name}, DC ---")
    # Flash LED as marker between captures
    lib.ps2000_flash_led(c_int16(handle))
    time.sleep(0.05)

    result = lib.ps2000_set_channel(
        c_int16(handle),
        c_int16(0),         # Channel A
        c_int16(1),         # Enabled
        c_int16(1),         # DC coupling
        c_int16(range_val)
    )
    print(f"  set_channel result: {result}")
    time.sleep(0.2)

# ============================================================
# Phase 2: Channel B with different ranges
# ============================================================
print("\n" + "=" * 60)
print("PHASE 2: Channel B, DC coupling, selected ranges")
print("=" * 60)

for range_val, range_name in [(8, "5V"), (3, "100mV"), (10, "20V")]:
    print(f"\n--- MARKER: Channel B, {range_name}, DC ---")
    lib.ps2000_flash_led(c_int16(handle))
    time.sleep(0.05)

    result = lib.ps2000_set_channel(
        c_int16(handle),
        c_int16(1),         # Channel B
        c_int16(1),         # Enabled
        c_int16(1),         # DC coupling
        c_int16(range_val)
    )
    print(f"  set_channel result: {result}")
    time.sleep(0.2)

# ============================================================
# Phase 3: AC coupling
# ============================================================
print("\n" + "=" * 60)
print("PHASE 3: AC coupling tests")
print("=" * 60)

print(f"\n--- MARKER: Channel A, 5V, AC ---")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.05)
result = lib.ps2000_set_channel(
    c_int16(handle),
    c_int16(0),         # Channel A
    c_int16(1),         # Enabled
    c_int16(0),         # AC coupling
    c_int16(8)          # 5V
)
print(f"  set_channel result: {result}")
time.sleep(0.2)

print(f"\n--- MARKER: Channel A, 5V, DC ---")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.05)
result = lib.ps2000_set_channel(
    c_int16(handle),
    c_int16(0),         # Channel A
    c_int16(1),         # Enabled
    c_int16(1),         # DC coupling
    c_int16(8)          # 5V
)
print(f"  set_channel result: {result}")
time.sleep(0.2)

# ============================================================
# Phase 4: Dual channel capture
# ============================================================
print("\n" + "=" * 60)
print("PHASE 4: Dual channel block capture (A=5V, B=2V)")
print("=" * 60)

print(f"\n--- MARKER: Setup dual channels ---")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.05)

# Set both channels
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(8))  # A, 5V, DC
time.sleep(0.1)
lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(1), c_int16(1), c_int16(7))  # B, 2V, DC
time.sleep(0.1)

# Run block capture
print(f"\n--- MARKER: Run block capture (dual channel) ---")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.05)

time_indisposed = c_int32(0)
result = lib.ps2000_run_block(
    c_int16(handle),
    c_int16(1000),      # samples
    c_int16(5),          # timebase
    c_int16(1),          # oversample
    byref(time_indisposed)
)
print(f"  run_block result: {result}")
time.sleep(0.5)

# Check if ready
ready = lib.ps2000_ready(c_int16(handle))
print(f"  ready: {ready}")

# Get data
if ready:
    buffer_a = (c_int16 * 1000)()
    buffer_b = (c_int16 * 1000)()
    overflow = c_int16(0)

    n_values = lib.ps2000_get_values(
        c_int16(handle),
        ctypes.cast(buffer_a, POINTER(c_int16)),
        ctypes.cast(buffer_b, POINTER(c_int16)),
        None,  # buffer C
        None,  # buffer D
        byref(overflow),
        c_int32(1000)
    )
    print(f"  got {n_values} values, overflow={overflow.value}")
    if n_values > 0:
        print(f"  A[0:5]: {[buffer_a[i] for i in range(min(5, n_values))]}")
        print(f"  B[0:5]: {[buffer_b[i] for i in range(min(5, n_values))]}")

# ============================================================
# Phase 5: Disable channel
# ============================================================
print(f"\n--- MARKER: Disable Channel B ---")
lib.ps2000_flash_led(c_int16(handle))
time.sleep(0.05)
result = lib.ps2000_set_channel(
    c_int16(handle),
    c_int16(1),         # Channel B
    c_int16(0),         # Disabled
    c_int16(1),         # DC coupling
    c_int16(8)          # 5V
)
print(f"  set_channel(B, disabled) result: {result}")
time.sleep(0.2)

print("\nClosing...")
lib.ps2000_close_unit(c_int16(handle))
print("Done!")
