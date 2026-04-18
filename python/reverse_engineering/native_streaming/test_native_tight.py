#!/usr/bin/env python3
"""Test tight streaming read loop to see if streaming is sustained."""
import sys
import os
import time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import picoscope_libusb_full as psf
from picoscope_libusb_full import Range, Channel, Coupling
import usb.core


def main():
    scope = psf.PicoScopeFull()
    if not scope.open():
        return
    try:
        scope.set_channel(Channel.A, enabled=True, coupling=Coupling.DC, range_=Range.R_5V)
        scope.set_channel(Channel.B, enabled=False, coupling=Coupling.DC, range_=Range.R_5V)
        time.sleep(0.3)

        # EXACT SDK legacy bytes
        cmd1 = bytes([
            0x02, 0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01,
            0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x06,
            0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x0f, 0x42, 0x3f,
            0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x41,
            0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
            0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x21, 0x42, 0xe0,
            0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
        ])
        cmd2 = bytes([
            0x02,
            0x85, 0x0c, 0x86, 0x00, 0x40, 0x00,
            0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
            0x85, 0x0b, 0x90, 0x00, 0x38, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x08, 0x8a, 0x00, 0x20, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x0b, 0x03, 0x14, 0x00,
            0x02, 0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        ])
        trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00])

        print("\n=== Test: small reads, tight loop ===")
        scope._flush_all_buffers()
        scope.device.write(0x01, cmd1.ljust(64, b'\x00'), timeout=1000)
        scope.device.write(0x01, cmd2.ljust(64, b'\x00'), timeout=1000)
        # Don't flush ACKs - try sending trigger immediately
        scope.device.write(0x01, trigger.ljust(64, b'\x00'), timeout=1000)

        total = 0
        packets = []
        t0 = time.time()
        last_data_t = t0
        duration = 10.0
        while time.time() - t0 < duration:
            try:
                # Small read buffer — match SDK's ~20 byte packets
                raw = bytes(scope.device.read(0x82, 512, timeout=10))
                if raw:
                    total += len(raw)
                    packets.append((time.time() - t0, len(raw)))
                    last_data_t = time.time()
            except usb.core.USBTimeoutError:
                # Is the device still sending? Check if stale
                if time.time() - last_data_t > 2.0 and packets:
                    print(f"  No data for 2s after {len(packets)} packets, stopping")
                    break
                continue
            except usb.core.USBError as e:
                print(f"  USBError: {e}")
                break

        dt = time.time() - t0
        print(f"\n  Duration: {dt:.2f}s  Total: {total}  Packets: {len(packets)}")
        if packets:
            print(f"  Rate: {total/dt:.1f} B/s")
            # First 20 packets
            for i, (t, n) in enumerate(packets[:20]):
                print(f"    [{i:3d}] t={t*1000:7.1f}ms  sz={n}")

        # Stop
        try:
            stop = bytes([0x02, 0x0a, 0x00, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
            scope.device.write(0x01, stop.ljust(64, b'\x00'), timeout=1000)
        except usb.core.USBError:
            pass
    finally:
        scope.close()


if __name__ == '__main__':
    main()
