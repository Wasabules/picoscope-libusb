#!/usr/bin/env python3
"""Hardware test: verify that range setting affects raw ADC values.

With the correct byte 50-52 encoding, changing the range should change
the PGA gain, resulting in different mV values for the same input voltage.

Expected behavior: higher sensitivity (smaller range) → larger mV values
for the same physical voltage (because ADC has finer resolution).
"""
import sys
import os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from picoscope_libusb_full import PicoScopeFull, Channel, Coupling, Range

scope = PicoScopeFull()
scope.open()

print("\n=== Hardware Range Test ===")
print("Input: floating (should see noise/DC offset changing with gain)")
print()

# Test all supported ranges on channel A
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

print(f"{'Range':>8s}  {'Mean mV':>10s}  {'Std mV':>8s}  {'Min mV':>10s}  {'Max mV':>10s}  {'N':>5s}")
print("-" * 60)

results = []
for rng, name in ranges:
    scope.set_channel(Channel.A, True, Coupling.DC, rng)
    scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
    scope.set_timebase(5, 1000)

    data = scope.capture_block(1000, Channel.A, timeout_ms=5000)
    if data is not None and 'A' in data:
        samples = data['A']
        n = len(samples)
        mean = np.mean(samples)
        std = np.std(samples)
        mn = np.min(samples)
        mx = np.max(samples)
        print(f"{name:>8s}  {mean:>10.2f}  {std:>8.2f}  {mn:>10.2f}  {mx:>10.2f}  {n:>5d}")
        results.append((name, mean, std))
    else:
        print(f"{name:>8s}  CAPTURE FAILED")
        results.append((name, 0, 0))

# Check if range actually affects values
print()
if len(results) >= 2:
    means = [abs(r[1]) for r in results if r[1] != 0]
    if len(means) >= 2:
        spread = max(means) - min(means)
        if spread > 5:
            print(f"Range WORKS! Mean mV spread across ranges: {spread:.1f} mV")
        else:
            print(f"Range may not be working. Mean mV spread only {spread:.1f} mV")

    # More meaningful: at sensitive ranges, std should be larger (amplified noise)
    stds = [(r[0], r[2]) for r in results if r[2] != 0]
    if stds:
        print(f"Noise check: {stds[0][0]} std={stds[0][1]:.2f}, {stds[-1][0]} std={stds[-1][1]:.2f}")

scope.close()
