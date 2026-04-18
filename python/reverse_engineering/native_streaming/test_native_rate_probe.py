#!/usr/bin/env python3
"""Probe native streaming rate encoding.

Systematically varies cmd2 bytes [7:10] and byte[47] while running native
streaming, measures actual data rate on EP 0x82. Goal: find encoding that
produces higher rates than the current ~100 S/s baseline.

Current default:
    cmd2[7:10] = b0 19 a6 10  (LE uint32 = 0x10a619b0 = 279,979,952)
    cmd2[47]   = 0x05

From SDK trace:
    Legacy (10ms, ~100 S/s target): cmd2[7:10]=0x00000018=24, cmd2[47]=0x14=20
    Fast   (10us, 100k S/s target): cmd2[7:10]=0x34eae3f0=887,091,184, cmd2[47]=0x05
"""
import sys
import os
import time
import struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import picoscope_libusb_full as psf


def build_streaming_cmd2_custom(rate_word: int, byte47: int) -> bytes:
    """Build cmd2 with custom rate parameters."""
    r = rate_word.to_bytes(4, 'little')
    return bytes([
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


def measure_rate(scope, rate_word, byte47, duration_s=1.5, verbose=False):
    """Start streaming with custom params, measure bytes/sec for duration."""
    # Flush
    scope._flush_all_buffers()

    # Build and send cmd1 + custom cmd2 + trigger
    cmd1 = scope._build_streaming_cmd1(10000)
    scope.device.write(0x01, cmd1.ljust(64, b'\x00'), timeout=1000)

    cmd2 = build_streaming_cmd2_custom(rate_word, byte47)
    scope.device.write(0x01, cmd2.ljust(64, b'\x00'), timeout=1000)

    # Drain ACKs
    import usb.core
    for _ in range(3):
        try:
            scope.device.read(0x81, 64, timeout=30)
        except (usb.core.USBTimeoutError, usb.core.USBError):
            break

    # Streaming trigger
    trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00])
    scope.device.write(0x01, trigger.ljust(64, b'\x00'), timeout=1000)

    # Measure data rate
    total_bytes = 0
    packet_count = 0
    t0 = time.time()
    sizes = []
    while time.time() - t0 < duration_s:
        try:
            raw = bytes(scope.device.read(0x82, 4096, timeout=50))
            if raw:
                total_bytes += len(raw)
                packet_count += 1
                sizes.append(len(raw))
        except (usb.core.USBTimeoutError, usb.core.USBError):
            continue

    dt = time.time() - t0

    # Send stop
    try:
        stop = bytes([0x02, 0x0a, 0x00, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
        scope.device.write(0x01, stop.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.01)
        stop2 = bytes([0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
        scope.device.write(0x01, stop2.ljust(64, b'\x00'), timeout=1000)
    except usb.core.USBError:
        pass
    scope._flush_all_buffers()
    time.sleep(0.1)

    rate = total_bytes / dt if dt > 0 else 0
    if verbose:
        avg_sz = total_bytes / packet_count if packet_count else 0
        print(f"  bytes={total_bytes}  pkts={packet_count}  avg_pkt={avg_sz:.1f}  rate={rate:.1f} B/s")
    return rate, total_bytes, packet_count


def main():
    scope = psf.PicoScopeFull()
    if not scope.open():
        print("Failed to open PicoScope")
        return
    try:
        from picoscope_libusb_full import Range, Channel, Coupling
        scope.set_channel(Channel.A, enabled=True, coupling=Coupling.DC, range_=Range.R_5V)
        scope.set_channel(Channel.B, enabled=False, coupling=Coupling.DC, range_=Range.R_5V)
        time.sleep(0.2)

        print("=" * 70)
        print("Native streaming rate probe")
        print("=" * 70)

        # Baseline: current Python defaults
        print("\n[Baseline] Current Python defaults (b0 19 a6 10, byte47=0x05)")
        r, n, p = measure_rate(scope, 0x10a619b0, 0x05, verbose=True)
        baseline = r

        # SDK legacy values
        print("\n[SDK-legacy] bytes=24, byte47=0x14 (10ms target)")
        r, n, p = measure_rate(scope, 24, 0x14, verbose=True)

        # SDK fast values
        print("\n[SDK-fast] bytes=0x34eae3f0, byte47=0x05 (10us target)")
        r, n, p = measure_rate(scope, 0x34eae3f0, 0x05, verbose=True)

        # Probe byte[47] variations with baseline rate_word
        print("\n[Probe byte47] byte47 in {1,2,5,10,20,50,100,200}")
        for b47 in [1, 2, 5, 10, 20, 50, 100, 200]:
            r, n, p = measure_rate(scope, 0x10a619b0, b47, duration_s=1.0)
            print(f"  byte47=0x{b47:02x} ({b47:3d})  rate={r:8.1f} B/s  pkts={p:4d}  bytes={n}")

        # Probe rate_word variations with small byte47
        print("\n[Probe rate_word] vary bytes[7:10], byte47=0x05")
        words = [0x18, 100, 1000, 10000, 100000,
                 0x10a619b0, 0x34eae3f0,
                 0xFFFFFFFF, 0x80000000, 0x40000000, 0x00000001, 0]
        for w in words:
            r, n, p = measure_rate(scope, w, 0x05, duration_s=1.0)
            print(f"  word=0x{w:08x}  rate={r:8.1f} B/s  pkts={p:4d}  bytes={n}")

        # Probe byte47=0 (might disable some prescaler)
        print("\n[Probe byte47=0 / rate=low]")
        for w in [0, 1, 2, 4, 8, 16]:
            r, n, p = measure_rate(scope, w, 0x00, duration_s=1.0)
            print(f"  word={w:5d}  byte47=0x00  rate={r:8.1f} B/s")

        print(f"\nBaseline rate: {baseline:.1f} B/s = {baseline:.1f} samples/s")
    finally:
        scope.close()


if __name__ == '__main__':
    main()
