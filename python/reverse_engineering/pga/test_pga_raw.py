#!/usr/bin/env python3
"""PGA test using RAW ADC values (no software scaling).

If PGA works, raw ADC values should CHANGE between ranges.
If PGA doesn't work, raw ADC values stay ~32000 regardless of range.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import PicoScopeFull, Range, Channel, Coupling
import numpy as np

scope = PicoScopeFull()
scope.open()

ranges = [
    (Range.R_50MV, "50mV"),
    (Range.R_500MV, "500mV"),
    (Range.R_5V, "5V"),
    (Range.R_20V, "20V"),
]

print(f"\n{'Range':>8s}  {'Raw ADC Mean':>12s}  {'Raw Std':>8s}  {'Scaled mV':>10s}  {'Expected if PGA off':>20s}")
print("-" * 70)

raw_means = []
for range_val, name in ranges:
    scope.set_channel(Channel.A, True, Coupling.DC, range_val)
    scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
    scope.set_timebase(5, 1000)

    result = scope.capture_block(1000)
    if result and 'A' in result and result['A'] is not None:
        # Get raw ADC values (reverse the scaling)
        from picoscope_libusb_full import RANGE_MV, ADC_MAX
        range_mv = RANGE_MV.get(range_val, 5000)
        scaled = result['A']
        # Reverse: scaled = raw * (range_mv / ADC_MAX), so raw = scaled * ADC_MAX / range_mv
        raw = scaled * ADC_MAX / range_mv
        raw_mean = np.mean(raw)
        raw_std = np.std(raw)
        scaled_mean = np.mean(scaled)
        expected = f"~{raw_mean * range_mv / ADC_MAX:.0f}mV"
        print(f"{name:>8s}  {raw_mean:>12.1f}  {raw_std:>8.1f}  {scaled_mean:>10.1f}  {expected:>20s}")
        raw_means.append(raw_mean)
    else:
        print(f"{name:>8s}  FAILED")

if len(raw_means) >= 2:
    spread = max(raw_means) - min(raw_means)
    print(f"\nRaw ADC spread: {spread:.0f}")
    if spread > 500:
        print("PGA IS WORKING! (raw ADC values differ between ranges)")
    else:
        print("PGA NOT working (raw ADC values are constant across ranges)")
        print("The different mV values are just software scaling, not hardware gain changes")

scope.close()
