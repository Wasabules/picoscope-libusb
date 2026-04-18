#!/usr/bin/env python3
"""Capture SDK USB commands for dual-channel block capture."""
import ctypes
import time

sdk = ctypes.CDLL('/opt/picoscope/lib/libps2000.so')

handle = sdk.ps2000_open_unit()
print(f"Handle: {handle}")
if handle <= 0:
    print("Cannot open device")
    exit(1)

PS2000_5V = 7
PS2000_500MV = 4

# Enable both channels
print("\n--- Setting up BOTH channels ---")
sdk.ps2000_set_channel(handle, ctypes.c_int16(0), ctypes.c_int16(1),
                       ctypes.c_int16(1), ctypes.c_int16(PS2000_5V))   # Ch A, enabled, DC, 5V
sdk.ps2000_set_channel(handle, ctypes.c_int16(1), ctypes.c_int16(1),
                       ctypes.c_int16(1), ctypes.c_int16(PS2000_500MV)) # Ch B, enabled, DC, 500mV

timebase = 5
num_samples = 1000

# Get timebase
time_interval = ctypes.c_int32(0)
time_units = ctypes.c_int16(0)
max_samples = ctypes.c_int32(0)
sdk.ps2000_get_timebase(handle, ctypes.c_int16(timebase),
                        ctypes.c_int32(num_samples),
                        ctypes.byref(time_interval),
                        ctypes.byref(time_units),
                        ctypes.c_int16(1),
                        ctypes.byref(max_samples))
print(f"Timebase: {timebase}, interval: {time_interval.value}, max: {max_samples.value}")

# Run block
print("\n--- Running block capture (dual channel) ---")
time_indisposed = ctypes.c_int32(0)
sdk.ps2000_run_block(handle, ctypes.c_int32(num_samples),
                     ctypes.c_int16(timebase),
                     ctypes.c_int16(1),
                     ctypes.byref(time_indisposed))

while sdk.ps2000_ready(handle) == 0:
    time.sleep(0.01)

# Get values for both channels
buffer_a = (ctypes.c_int16 * num_samples)()
buffer_b = (ctypes.c_int16 * num_samples)()
overflow = ctypes.c_int16(0)
num_got = sdk.ps2000_get_values(handle, ctypes.byref(buffer_a),
                                ctypes.byref(buffer_b),
                                None, None,
                                ctypes.byref(overflow),
                                ctypes.c_int32(num_samples))

print(f"Got {num_got} samples per channel")

import numpy as np
data_a = np.array(buffer_a[:num_got], dtype=np.int16)
data_b = np.array(buffer_b[:num_got], dtype=np.int16)

mv_a = data_a * 5000.0 / 32767.0
mv_b = data_b * 500.0 / 32767.0

print(f"CH A (5V):    raw_mean={np.mean(data_a):.0f}  mV_mean={np.mean(mv_a):.1f}mV  mV_std={np.std(mv_a):.1f}mV")
print(f"CH B (500mV): raw_mean={np.mean(data_b):.0f}  mV_mean={np.mean(mv_b):.1f}mV  mV_std={np.std(mv_b):.1f}mV")

sdk.ps2000_close_unit(handle)
print("Done!")
