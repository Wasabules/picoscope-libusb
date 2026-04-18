#!/usr/bin/env python3
"""Capture SDK USB commands for different ranges.
Run WITHOUT interceptor - use tshark/usbmon to capture traffic separately.
"""
import ctypes
from ctypes import c_int16, c_int32, c_uint32, POINTER, byref
import time
import sys

lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")

print("Opening PicoScope via SDK...", flush=True)
handle = lib.ps2000_open_unit()
print(f"Handle: {handle}", flush=True)

if handle <= 0:
    print("FAILED to open device!")
    sys.exit(1)

time.sleep(1.0)
print("Device open. Starting range tests in 2 seconds...", flush=True)
time.sleep(2.0)

# ============================================================
# Phase 1: Different ranges on Channel A (DC coupling)
# Each range change separated by a flash_led marker
# ============================================================
ranges = [
    (0, "10mV"), (1, "20mV"), (2, "50mV"), (3, "100mV"),
    (4, "200mV"), (5, "500mV"), (6, "1V"), (7, "2V"),
    (8, "5V"), (9, "10V"), (10, "20V"),
]

for range_val, range_name in ranges:
    print(f"Setting Channel A, {range_name}, DC...", flush=True)
    time.sleep(0.3)
    result = lib.ps2000_set_channel(
        c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(range_val))
    print(f"  result: {result}", flush=True)
    time.sleep(0.3)

# Phase 2: Channel B
print("\n--- Channel B tests ---", flush=True)
for range_val, range_name in [(8, "5V"), (3, "100mV"), (10, "20V")]:
    print(f"Setting Channel B, {range_name}, DC...", flush=True)
    time.sleep(0.3)
    result = lib.ps2000_set_channel(
        c_int16(handle), c_int16(1), c_int16(1), c_int16(1), c_int16(range_val))
    print(f"  result: {result}", flush=True)
    time.sleep(0.3)

# Phase 3: AC/DC coupling
print("\n--- Coupling tests ---", flush=True)
print("Channel A, 5V, AC...", flush=True)
time.sleep(0.3)
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(0), c_int16(8))
time.sleep(0.3)
print("Channel A, 5V, DC...", flush=True)
time.sleep(0.3)
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(8))
time.sleep(0.3)

# Phase 4: Dual channel capture
print("\n--- Dual channel capture ---", flush=True)
time.sleep(0.3)
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(8))  # A, 5V, DC
time.sleep(0.2)
lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(1), c_int16(1), c_int16(7))  # B, 2V, DC
time.sleep(0.2)

time_indisposed = c_int32(0)
result = lib.ps2000_run_block(
    c_int16(handle), c_int16(1000), c_int16(5), c_int16(1), byref(time_indisposed))
print(f"run_block result: {result}", flush=True)

# Wait for data
for _ in range(50):
    time.sleep(0.1)
    if lib.ps2000_ready(c_int16(handle)):
        break

buffer_a = (c_int16 * 1000)()
buffer_b = (c_int16 * 1000)()
overflow = c_int16(0)
n_values = lib.ps2000_get_values(
    c_int16(handle),
    ctypes.cast(buffer_a, POINTER(c_int16)),
    ctypes.cast(buffer_b, POINTER(c_int16)),
    None, None,
    byref(overflow),
    c_int32(1000))
print(f"got {n_values} values", flush=True)
if n_values > 0:
    print(f"  A[0:5]: {[buffer_a[i] for i in range(min(5, n_values))]}")
    print(f"  B[0:5]: {[buffer_b[i] for i in range(min(5, n_values))]}")

# Phase 5: Disable channel B
print("\n--- Disable Channel B ---", flush=True)
time.sleep(0.3)
lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(0), c_int16(1), c_int16(8))
time.sleep(0.3)

print("\nClosing...", flush=True)
lib.ps2000_close_unit(c_int16(handle))
print("Done!", flush=True)
