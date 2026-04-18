#!/usr/bin/env python3
"""Test PGA using the official PicoScope SDK (ctypes) to verify hardware works."""
import ctypes
import numpy as np
import time

# Load the ps2000 SDK
sdk = ctypes.CDLL('/opt/picoscope/lib/libps2000.so')

# Open device
handle = ctypes.c_int16(0)
handle = sdk.ps2000_open_unit()
print(f"Device handle: {handle}")
if handle <= 0:
    print("ERROR: Could not open device with SDK")
    exit(1)

# Get device info
info_buf = ctypes.create_string_buffer(80)
for info_type, name in [(0, "Driver"), (1, "USB"), (3, "Variant"), (4, "Serial"), (5, "Cal Date")]:
    sdk.ps2000_get_unit_info(handle, info_buf, ctypes.c_int16(80), ctypes.c_int16(info_type))
    print(f"  {name}: {info_buf.value.decode()}")

# SDK range constants
PS2000_50MV  = 1
PS2000_100MV = 2
PS2000_200MV = 3
PS2000_500MV = 4
PS2000_1V    = 5
PS2000_2V    = 6
PS2000_5V    = 7
PS2000_10V   = 8
PS2000_20V   = 9

# Set up channel A with different ranges and capture
print("\n=== PGA Test with Official SDK ===\n")

timebase = 5
num_samples = 1000
oversample = 1

for range_val, name, range_mv in [
    (PS2000_50MV, "50mV", 50),
    (PS2000_500MV, "500mV", 500),
    (PS2000_5V, "5V", 5000),
    (PS2000_20V, "20V", 20000),
]:
    # Set channel A
    sdk.ps2000_set_channel(handle, ctypes.c_int16(0), ctypes.c_int16(1),
                           ctypes.c_int16(1), ctypes.c_int16(range_val))  # Ch A, enabled, DC, range
    # Disable channel B
    sdk.ps2000_set_channel(handle, ctypes.c_int16(1), ctypes.c_int16(0),
                           ctypes.c_int16(1), ctypes.c_int16(PS2000_5V))

    # Set timebase
    time_interval = ctypes.c_int32(0)
    time_units = ctypes.c_int16(0)
    max_samples = ctypes.c_int32(0)
    ret = sdk.ps2000_get_timebase(handle, ctypes.c_int16(timebase),
                                  ctypes.c_int32(num_samples),
                                  ctypes.byref(time_interval),
                                  ctypes.byref(time_units),
                                  ctypes.c_int16(oversample),
                                  ctypes.byref(max_samples))

    # Run block capture
    time_indisposed = ctypes.c_int32(0)
    ret = sdk.ps2000_run_block(handle, ctypes.c_int32(num_samples),
                               ctypes.c_int16(timebase),
                               ctypes.c_int16(oversample),
                               ctypes.byref(time_indisposed))

    # Wait for data
    while sdk.ps2000_ready(handle) == 0:
        time.sleep(0.01)

    # Get data
    buffer_a = (ctypes.c_int16 * num_samples)()
    buffer_b = (ctypes.c_int16 * num_samples)()
    overflow = ctypes.c_int16(0)
    num_got = sdk.ps2000_get_values(handle, ctypes.byref(buffer_a),
                                    ctypes.byref(buffer_b),
                                    None, None,
                                    ctypes.byref(overflow),
                                    ctypes.c_int32(num_samples))

    data = np.array(buffer_a[:num_got], dtype=np.int16)
    raw_mean = np.mean(data)
    raw_std = np.std(data)
    # SDK returns values scaled to ADC range (-32767 to +32767)
    # Convert to mV: value * range_mv / 32767
    mv_mean = raw_mean * range_mv / 32767.0
    print(f"  {name:>5s}: raw_mean={raw_mean:.0f}  raw_std={raw_std:.0f}  mV_mean={mv_mean:.1f}  n={num_got}")

sdk.ps2000_close_unit(handle)
print("\nDone!")
