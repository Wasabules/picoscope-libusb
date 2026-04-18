#!/usr/bin/env python3
"""Quick PGA test - check if range changes affect raw ADC values.

Uses our driver with threaded waveform upload (channel A OK, B fails).
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import PicoScopeFull, Range, Channel, Coupling
import numpy as np

scope = PicoScopeFull()
scope.open()

# Test different ranges
ranges = [
    (Range.R_50MV, "50mV"),
    (Range.R_100MV, "100mV"),
    (Range.R_500MV, "500mV"),
    (Range.R_1V, "1V"),
    (Range.R_5V, "5V"),
    (Range.R_20V, "20V"),
]

print(f"\n{'Range':>8s}  {'Raw Mean':>10s}  {'Raw Std':>8s}  {'mV Mean':>10s}  {'N':>5s}")
print("-" * 50)

raw_means = []
for range_val, name in ranges:
    scope.set_channel(Channel.A, True, Coupling.DC, range_val)
    scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
    scope.set_timebase(5, 1000)

    result = scope.capture_block(1000)
    if result and 'A' in result and result['A'] is not None:
        data = result['A']
        # Get raw ADC value from the internal buffer
        raw_mean = np.mean(data)
        raw_std = np.std(data)
        print(f"{name:>8s}  {raw_mean:>10.1f}  {raw_std:>8.1f}  {raw_mean:>10.1f}  {len(data):>5d}")
        raw_means.append(raw_mean)
    else:
        print(f"{name:>8s}  FAILED")

if len(raw_means) >= 2:
    spread = max(raw_means) - min(raw_means)
    if spread > 100:
        print(f"\nPGA WORKING! Spread = {spread:.0f} mV")
    else:
        print(f"\nPGA NOT working. Spread = {spread:.0f} mV")

scope.close()
