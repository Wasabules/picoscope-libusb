#!/usr/bin/env python3
"""Hardware test v3: send EXACT SDK bytes, only changing bytes 50-52.

This test sends the identical compound commands as the SDK, to verify
that the PGA responds when the command format matches exactly.
"""
import sys
import os
import time
import struct
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from picoscope_libusb_full import PicoScopeFull, Channel, Coupling, Range, RANGE_MV
import usb.core

scope = PicoScopeFull()
scope.open()

EP_CMD_OUT = 0x01
EP_RESP_IN = 0x81
EP_DATA_IN = 0x82

def flush():
    """Flush pending USB responses."""
    for _ in range(5):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=50)
        except:
            break
    for _ in range(3):
        try:
            scope.device.read(EP_DATA_IN, 16384, timeout=50)
        except:
            break

def raw_capture(byte50, byte51, byte52, label):
    """Send exact SDK compound commands with only bytes 50-52 changed."""
    flush()

    # Exact SDK compound cmd1 (from capture_sdk_all_ranges.py trace)
    # Only bytes 50, 51, 52 are parameterized
    cmd1 = bytes([
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0xc8,  # Trigger buffer (SDK format)
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x57,  # Channel (SDK constant)
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x20,  # Buffer
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,  # Get data setup
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,  # Timebase config
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, byte50, byte51, byte52,  # Run block
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00  # Status
    ])

    # Exact SDK compound cmd2
    cmd2 = bytes([
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x02,
        0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00
    ])

    scope.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)
    scope.device.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    # Flush config responses
    for _ in range(3):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=100)
        except:
            break

    # Poll status
    status = scope._poll_status(timeout_ms=5000)
    if status != 0x3b:
        print(f"  {label}: status=0x{status:02x} (not ready)")
        return None

    # Trigger
    trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
    scope.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    # Read data
    raw = scope._read_data(16384)
    if len(raw) < 4:
        time.sleep(0.2)
        raw = scope._read_data(16384)

    # Parse raw int16 samples
    all_samples = np.frombuffer(raw, dtype='<i2')
    if len(all_samples) > 1:
        buf = all_samples[1:]  # Skip header
        non_zero = buf[buf != 0]
        if len(non_zero) > 0:
            return non_zero
    return None


print("\n=== Hardware Range Test v3 (EXACT SDK bytes) ===")
print("Only bytes 50-52 change. All other cmd1/cmd2 bytes match SDK exactly.")
print()

# SDK byte 50-52 values from trace (Phase 1: A varies, B=5V DC)
tests = [
    ("A=50mV, B=5V",  0x23, 0xd4, 0xc0, Range.R_50MV),
    ("A=100mV, B=5V", 0x23, 0xd4, 0xe0, Range.R_100MV),
    ("A=200mV, B=5V", 0x23, 0xd4, 0x50, Range.R_200MV),
    ("A=500mV, B=5V", 0x23, 0xd4, 0x60, Range.R_500MV),
    ("A=1V, B=5V",    0x23, 0xd4, 0x20, Range.R_1V),
    ("A=2V, B=5V",    0x23, 0xc4, 0xe0, Range.R_2V),
    ("A=5V, B=5V",    0x23, 0xc4, 0x40, Range.R_5V),
    ("A=10V, B=5V",   0x23, 0xc4, 0x60, Range.R_10V),
    ("A=20V, B=5V",   0x23, 0xc4, 0x20, Range.R_20V),
]

print(f"{'Config':>16s}  {'RawMean':>10s}  {'RawStd':>8s}  {'N':>5s}")
print("-" * 50)

results = []
for label, b50, b51, b52, rng in tests:
    data = raw_capture(b50, b51, b52, label)
    if data is not None:
        mean = np.mean(data)
        std = np.std(data)
        n = len(data)
        print(f"{label:>16s}  {mean:>10.1f}  {std:>8.1f}  {n:>5d}")
        results.append((label, mean))
    else:
        print(f"{label:>16s}  FAILED")
        results.append((label, 0))

print()
if len(results) >= 2:
    means = [r[1] for r in results if r[1] != 0]
    if len(means) >= 2:
        spread = max(means) - min(means)
        if spread > 100:
            print(f"PGA WORKS! Raw ADC spread: {spread:.0f}")
        else:
            print(f"PGA NOT working. Raw ADC spread: {spread:.0f}")

scope.close()
