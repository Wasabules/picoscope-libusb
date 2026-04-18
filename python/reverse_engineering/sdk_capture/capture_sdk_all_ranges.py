#!/usr/bin/env python3
"""Capture SDK compound cmd1 bytes 50-52 for ALL range combinations.

Each capture forces the SDK to send a compound command with the current
channel/range/coupling settings encoded in the run_block sub-command.
"""
import ctypes
from ctypes import c_int16, c_int32, POINTER, byref
import time

lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")

print("Opening PicoScope via SDK...", flush=True)
handle = lib.ps2000_open_unit()
if handle <= 0:
    print("FAILED!")
    exit(1)
time.sleep(1.0)
print(f"Handle: {handle}\n", flush=True)


def do_capture(handle, ch_a_range, ch_b_range, ch_a_coupling=1, ch_b_coupling=1,
               ch_a_enabled=1, ch_b_enabled=1):
    """Set channels and do a capture."""
    lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(ch_a_enabled),
                           c_int16(ch_a_coupling), c_int16(ch_a_range))
    lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(ch_b_enabled),
                           c_int16(ch_b_coupling), c_int16(ch_b_range))

    time_indisposed = c_int32(0)
    lib.ps2000_run_block(c_int16(handle), c_int16(200), c_int16(5),
                         c_int16(1), byref(time_indisposed))

    for _ in range(50):
        time.sleep(0.05)
        if lib.ps2000_ready(c_int16(handle)):
            break

    buf_a = (c_int16 * 200)()
    buf_b = (c_int16 * 200)()
    overflow = c_int16(0)
    lib.ps2000_get_values(c_int16(handle),
                          ctypes.cast(buf_a, POINTER(c_int16)),
                          ctypes.cast(buf_b, POINTER(c_int16)),
                          None, None, byref(overflow), c_int32(200))
    time.sleep(0.1)


# Phase 1: All A ranges with B=5V DC (both enabled)
print("=== Phase 1: Vary A range, B=5V DC ===", flush=True)
ranges = [(2, "50mV"), (3, "100mV"), (4, "200mV"), (5, "500mV"),
          (6, "1V"), (7, "2V"), (8, "5V"), (9, "10V"), (10, "20V")]
for rv, name in ranges:
    print(f"A={name}, B=5V", flush=True)
    do_capture(handle, rv, 8)

# Phase 2: All B ranges with A=5V DC (both enabled)
print("\n=== Phase 2: A=5V DC, Vary B range ===", flush=True)
for rv, name in ranges:
    print(f"A=5V, B={name}", flush=True)
    do_capture(handle, 8, rv)

# Phase 3: Coupling variations
print("\n=== Phase 3: Coupling ===", flush=True)
print("A=5V DC, B=5V DC", flush=True)
do_capture(handle, 8, 8, ch_a_coupling=1, ch_b_coupling=1)
print("A=5V AC, B=5V DC", flush=True)
do_capture(handle, 8, 8, ch_a_coupling=0, ch_b_coupling=1)
print("A=5V DC, B=5V AC", flush=True)
do_capture(handle, 8, 8, ch_a_coupling=1, ch_b_coupling=0)
print("A=5V AC, B=5V AC", flush=True)
do_capture(handle, 8, 8, ch_a_coupling=0, ch_b_coupling=0)

# Phase 4: Single channel modes
print("\n=== Phase 4: Channel enable ===", flush=True)
print("A=5V only", flush=True)
do_capture(handle, 8, 8, ch_a_enabled=1, ch_b_enabled=0)
print("B=5V only", flush=True)
do_capture(handle, 8, 8, ch_a_enabled=0, ch_b_enabled=1)

print("\nClosing...", flush=True)
lib.ps2000_close_unit(c_int16(handle))
print("Done!", flush=True)
