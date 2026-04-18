#!/usr/bin/env python3
"""Test if set_channel() range actually affects raw ADC values.

This tests whether the PGA (Programmable Gain Amplifier) is being
controlled by our set_channel() command.

With a floating input at ~4.9V:
- 5V range: raw ADC ≈ 32112 (near full scale)
- 2V range: raw ADC = 32767 (saturated)
- 20V range: raw ADC ≈ 8028 (quarter scale)
- 500mV range: raw ADC = 32767 (saturated)

If the range is working, we should see DIFFERENT raw ADC values.
If all are the same, set_channel() isn't controlling the PGA.
"""
import sys
import os
import time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from picoscope_libusb_full import (
    PicoScopeFull, Channel, Coupling, Range,
    EP_CMD_OUT, EP_RESP_IN, EP_DATA_IN
)
import usb.core


def raw_capture(scope, n_samples=2000, timebase=5):
    """Do a capture and return the raw int16 buffer."""
    scope._flush_all_buffers()

    config_cmd1 = scope._build_capture_cmd1(n_samples, Channel.A, timebase)
    scope.device.write(EP_CMD_OUT, config_cmd1.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)

    config_cmd2 = scope._build_capture_cmd2()
    scope.device.write(EP_CMD_OUT, config_cmd2.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    for _ in range(3):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=100)
        except:
            break

    status = scope._poll_status(timeout_ms=3000)
    if status != 0x3b:
        print(f"  WARNING: status=0x{status:02x}")
        return None

    trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
    scope.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=5000))
    except usb.core.USBTimeoutError:
        return None

    return raw


def raw_capture_with_sdk_bytes(scope, n_samples=2000, timebase=5):
    """Capture using SDK-matching compound command (0x01 0x57 instead of channel bytes)."""
    scope._flush_all_buffers()

    samples_hi = (n_samples >> 8) & 0xFF
    samples_lo = n_samples & 0xFF
    tb_byte = 0x20 | (timebase & 0x0F)
    trigger_buf = 0x20

    # Use SDK-exact bytes for 0x85 0x08 0x93: 01 57 instead of channel-based
    cmd1 = bytes([
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, trigger_buf, 0x00,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x57,  # SDK values
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x20,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, samples_hi, samples_lo, tb_byte, 0xc4, 0x40,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
    ])

    scope.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)

    config_cmd2 = scope._build_capture_cmd2()
    scope.device.write(EP_CMD_OUT, config_cmd2.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    for _ in range(3):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=100)
        except:
            break

    status = scope._poll_status(timeout_ms=3000)
    if status != 0x3b:
        print(f"  WARNING: status=0x{status:02x}")
        return None

    trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
    scope.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=5000))
    except usb.core.USBTimeoutError:
        return None

    return raw


def get_stats(raw, label):
    """Get stats from raw buffer."""
    if raw is None or len(raw) < 4:
        print(f"  {label}: No data")
        return None

    samples = np.frombuffer(raw, dtype='<i2')
    buf = samples[1:]  # skip header
    nz = buf[buf != 0]

    if len(nz) == 0:
        print(f"  {label}: All zeros")
        return None

    print(f"  {label}: {len(nz)} non-zero, mean={nz.mean():.1f}, "
          f"min={nz.min()}, max={nz.max()}, std={nz.std():.2f}")
    return nz.mean()


def main():
    print("=" * 60)
    print("Range Effect Test")
    print("=" * 60)

    with PicoScopeFull() as scope:
        print("\nDevice ready.\n")

        ranges_to_test = [
            (Range.R_20V, "20V"),
            (Range.R_5V, "5V"),
            (Range.R_2V, "2V"),
            (Range.R_1V, "1V"),
            (Range.R_500MV, "500mV"),
            (Range.R_200MV, "200mV"),
            (Range.R_100MV, "100mV"),
        ]

        # Test A: Using our compound command (with channel bytes in 0x85 0x08 0x93)
        print("=" * 60)
        print("Test A: set_channel() + our compound command")
        print("=" * 60)

        means_a = {}
        for rng, name in ranges_to_test:
            scope.set_channel(Channel.A, True, Coupling.DC, rng)
            scope.set_timebase(5, 2000)
            time.sleep(0.1)

            raw = raw_capture(scope, 2000, 5)
            mean = get_stats(raw, f"Range {name:>5s}")
            means_a[name] = mean
            time.sleep(0.2)

        # Test B: Using SDK-matching compound command
        print(f"\n{'=' * 60}")
        print("Test B: set_channel() + SDK-matching compound (01 57)")
        print("=" * 60)

        means_b = {}
        for rng, name in ranges_to_test:
            scope.set_channel(Channel.A, True, Coupling.DC, rng)
            scope.set_timebase(5, 2000)
            time.sleep(0.1)

            raw = raw_capture_with_sdk_bytes(scope, 2000, 5)
            mean = get_stats(raw, f"Range {name:>5s}")
            means_b[name] = mean
            time.sleep(0.2)

        # Summary
        print(f"\n{'=' * 60}")
        print("SUMMARY")
        print("=" * 60)

        print(f"\n  {'Range':<8s} | {'Our cmd':>10s} | {'SDK cmd':>10s} | {'Diff':>8s}")
        print(f"  {'-'*8} | {'-'*10} | {'-'*10} | {'-'*8}")
        for _, name in ranges_to_test:
            ma = means_a.get(name)
            mb = means_b.get(name)
            ma_str = f"{ma:.0f}" if ma else "N/A"
            mb_str = f"{mb:.0f}" if mb else "N/A"
            diff_str = f"{abs(ma-mb):.0f}" if (ma and mb) else "N/A"
            print(f"  {name:<8s} | {ma_str:>10s} | {mb_str:>10s} | {diff_str:>8s}")

        # Check if range makes any difference
        all_means = [m for m in means_a.values() if m is not None]
        if all_means:
            spread = max(all_means) - min(all_means)
            print(f"\n  Our cmd spread: {spread:.0f} (max-min of means)")
            if spread < 500:
                print("  → Range setting has NO EFFECT on raw ADC ❌")
            else:
                print("  → Range setting IS affecting raw ADC ✓")

        all_means_b = [m for m in means_b.values() if m is not None]
        if all_means_b:
            spread_b = max(all_means_b) - min(all_means_b)
            print(f"  SDK cmd spread: {spread_b:.0f} (max-min of means)")
            if spread_b < 500:
                print("  → Range setting has NO EFFECT with SDK cmd ❌")
            else:
                print("  → Range setting IS affecting with SDK cmd ✓")


if __name__ == "__main__":
    main()
