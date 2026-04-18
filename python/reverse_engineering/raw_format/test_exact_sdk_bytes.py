#!/usr/bin/env python3
"""Use EXACT SDK bytes for capture commands after our init.

If PGA works: our cmd encoding was wrong.
If PGA still fails: our init is missing something the SDK does.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import PicoScopeFull, EP_CMD_OUT, EP_RESP_IN, EP_DATA_IN
import numpy as np
import time

scope = PicoScopeFull()
scope.open()

def flush():
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

def raw_capture_sdk_bytes(label, gain_bytes):
    """Capture using EXACT SDK compound commands, only changing 3 gain bytes."""
    flush()

    b50, b51, b52 = gain_bytes

    # EXACT SDK cmd1 (packet [0047]) with variable gain bytes
    cmd1 = bytes([
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0xc8,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x57,
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x20,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, b50, b51, b52,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
    ]).ljust(64, b'\x00')

    # EXACT SDK cmd2 (packet [0048]) - constant
    cmd2 = bytes([
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x02,
        0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    ]).ljust(64, b'\x00')

    # Status poll
    status_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')

    # Send all 3 without reading ACKs (matching SDK async pattern)
    scope.device.write(EP_CMD_OUT, cmd1, timeout=1000)
    scope.device.write(EP_CMD_OUT, cmd2, timeout=1000)
    scope.device.write(EP_CMD_OUT, status_cmd, timeout=1000)

    time.sleep(0.02)
    try:
        resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=2000))
        status = resp[0] if resp else 0
    except:
        status = 0

    if status == 0x33:
        # Poll for ready
        for _ in range(50):
            time.sleep(0.05)
            scope.device.write(EP_CMD_OUT, status_cmd, timeout=1000)
            try:
                resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=500))
                if resp and resp[0] == 0x3b:
                    status = 0x3b
                    break
                elif resp and resp[0] == 0x7b:
                    status = 0x7b
                    break
            except:
                pass

    if status != 0x3b:
        print(f"  {label}: FAILED (status=0x{status:02x})")
        return None

    # Data read (packet [0051])
    read_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]).ljust(64, b'\x00')
    scope.device.write(EP_CMD_OUT, read_cmd, timeout=1000)

    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=5000))
        samples = np.frombuffer(raw, dtype='<i2')
        if len(samples) > 1:
            data = samples[1:]  # skip header
            non_zero = data[data != 0]
            if len(non_zero) > 0:
                mean = np.mean(non_zero)
                std = np.std(non_zero)
                print(f"  {label}: raw_mean={mean:.1f} raw_std={std:.1f} n={len(non_zero)}")
                return mean
    except Exception as e:
        print(f"  {label}: data error: {e}")

    print(f"  {label}: no data")
    return None

# ========================================
# Test with EXACT SDK gain bytes for each range
# ========================================
print("\n=== EXACT SDK Bytes - PGA Test ===\n")

# From SDK USB trace (capture_sdk_all_ranges.py)
sdk_ranges = [
    ("50mV",  (0x23, 0xd4, 0xc0)),
    ("100mV", (0x23, 0xd4, 0xe0)),
    ("200mV", (0x23, 0xd4, 0x50)),
    ("500mV", (0x23, 0xd4, 0x60)),
    ("1V",    (0x23, 0xd4, 0x20)),
    ("2V",    (0x23, 0xc4, 0xe0)),
    ("5V",    (0x23, 0xc4, 0x40)),
    ("10V",   (0x23, 0xc4, 0x60)),
    ("20V",   (0x23, 0xc4, 0x20)),
]

means = []
for name, gain_bytes in sdk_ranges:
    m = raw_capture_sdk_bytes(name, gain_bytes)
    means.append((name, m))

print()
valid = [m for _, m in means if m is not None]
if len(valid) >= 2:
    spread = max(valid) - min(valid)
    if spread > 500:
        print(f"PGA WORKING! Raw ADC spread = {spread:.0f}")
    else:
        print(f"PGA NOT working. Raw ADC spread = {spread:.0f}")
        print("Issue is in INIT sequence, not capture commands")

scope.close()
