#!/usr/bin/env python3
"""Hardware test v2: skip set_channel USB commands between captures.

The SDK only sends 85 21 once during init. For range changes, it only
modifies bytes 50-52 in the capture compound command. Test if our
set_channel() USB command is interfering with the PGA setting.
"""
import sys
import os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from picoscope_libusb_full import (PicoScopeFull, Channel, Coupling, Range,
                                   RANGE_MV, _RANGE_HW_GAIN)

scope = PicoScopeFull()
scope.open()

# Initial setup: both channels 5V DC (matching SDK init)
# This sends the 85 21 compound once
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
scope.set_timebase(5, 1000)

print("\n=== Hardware Range Test v2 (no set_channel between captures) ===")
print("Only changing internal state + bytes 50-52 in capture command")
print()

ranges = [
    (Range.R_50MV, "50mV"),
    (Range.R_100MV, "100mV"),
    (Range.R_200MV, "200mV"),
    (Range.R_500MV, "500mV"),
    (Range.R_1V, "1V"),
    (Range.R_2V, "2V"),
    (Range.R_5V, "5V"),
    (Range.R_10V, "10V"),
    (Range.R_20V, "20V"),
]

print(f"{'Range':>8s}  {'RawMean':>10s}  {'RawStd':>8s}  {'RawMin':>10s}  {'RawMax':>10s}  {'B50':>4s} {'B51':>4s} {'B52':>4s}")
print("-" * 75)

results = []
for rng, name in ranges:
    # Update internal state WITHOUT sending USB commands
    scope._channels[Channel.A] = {
        'enabled': True, 'coupling': Coupling.DC,
        'range': rng, 'range_mv': RANGE_MV[rng]
    }
    scope._range_a = rng

    # Check what bytes will be sent
    b50, b51, b52 = scope._encode_hw_gain_bytes()

    data = scope.capture_block(1000, Channel.A, timeout_ms=5000)
    if data is not None and 'A' in data:
        # Get raw ADC values (before mV scaling)
        # capture_block scales: data * (range_mv / ADC_MAX)
        # So raw = data_mv * ADC_MAX / range_mv
        samples_mv = data['A']
        raw = samples_mv * 32767 / RANGE_MV[rng]
        mean = np.mean(raw)
        std = np.std(raw)
        mn = np.min(raw)
        mx = np.max(raw)
        print(f"{name:>8s}  {mean:>10.1f}  {std:>8.1f}  {mn:>10.1f}  {mx:>10.1f}  0x{b50:02x} 0x{b51:02x} 0x{b52:02x}")
        results.append((name, mean))
    else:
        print(f"{name:>8s}  CAPTURE FAILED                              0x{b50:02x} 0x{b51:02x} 0x{b52:02x}")
        results.append((name, 0))

print()
if len(results) >= 2:
    means = [r[1] for r in results if r[1] != 0]
    if len(means) >= 2:
        spread = max(means) - min(means)
        if spread > 100:
            print(f"PGA WORKING! Raw ADC spread: {spread:.0f}")
        else:
            print(f"PGA NOT working. Raw ADC spread: {spread:.0f} (same raw ADC regardless of range)")

scope.close()
