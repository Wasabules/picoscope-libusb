#!/usr/bin/env python3
"""Validation test: verify 8-bit ADC fix across sample counts and timebases."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from picoscope_libusb_full import (PicoScopeFull, Range, Channel, Coupling,
                                   ADC_HALF_RANGE, RANGE_MV)
import numpy as np

scope = PicoScopeFull()
scope.open()

scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)

# Test 1: Multiple sample counts at fixed timebase
print("\n=== Test 1: Sample counts (timebase=5) ===\n")
scope.set_timebase(5, 1000)
for n in [100, 500, 1000, 2000, 4000]:
    scope.set_timebase(5, n)
    result = scope.capture_block(n)
    if result and 'A' in result:
        s = result['A']
        got = len(s)
        ok = "OK" if abs(got - n) <= 1 else f"MISMATCH (expected {n})"
        print(f"  n={n:5d}: got {got:5d} samples, mean={np.mean(s):+7.1f}mV, "
              f"std={np.std(s):5.1f}mV  [{ok}]")
    else:
        print(f"  n={n:5d}: FAILED")
    time.sleep(0.1)

# Test 2: Multiple timebases at fixed sample count
print("\n=== Test 2: Timebases (n=1000) ===\n")
for tb in [0, 1, 2, 3, 5, 10, 15, 20, 23]:
    scope.set_timebase(tb, 1000)
    result = scope.capture_block(1000)
    if result and 'A' in result:
        s = result['A']
        got = len(s)
        info = scope.get_timebase_info(tb)
        interval_ns = info['sample_interval_ns']
        print(f"  tb={tb:2d} ({interval_ns:8.0f}ns): {got:5d} samples, "
              f"mean={np.mean(s):+7.1f}mV, std={np.std(s):5.1f}mV")
    else:
        print(f"  tb={tb:2d}: FAILED")
    time.sleep(0.1)

# Test 3: Voltage ranges (PGA test with floating input)
print("\n=== Test 3: Voltage ranges (floating input) ===\n")
scope.set_timebase(5, 1000)
for range_val, name in [
    (Range.R_50MV, "50mV"), (Range.R_100MV, "100mV"),
    (Range.R_200MV, "200mV"), (Range.R_500MV, "500mV"),
    (Range.R_1V, "1V"), (Range.R_2V, "2V"),
    (Range.R_5V, "5V"), (Range.R_10V, "10V"), (Range.R_20V, "20V"),
]:
    scope.set_channel(Channel.A, True, Coupling.DC, range_val)
    scope.set_timebase(5, 1000)
    result = scope.capture_block(1000)
    if result and 'A' in result:
        s = result['A']
        range_mv = RANGE_MV.get(range_val, 5000)
        # With floating input, raw ADC should be near mid-scale (0 signed)
        # Mean should be small relative to the range
        pct_of_range = abs(np.mean(s)) / range_mv * 100
        print(f"  {name:>5s} ({range_mv:5d}mV): mean={np.mean(s):+8.1f}mV, "
              f"std={np.std(s):6.1f}mV, {pct_of_range:.1f}% of range")
    else:
        print(f"  {name:>5s}: FAILED")
    time.sleep(0.1)

# Test 4: Consecutive captures (stability)
print("\n=== Test 4: 10 consecutive captures ===\n")
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_timebase(5, 1000)
means = []
for i in range(10):
    result = scope.capture_block(1000)
    if result and 'A' in result:
        s = result['A']
        means.append(np.mean(s))
        print(f"  #{i+1:2d}: {len(s):5d} samples, mean={np.mean(s):+7.1f}mV")
    else:
        print(f"  #{i+1:2d}: FAILED")
    time.sleep(0.05)

if means:
    print(f"\n  Overall: {len(means)}/10 succeeded, "
          f"mean of means={np.mean(means):+.1f}mV, "
          f"std of means={np.std(means):.1f}mV")

scope.close()
print("\n=== All validation tests complete ===")
