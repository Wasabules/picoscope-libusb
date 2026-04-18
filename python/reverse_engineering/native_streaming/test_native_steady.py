#!/usr/bin/env python3
"""Measure steady-state native streaming rate over longer durations.

The single-config version — establish the TRUE rate and packet pattern."""
import sys
import os
import time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import picoscope_libusb_full as psf
from picoscope_libusb_full import Range, Channel, Coupling
import usb.core


def measure_steady(scope, rate_word, byte47, duration_s=5.0):
    """Start native streaming, measure rate for duration, then stop."""
    scope._flush_all_buffers()

    cmd1 = scope._build_streaming_cmd1(100000)
    scope.device.write(0x01, cmd1.ljust(64, b'\x00'), timeout=1000)

    # Custom cmd2
    r = rate_word.to_bytes(4, 'little')
    cmd2 = bytes([
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00,
        r[0], r[1], r[2], r[3],
        0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x0b, 0x03, byte47 & 0xFF,
        0x00, 0x02, 0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    ])
    scope.device.write(0x01, cmd2.ljust(64, b'\x00'), timeout=1000)

    for _ in range(3):
        try:
            scope.device.read(0x81, 64, timeout=30)
        except (usb.core.USBTimeoutError, usb.core.USBError):
            break

    trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00])
    scope.device.write(0x01, trigger.ljust(64, b'\x00'), timeout=1000)

    total_bytes = 0
    packet_sizes = []
    timestamps = []
    t0 = time.time()
    while time.time() - t0 < duration_s:
        try:
            raw = bytes(scope.device.read(0x82, 4096, timeout=100))
            if raw:
                total_bytes += len(raw)
                packet_sizes.append(len(raw))
                timestamps.append(time.time() - t0)
        except (usb.core.USBTimeoutError, usb.core.USBError):
            continue

    dt = time.time() - t0

    # Stop
    try:
        stop = bytes([0x02, 0x0a, 0x00, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
        scope.device.write(0x01, stop.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.01)
        stop2 = bytes([0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
        scope.device.write(0x01, stop2.ljust(64, b'\x00'), timeout=1000)
    except usb.core.USBError:
        pass
    scope._flush_all_buffers()

    print(f"  Duration: {dt:.2f}s  Total bytes: {total_bytes}  Packets: {len(packet_sizes)}")
    if packet_sizes:
        print(f"  Rate: {total_bytes/dt:.1f} B/s ({total_bytes/dt:.1f} samples/s)")
        print(f"  Pkt sizes: min={min(packet_sizes)} max={max(packet_sizes)} avg={total_bytes/len(packet_sizes):.1f}")
        # Inter-packet intervals
        if len(timestamps) > 1:
            intervals = [timestamps[i+1] - timestamps[i] for i in range(len(timestamps)-1)]
            print(f"  Inter-pkt: min={min(intervals)*1000:.1f}ms max={max(intervals)*1000:.1f}ms avg={sum(intervals)/len(intervals)*1000:.1f}ms")
    return total_bytes, dt


def main():
    scope = psf.PicoScopeFull()
    if not scope.open():
        return
    try:
        scope.set_channel(Channel.A, enabled=True, coupling=Coupling.DC, range_=Range.R_5V)
        scope.set_channel(Channel.B, enabled=False, coupling=Coupling.DC, range_=Range.R_5V)
        time.sleep(0.2)

        print("=" * 70)
        print("[Test 1] Baseline Python defaults (rate_word=0x10a619b0, byte47=0x05)")
        print("=" * 70)
        measure_steady(scope, 0x10a619b0, 0x05, duration_s=5.0)
    finally:
        scope.close()


if __name__ == '__main__':
    main()
