#!/usr/bin/env python3
"""Test dual-channel layout on PicoScope 2204A.

Strategy: Compare raw ADC buffers from:
1. Only Channel A enabled (B disabled)
2. Only Channel B enabled (A disabled)
3. Both channels enabled

Even with floating inputs, analog front-end differences between channels
(offset, noise floor) may reveal the data layout.
"""
import sys
import os
import time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from picoscope_libusb_full import (
    PicoScopeFull, Channel, Coupling, Range, EP_CMD_OUT, EP_RESP_IN, EP_DATA_IN
)
import usb.core


def raw_capture(scope, n_samples=4000, timebase=5):
    """Do a capture and return the raw int16 buffer (no mV conversion)."""
    scope._flush_all_buffers()

    config_cmd1 = scope._build_capture_cmd1(n_samples, Channel.A, timebase)
    scope.device.write(EP_CMD_OUT, config_cmd1.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)

    config_cmd2 = scope._build_capture_cmd2()
    scope.device.write(EP_CMD_OUT, config_cmd2.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    # Flush config responses
    for _ in range(3):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=100)
        except:
            break

    status = scope._poll_status(timeout_ms=3000)
    if status != 0x3b:
        print(f"  WARNING: status=0x{status:02x}")
        return None

    # Trigger
    trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
    scope.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    # Read data - try 32KB first, then 16KB
    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 32768, timeout=5000))
    except usb.core.USBTimeoutError:
        raw = b''

    # Try to read more
    extra = b''
    try:
        extra = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=500))
    except usb.core.USBTimeoutError:
        pass

    total_raw = raw + extra
    return total_raw


def analyze_buffer(raw, label):
    """Analyze a raw buffer and print statistics."""
    if raw is None or len(raw) < 4:
        print(f"  {label}: No data")
        return None

    samples = np.frombuffer(raw, dtype='<i2')
    print(f"\n  {label}:")
    print(f"    Total bytes: {len(raw)}, Total int16 samples: {len(samples)}")
    print(f"    Header (index 0): 0x{samples[0]:04X} ({samples[0]})")

    # Skip header
    buf = samples[1:]
    non_zero = np.nonzero(buf)[0]
    n_nz = len(non_zero)
    print(f"    Non-zero samples: {n_nz} / {len(buf)}")

    if n_nz > 0:
        nz_vals = buf[non_zero]
        print(f"    Non-zero range: min={nz_vals.min()}, max={nz_vals.max()}, "
              f"mean={nz_vals.mean():.1f}, std={nz_vals.std():.2f}")

        # Show first and last few non-zero values
        print(f"    First 5 non-zero values (at idx {non_zero[:5]}): {nz_vals[:5]}")
        print(f"    Last 5 non-zero values (at idx {non_zero[-5:]}): {nz_vals[-5:]}")

        # Analyze first half vs second half of non-zero data
        if n_nz >= 100:
            half = n_nz // 2
            first_half = nz_vals[:half]
            second_half = nz_vals[half:]
            print(f"    First half:  mean={first_half.mean():.2f}, std={first_half.std():.2f}")
            print(f"    Second half: mean={second_half.mean():.2f}, std={second_half.std():.2f}")
            print(f"    Half difference: {abs(first_half.mean() - second_half.mean()):.2f}")

    return buf


def main():
    print("=" * 60)
    print("Dual-Channel Layout Test")
    print("=" * 60)

    with PicoScopeFull() as scope:
        print("\nDevice ready.\n")

        # ============================================================
        # Test 1: Channel A only (B disabled)
        # ============================================================
        print("=" * 60)
        print("TEST 1: Channel A only (5V DC), Channel B DISABLED")
        print("=" * 60)

        scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
        scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
        scope.set_timebase(5, 4000)
        time.sleep(0.1)

        raw_a_only = raw_capture(scope, 4000, 5)
        buf_a_only = analyze_buffer(raw_a_only, "CH_A only")

        time.sleep(0.3)

        # ============================================================
        # Test 2: Channel B only (A disabled)
        # ============================================================
        print("\n" + "=" * 60)
        print("TEST 2: Channel B only (5V DC), Channel A DISABLED")
        print("=" * 60)

        scope.set_channel(Channel.A, False, Coupling.DC, Range.R_5V)
        scope.set_channel(Channel.B, True, Coupling.DC, Range.R_5V)
        scope.set_timebase(5, 4000)
        time.sleep(0.1)

        raw_b_only = raw_capture(scope, 4000, 5)
        buf_b_only = analyze_buffer(raw_b_only, "CH_B only")

        time.sleep(0.3)

        # ============================================================
        # Test 3: Both channels enabled
        # ============================================================
        print("\n" + "=" * 60)
        print("TEST 3: Both channels (A=5V DC, B=5V DC)")
        print("=" * 60)

        scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
        scope.set_channel(Channel.B, True, Coupling.DC, Range.R_5V)
        scope.set_timebase(5, 4000)
        time.sleep(0.1)

        raw_both = raw_capture(scope, 4000, 5)
        buf_both = analyze_buffer(raw_both, "Both channels")

        time.sleep(0.3)

        # ============================================================
        # Test 4: Both channels, different ranges
        # ============================================================
        print("\n" + "=" * 60)
        print("TEST 4: Both channels (A=20V, B=100mV) - max range difference")
        print("=" * 60)

        scope.set_channel(Channel.A, True, Coupling.DC, Range.R_20V)
        scope.set_channel(Channel.B, True, Coupling.DC, Range.R_100MV)
        scope.set_timebase(5, 4000)
        time.sleep(0.1)

        raw_diff = raw_capture(scope, 4000, 5)
        buf_diff = analyze_buffer(raw_diff, "A=20V, B=100mV")

        # ============================================================
        # Comparison
        # ============================================================
        print("\n" + "=" * 60)
        print("COMPARISON")
        print("=" * 60)

        if buf_a_only is not None and buf_b_only is not None:
            nz_a = buf_a_only[buf_a_only != 0]
            nz_b = buf_b_only[buf_b_only != 0]
            if len(nz_a) > 0 and len(nz_b) > 0:
                print(f"\n  A-only mean: {nz_a.mean():.2f}, B-only mean: {nz_b.mean():.2f}")
                print(f"  Difference: {abs(nz_a.mean() - nz_b.mean()):.2f}")
                print(f"  A-only std: {nz_a.std():.2f}, B-only std: {nz_b.std():.2f}")

        if buf_a_only is not None and buf_both is not None:
            nz_a = buf_a_only[buf_a_only != 0]
            nz_both = buf_both[buf_both != 0]
            if len(nz_a) > 0 and len(nz_both) > 0:
                print(f"\n  A-only non-zero count: {len(nz_a)}")
                print(f"  Both non-zero count:   {len(nz_both)}")
                print(f"  Ratio: {len(nz_both)/len(nz_a):.2f}x")
                print(f"  If dual-channel has ~2x the data, data is interleaved or sequential in ONE buffer")
                print(f"  If same count, device only captures one channel regardless of config")

        if buf_diff is not None:
            nz_diff = buf_diff[buf_diff != 0]
            if len(nz_diff) >= 100:
                # Check if there are two distinct clusters
                vals = np.sort(nz_diff)
                # Look at value distribution
                q25 = np.percentile(vals, 25)
                q50 = np.percentile(vals, 50)
                q75 = np.percentile(vals, 75)
                print(f"\n  Diff-range buffer value distribution:")
                print(f"    Q25={q25:.0f}, Q50={q50:.0f}, Q75={q75:.0f}")
                print(f"    Min={vals[0]}, Max={vals[-1]}")

                # Check for bimodal distribution
                # If A=20V and B=100mV with floating input (~2.5V):
                # A raw ≈ 2500/20000 * 32767 ≈ 4096
                # B raw ≈ 2500/100 * 32767 ≈ saturated (32767)
                # Actually range affects PGA gain, so raw ADC values differ
                unique_clusters = []
                threshold = 1000  # If values are separated by >1000, it's a cluster boundary
                prev = vals[0]
                cluster_start = vals[0]
                for v in vals[1:]:
                    if v - prev > threshold:
                        unique_clusters.append((cluster_start, prev))
                        cluster_start = v
                    prev = v
                unique_clusters.append((cluster_start, prev))

                print(f"    Clusters (gap > 1000): {len(unique_clusters)}")
                for i, (lo, hi) in enumerate(unique_clusters):
                    n_in = np.sum((nz_diff >= lo) & (nz_diff <= hi))
                    print(f"      Cluster {i}: [{lo}, {hi}] ({n_in} values)")

                if len(unique_clusters) == 2:
                    print("\n  TWO CLUSTERS DETECTED!")
                    print("  This confirms dual-channel data in the buffer.")
                    # Determine layout
                    c1_vals = nz_diff[nz_diff <= unique_clusters[0][1]]
                    c2_vals = nz_diff[nz_diff >= unique_clusters[1][0]]

                    # Check interleaved vs sequential
                    nz_idx = np.nonzero(buf_diff)[0]
                    nz_data = buf_diff[nz_idx]

                    # Check if values alternate (interleaved) or are grouped (sequential)
                    transitions = 0
                    for i in range(1, len(nz_data)):
                        in_c1_prev = nz_data[i-1] <= unique_clusters[0][1]
                        in_c1_curr = nz_data[i] <= unique_clusters[0][1]
                        if in_c1_prev != in_c1_curr:
                            transitions += 1

                    print(f"  Transitions between clusters: {transitions}")
                    print(f"  Non-zero samples: {len(nz_data)}")
                    if transitions > len(nz_data) * 0.3:
                        print("  → INTERLEAVED layout (many transitions)")
                    elif transitions <= 5:
                        print("  → SEQUENTIAL layout (few transitions)")
                    else:
                        print(f"  → UNCLEAR (transition rate: {transitions/len(nz_data):.2%})")
                elif len(unique_clusters) == 1:
                    print("\n  Only ONE cluster - device may only capture one channel")


if __name__ == "__main__":
    main()
