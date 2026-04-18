#!/usr/bin/env python3
"""Debug PGA: print exact bytes sent for each range and compare with SDK."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import PicoScopeFull, Range, Channel, Coupling, _RANGE_HW_GAIN
import numpy as np

# SDK reference bytes for each range (from USB trace)
SDK_BYTES = {
    Range.R_50MV:  (0x23, 0xd4, 0xc0),
    Range.R_100MV: (0x23, 0xd4, 0xe0),
    Range.R_200MV: (0x23, 0xd4, 0x50),
    Range.R_500MV: (0x23, 0xd4, 0x60),
    Range.R_1V:    (0x23, 0xd4, 0x20),
    Range.R_2V:    (0x23, 0xc4, 0xe0),
    Range.R_5V:    (0x23, 0xc4, 0x40),
    Range.R_10V:   (0x23, 0xc4, 0x60),
    Range.R_20V:   (0x23, 0xc4, 0x20),
}

scope = PicoScopeFull()
scope.open()

print("\n=== Gain Byte Encoding Debug ===\n")
print(f"{'Range':>8s}  {'Our Bytes':>12s}  {'SDK Bytes':>12s}  {'Match':>6s}  {'HW Gain (bank,sel,200)':>24s}")
print("-" * 80)

all_match = True
for range_val in [Range.R_50MV, Range.R_100MV, Range.R_200MV, Range.R_500MV,
                  Range.R_1V, Range.R_2V, Range.R_5V, Range.R_10V, Range.R_20V]:
    # Set channel A to this range, B disabled
    scope.set_channel(Channel.A, True, Coupling.DC, range_val)
    scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
    scope._current_timebase = 3  # Match SDK test (timebase=3)

    b50, b51, b52 = scope._encode_hw_gain_bytes(3)
    sdk = SDK_BYTES[range_val]
    match = (b50, b51, b52) == sdk
    if not match:
        all_match = False
    hw = _RANGE_HW_GAIN.get(range_val, "N/A")

    name = range_val.name.replace('R_', '')
    print(f"{name:>8s}  {b50:02x} {b51:02x} {b52:02x}     {sdk[0]:02x} {sdk[1]:02x} {sdk[2]:02x}     {'OK' if match else 'DIFF':>6s}  {str(hw):>24s}")

print()
if all_match:
    print("ALL bytes match SDK!")
else:
    print("MISMATCH detected - fixing byte encoding may fix PGA")

# Now do a single capture and print the raw cmd1 bytes
print("\n=== Capture Command Bytes ===\n")
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_50MV)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
scope._current_timebase = 5
cmd1 = scope._build_capture_cmd1(1000, Channel.A, 5)
print(f"cmd1 ({len(cmd1)} bytes):")
print(" ".join(f"{b:02x}" for b in cmd1))
print(f"\nBytes 49-51 (gain): {cmd1[49]:02x} {cmd1[50]:02x} {cmd1[51]:02x}")

# Compare with actual capture
print("\n=== Quick PGA Test (50mV vs 20V) ===\n")
for range_val, name in [(Range.R_50MV, "50mV"), (Range.R_20V, "20V")]:
    scope.set_channel(Channel.A, True, Coupling.DC, range_val)
    scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
    scope.set_timebase(5, 1000)
    result = scope.capture_block(1000)
    if result and 'A' in result and result['A'] is not None:
        from picoscope_libusb_full import RANGE_MV, ADC_MAX
        range_mv = RANGE_MV.get(range_val, 5000)
        scaled = result['A']
        raw = scaled * ADC_MAX / range_mv
        print(f"  {name:>5s}: raw_mean={np.mean(raw):.0f}, scaled_mean={np.mean(scaled):.1f} mV")
    else:
        print(f"  {name:>5s}: FAILED")

scope.close()
