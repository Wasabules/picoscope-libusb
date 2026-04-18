#!/usr/bin/env python3
"""Capture SDK USB commands for multiple range configurations (dual channel).
Compare with single-channel to identify dual-channel encoding differences."""
import ctypes
import time
import numpy as np

sdk = ctypes.CDLL('/opt/picoscope/lib/libps2000.so')

handle = sdk.ps2000_open_unit()
print(f"Handle: {handle}")
if handle <= 0:
    print("Cannot open device")
    exit(1)

RANGES = {
    1: ("50mV", 50), 2: ("100mV", 100), 3: ("200mV", 200),
    4: ("500mV", 500), 5: ("1V", 1000), 6: ("2V", 2000),
    7: ("5V", 5000), 8: ("10V", 10000), 9: ("20V", 20000),
}

def capture(label):
    """Do a block capture and return sample counts."""
    num = 1000
    time_interval = ctypes.c_int32(0)
    time_units = ctypes.c_int16(0)
    max_samples = ctypes.c_int32(0)
    sdk.ps2000_get_timebase(handle, ctypes.c_int16(5),
                            ctypes.c_int32(num),
                            ctypes.byref(time_interval),
                            ctypes.byref(time_units),
                            ctypes.c_int16(1),
                            ctypes.byref(max_samples))

    time_indisposed = ctypes.c_int32(0)
    sdk.ps2000_run_block(handle, ctypes.c_int32(num),
                         ctypes.c_int16(5), ctypes.c_int16(1),
                         ctypes.byref(time_indisposed))
    while sdk.ps2000_ready(handle) == 0:
        time.sleep(0.01)

    buf_a = (ctypes.c_int16 * num)()
    buf_b = (ctypes.c_int16 * num)()
    overflow = ctypes.c_int16(0)
    got = sdk.ps2000_get_values(handle, ctypes.byref(buf_a),
                                ctypes.byref(buf_b), None, None,
                                ctypes.byref(overflow), ctypes.c_int32(num))
    a_mean = np.mean(np.array(buf_a[:got], dtype=np.int16))
    b_mean = np.mean(np.array(buf_b[:got], dtype=np.int16))
    print(f"  {label}: got={got}, interval={time_interval.value}ns, "
          f"max={max_samples.value}, A_raw={a_mean:.0f}, B_raw={b_mean:.0f}")

# Test 1: Single channel (only A enabled)
print("\n=== Single channel (A=5V, B=off) ===")
sdk.ps2000_set_channel(handle, 0, 1, 1, 7)  # A enabled, DC, 5V
sdk.ps2000_set_channel(handle, 1, 0, 1, 7)  # B disabled
capture("A=5V, B=off")

# Test 2: Dual channel same range
print("\n=== Dual channel (A=5V, B=5V) ===")
sdk.ps2000_set_channel(handle, 0, 1, 1, 7)  # A=5V
sdk.ps2000_set_channel(handle, 1, 1, 1, 7)  # B=5V
capture("A=5V, B=5V")

# Test 3: Dual channel different ranges
print("\n=== Dual channel (A=5V, B=500mV) ===")
sdk.ps2000_set_channel(handle, 0, 1, 1, 7)  # A=5V
sdk.ps2000_set_channel(handle, 1, 1, 1, 4)  # B=500mV
capture("A=5V, B=500mV")

# Test 4: Dual channel - all A ranges with B=5V
print("\n=== Sweep A ranges (B=5V fixed) ===")
for r, (name, mv) in sorted(RANGES.items()):
    sdk.ps2000_set_channel(handle, 0, 1, 1, r)  # A=range
    sdk.ps2000_set_channel(handle, 1, 1, 1, 7)  # B=5V
    capture(f"A={name}, B=5V")

# Test 5: Different timebases dual channel
print("\n=== Timebases dual (A=5V, B=5V) ===")
sdk.ps2000_set_channel(handle, 0, 1, 1, 7)
sdk.ps2000_set_channel(handle, 1, 1, 1, 7)
for tb in [0, 1, 2, 3, 5, 10]:
    num = 1000
    ti = ctypes.c_int32(0)
    tu = ctypes.c_int16(0)
    ms = ctypes.c_int32(0)
    sdk.ps2000_get_timebase(handle, ctypes.c_int16(tb),
                            ctypes.c_int32(num), ctypes.byref(ti),
                            ctypes.byref(tu), ctypes.c_int16(1),
                            ctypes.byref(ms))
    td = ctypes.c_int32(0)
    sdk.ps2000_run_block(handle, ctypes.c_int32(num),
                         ctypes.c_int16(tb), ctypes.c_int16(1),
                         ctypes.byref(td))
    while sdk.ps2000_ready(handle) == 0:
        time.sleep(0.01)
    ba = (ctypes.c_int16 * num)()
    bb = (ctypes.c_int16 * num)()
    ov = ctypes.c_int16(0)
    got = sdk.ps2000_get_values(handle, ctypes.byref(ba), ctypes.byref(bb),
                                None, None, ctypes.byref(ov), ctypes.c_int32(num))
    print(f"  tb={tb}: interval={ti.value}ns, max={ms.value}, got={got}")

# Test 6: Single channel timebases for comparison
print("\n=== Timebases single (A=5V, B=off) ===")
sdk.ps2000_set_channel(handle, 0, 1, 1, 7)
sdk.ps2000_set_channel(handle, 1, 0, 1, 7)
for tb in [0, 1, 2, 3, 5, 10]:
    num = 1000
    ti = ctypes.c_int32(0)
    tu = ctypes.c_int16(0)
    ms = ctypes.c_int32(0)
    sdk.ps2000_get_timebase(handle, ctypes.c_int16(tb),
                            ctypes.c_int32(num), ctypes.byref(ti),
                            ctypes.byref(tu), ctypes.c_int16(1),
                            ctypes.byref(ms))
    td = ctypes.c_int32(0)
    sdk.ps2000_run_block(handle, ctypes.c_int32(num),
                         ctypes.c_int16(tb), ctypes.c_int16(1),
                         ctypes.byref(td))
    while sdk.ps2000_ready(handle) == 0:
        time.sleep(0.01)
    ba = (ctypes.c_int16 * num)()
    ov = ctypes.c_int16(0)
    got = sdk.ps2000_get_values(handle, ctypes.byref(ba), None,
                                None, None, ctypes.byref(ov), ctypes.c_int32(num))
    print(f"  tb={tb}: interval={ti.value}ns, max={ms.value}, got={got}")

sdk.ps2000_close_unit(handle)
print("\nDone!")
