#!/usr/bin/env python3
"""Compare raw data from our driver with SDK to find the data processing bug."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import (PicoScopeFull, EP_CMD_OUT, EP_RESP_IN,
                                   EP_DATA_IN, Range, Channel, Coupling,
                                   RANGE_MV, ADC_MAX)
import numpy as np

scope = PicoScopeFull()
scope.open()

scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
scope.set_timebase(5, 1000)

# Do a capture but examine the raw buffer
print("\n=== Raw Buffer Analysis ===\n")

# Build and send capture commands
cmd1 = scope._build_capture_cmd1(1000, Channel.A, 5)
cmd2 = scope._build_capture_cmd2()
status_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')

scope._flush_all_buffers()
scope._send_cmd(cmd1, read_response=False)
scope._send_cmd(cmd2, read_response=False)
scope._send_cmd(status_cmd, read_response=False)

time.sleep(0.02)
resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=2000))
status = resp[0] if resp else 0

if status == 0x33:
    for _ in range(50):
        time.sleep(0.05)
        scope.device.write(EP_CMD_OUT, status_cmd, timeout=1000)
        resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=500))
        if resp and resp[0] == 0x3b:
            status = 0x3b
            break

print(f"Capture status: 0x{status:02x}")

if status == 0x3b:
    # Read data
    read_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]).ljust(64, b'\x00')
    scope.device.write(EP_CMD_OUT, read_cmd, timeout=1000)

    raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=5000))
    print(f"Raw buffer: {len(raw)} bytes")

    # Interpret as signed int16 (little-endian)
    samples_signed = np.frombuffer(raw, dtype='<i2')
    # Interpret as unsigned int16 (little-endian)
    samples_unsigned = np.frombuffer(raw, dtype='<u2')

    print(f"\nFirst 20 values (signed int16):  {samples_signed[:20]}")
    print(f"First 20 values (unsigned int16): {samples_unsigned[:20]}")

    # Header
    print(f"\nHeader (word 0): 0x{samples_unsigned[0]:04x}")

    # Skip header, analyze data
    data_s = samples_signed[1:]
    data_u = samples_unsigned[1:]

    # Find non-zero data
    nonzero_mask = data_u != 0
    nonzero_s = data_s[nonzero_mask]
    nonzero_u = data_u[nonzero_mask]

    print(f"\nNon-zero samples: {len(nonzero_s)} / {len(data_s)}")
    if len(nonzero_s) > 0:
        print(f"\nSigned stats:   mean={np.mean(nonzero_s):.1f}  std={np.std(nonzero_s):.1f}  min={np.min(nonzero_s)}  max={np.max(nonzero_s)}")
        print(f"Unsigned stats: mean={np.mean(nonzero_u):.1f}  std={np.std(nonzero_u):.1f}  min={np.min(nonzero_u)}  max={np.max(nonzero_u)}")

        # Check: are values centered near 0 (correct) or near 32768 (wrong offset)?
        if abs(np.mean(nonzero_s)) < 1000:
            print("\n>>> Data is centered near 0 (CORRECT)")
        elif np.mean(nonzero_u) > 30000:
            print(f"\n>>> Data is near +full-scale ({np.mean(nonzero_u):.0f})")
            print("    This suggests a DC offset or wrong data interpretation")
        else:
            print(f"\n>>> Data center: signed={np.mean(nonzero_s):.0f} unsigned={np.mean(nonzero_u):.0f}")

        # Show distribution
        print(f"\nFirst 10 non-zero (signed):  {nonzero_s[:10]}")
        print(f"Last 10 non-zero (signed):   {nonzero_s[-10:]}")

        # Check if data is actually unsigned with offset 32768 (mid-scale)
        recentered = nonzero_u.astype(np.int32) - 32768
        print(f"\nIf recentered (unsigned - 32768): mean={np.mean(recentered):.1f}  std={np.std(recentered):.1f}")

    # Also try interpreting first few bytes differently
    print(f"\nFirst 32 raw bytes: {raw[:32].hex()}")
    print(f"Last 32 raw bytes:  {raw[-32:].hex()}")

    # Check where data starts/ends (zero gap analysis)
    zero_run_start = -1
    zero_run_end = -1
    for i in range(1, len(data_u)):
        if data_u[i] == 0 and data_u[i-1] != 0:
            zero_run_start = i
        elif data_u[i] != 0 and data_u[i-1] == 0 and zero_run_start >= 0:
            zero_run_end = i
            break
    print(f"\nZero gap: starts at {zero_run_start}, ends at {zero_run_end}")

scope.close()
