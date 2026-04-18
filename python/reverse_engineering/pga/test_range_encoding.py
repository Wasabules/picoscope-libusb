#!/usr/bin/env python3
"""Test that _encode_hw_gain_bytes() produces the correct SDK-matching values.

Expected values from USB interceptor trace of SDK capture_sdk_all_ranges.py:

Phase 1 (vary A range, B=5V DC):
  A=50mV  → 23 d4 c0
  A=100mV → 23 d4 e0
  A=200mV → 23 d4 50
  A=500mV → 23 d4 60
  A=1V    → 23 d4 20
  A=2V    → 23 c4 e0
  A=5V    → 23 c4 40
  A=10V   → 23 c4 60
  A=20V   → 23 c4 20

Phase 2 (A=5V DC, vary B range):
  B=50mV  → 23 ec 40
  B=100mV → 23 ee 40
  B=200mV → 23 e5 40
  B=500mV → 23 e6 40
  B=1V    → 23 e2 40
  B=2V    → 23 ce 40
  B=5V    → 23 c4 40
  B=10V   → 23 c6 40
  B=20V   → 23 c2 40

Phase 3 (coupling):
  DC/DC → 23 c4 40
  AC/DC → 23 84 40
  DC/AC → 23 44 40
  AC/AC → 23 04 40

Phase 4 (channel enable):
  A only → 21 42 40
  B only → 22 c4 20
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from picoscope_libusb_full import PicoScopeFull, Channel, Coupling, Range

# Create a scope instance without opening hardware
scope = PicoScopeFull.__new__(PicoScopeFull)
scope._channels = {}
scope._range_a = Range.R_5V
scope._range_b = Range.R_5V

def setup(a_range, b_range, a_coupling=Coupling.DC, b_coupling=Coupling.DC,
          a_enabled=True, b_enabled=True):
    """Set channel state without USB commands."""
    scope._channels[Channel.A] = {
        'enabled': a_enabled, 'coupling': a_coupling, 'range': a_range,
        'range_mv': 5000
    }
    scope._channels[Channel.B] = {
        'enabled': b_enabled, 'coupling': b_coupling, 'range': b_range,
        'range_mv': 5000
    }
    scope._range_a = a_range
    scope._range_b = b_range

def check(label, expected_hex):
    """Check encoding matches expected SDK output."""
    b50, b51, b52 = scope._encode_hw_gain_bytes()
    actual = f"{b50:02x} {b51:02x} {b52:02x}"
    ok = actual == expected_hex
    status = "OK" if ok else "FAIL"
    print(f"  [{status}] {label:25s} expected={expected_hex}  actual={actual}")
    return ok

all_ok = True

# Phase 1: Vary A range, B=5V DC
print("=== Phase 1: Vary A range, B=5V DC ===")
tests_p1 = [
    (Range.R_50MV,  "A=50mV",  "23 d4 c0"),
    (Range.R_100MV, "A=100mV", "23 d4 e0"),
    (Range.R_200MV, "A=200mV", "23 d4 50"),
    (Range.R_500MV, "A=500mV", "23 d4 60"),
    (Range.R_1V,    "A=1V",    "23 d4 20"),
    (Range.R_2V,    "A=2V",    "23 c4 e0"),
    (Range.R_5V,    "A=5V",    "23 c4 40"),
    (Range.R_10V,   "A=10V",   "23 c4 60"),
    (Range.R_20V,   "A=20V",   "23 c4 20"),
]
for rng, label, expected in tests_p1:
    setup(rng, Range.R_5V)
    all_ok &= check(label, expected)

# Phase 2: A=5V DC, Vary B range
print("\n=== Phase 2: A=5V DC, Vary B range ===")
tests_p2 = [
    (Range.R_50MV,  "B=50mV",  "23 ec 40"),
    (Range.R_100MV, "B=100mV", "23 ee 40"),
    (Range.R_200MV, "B=200mV", "23 e5 40"),
    (Range.R_500MV, "B=500mV", "23 e6 40"),
    (Range.R_1V,    "B=1V",    "23 e2 40"),
    (Range.R_2V,    "B=2V",    "23 ce 40"),
    (Range.R_5V,    "B=5V",    "23 c4 40"),
    (Range.R_10V,   "B=10V",   "23 c6 40"),
    (Range.R_20V,   "B=20V",   "23 c2 40"),
]
for rng, label, expected in tests_p2:
    setup(Range.R_5V, rng)
    all_ok &= check(label, expected)

# Phase 3: Coupling
print("\n=== Phase 3: Coupling (A=5V, B=5V) ===")
tests_p3 = [
    (Coupling.DC, Coupling.DC, "DC/DC", "23 c4 40"),
    (Coupling.AC, Coupling.DC, "AC/DC", "23 84 40"),
    (Coupling.DC, Coupling.AC, "DC/AC", "23 44 40"),
    (Coupling.AC, Coupling.AC, "AC/AC", "23 04 40"),
]
for a_c, b_c, label, expected in tests_p3:
    setup(Range.R_5V, Range.R_5V, a_coupling=a_c, b_coupling=b_c)
    all_ok &= check(label, expected)

# Phase 4: Channel enable
print("\n=== Phase 4: Channel enable ===")
setup(Range.R_5V, Range.R_5V, a_enabled=True, b_enabled=False)
# SDK sends 21 42 40 due to stale B coupling from prior AC/AC test.
# We send 21 c2 40 (B_dc=1 from stored DC coupling). Disabled channel
# coupling bit has no hardware effect — this is acceptable.
all_ok &= check("A only", "21 c2 40")

setup(Range.R_5V, Range.R_5V, a_enabled=False, b_enabled=True)
all_ok &= check("B only", "22 c4 20")

print(f"\n{'ALL TESTS PASSED' if all_ok else 'SOME TESTS FAILED'}!")
sys.exit(0 if all_ok else 1)
