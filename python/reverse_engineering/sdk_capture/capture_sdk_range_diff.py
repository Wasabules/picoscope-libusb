#!/usr/bin/env python3
"""Capture SDK USB commands for TWO separate block captures at different ranges.

The SDK batches set_channel into the run_block compound command.
So we need to do actual captures to see the range bytes change.
"""
import ctypes
from ctypes import c_int16, c_int32, POINTER, byref
import time

lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")

print("Opening PicoScope via SDK...", flush=True)
handle = lib.ps2000_open_unit()
print(f"Handle: {handle}", flush=True)

if handle <= 0:
    print("FAILED!")
    exit(1)

time.sleep(1.0)
print("Device ready. Starting tests...\n", flush=True)


def do_capture(handle, ch_a_range, ch_b_range, label):
    """Set channels and capture, so the SDK sends the full compound command."""
    print(f"--- {label} ---", flush=True)

    # Set channels
    lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(ch_a_range))
    lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(1), c_int16(1), c_int16(ch_b_range))

    # Run block capture (forces SDK to send compound commands)
    time_indisposed = c_int32(0)
    result = lib.ps2000_run_block(
        c_int16(handle), c_int16(500), c_int16(5), c_int16(1), byref(time_indisposed))
    print(f"  run_block result: {result}", flush=True)

    # Wait for ready
    for _ in range(50):
        time.sleep(0.1)
        if lib.ps2000_ready(c_int16(handle)):
            break

    # Read data
    buf_a = (c_int16 * 500)()
    buf_b = (c_int16 * 500)()
    overflow = c_int16(0)
    n = lib.ps2000_get_values(
        c_int16(handle),
        ctypes.cast(buf_a, POINTER(c_int16)),
        ctypes.cast(buf_b, POINTER(c_int16)),
        None, None, byref(overflow), c_int32(500))
    print(f"  got {n} values, A[0]={buf_a[0]}, B[0]={buf_b[0]}", flush=True)
    time.sleep(0.3)


# Capture 1: A=5V, B=5V (same range for reference)
do_capture(handle, 8, 8, "Capture 1: A=5V, B=5V")

# Capture 2: A=2V, B=5V (different A range)
do_capture(handle, 7, 8, "Capture 2: A=2V, B=5V")

# Capture 3: A=100mV, B=5V (very different A range)
do_capture(handle, 3, 8, "Capture 3: A=100mV, B=5V")

# Capture 4: A=20V, B=5V
do_capture(handle, 10, 8, "Capture 4: A=20V, B=5V")

# Capture 5: A=5V, B=100mV (different B range)
do_capture(handle, 8, 3, "Capture 5: A=5V, B=100mV")

# Capture 6: A=5V, B=5V, AC coupling on A
print("--- Capture 6: A=5V AC, B=5V DC ---", flush=True)
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(0), c_int16(8))  # AC
lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(1), c_int16(1), c_int16(8))  # DC
time_indisposed = c_int32(0)
result = lib.ps2000_run_block(
    c_int16(handle), c_int16(500), c_int16(5), c_int16(1), byref(time_indisposed))
for _ in range(50):
    time.sleep(0.1)
    if lib.ps2000_ready(c_int16(handle)):
        break
buf_a = (c_int16 * 500)()
buf_b = (c_int16 * 500)()
overflow = c_int16(0)
lib.ps2000_get_values(
    c_int16(handle), ctypes.cast(buf_a, POINTER(c_int16)),
    ctypes.cast(buf_b, POINTER(c_int16)), None, None, byref(overflow), c_int32(500))
print(f"  A[0]={buf_a[0]}, B[0]={buf_b[0]}", flush=True)

# Capture 7: Single channel (B disabled)
print("--- Capture 7: A=5V only, B disabled ---", flush=True)
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(8))
lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(0), c_int16(1), c_int16(8))
time_indisposed = c_int32(0)
result = lib.ps2000_run_block(
    c_int16(handle), c_int16(500), c_int16(5), c_int16(1), byref(time_indisposed))
for _ in range(50):
    time.sleep(0.1)
    if lib.ps2000_ready(c_int16(handle)):
        break
buf_a = (c_int16 * 500)()
buf_b = (c_int16 * 500)()
overflow = c_int16(0)
lib.ps2000_get_values(
    c_int16(handle), ctypes.cast(buf_a, POINTER(c_int16)),
    ctypes.cast(buf_b, POINTER(c_int16)), None, None, byref(overflow), c_int32(500))
print(f"  A[0]={buf_a[0]}, B[0]={buf_b[0]}", flush=True)

print("\nClosing...", flush=True)
lib.ps2000_close_unit(c_int16(handle))
print("Done!", flush=True)
