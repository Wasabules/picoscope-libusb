#!/usr/bin/env python3
"""Use strace to capture USB ioctl calls from the SDK.

strace captures the actual USB request blocks (URBs) sent via ioctl to usbfs.
We can parse these to see the exact bytes sent for each set_channel call.
"""
import subprocess
import sys
import os
import re
import struct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def run_sdk_with_strace():
    """Run the SDK range test under strace and capture ioctls."""
    sdk_script = os.path.join(SCRIPT_DIR, "capture_sdk_ranges_simple.py")

    # Use strace to capture all ioctl calls (USB goes through ioctls)
    # -e trace=ioctl captures USB submit/reap URBs
    # -f follows child threads
    # -x prints hex strings
    result = subprocess.run(
        ["strace", "-f", "-e", "trace=ioctl,write", "-s", "256", "-x",
         "python3", sdk_script],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=SCRIPT_DIR
    )

    return result.stdout, result.stderr


def parse_strace_output(stderr):
    """Parse strace output for USB bulk transfer data."""
    # In strace, libusb uses ioctl(fd, USBDEVFS_SUBMITURB, ...) and
    # ioctl(fd, USBDEVFS_REAPURB, ...)
    # The key is finding bulk OUT data to EP 0x01

    lines = stderr.split('\n')
    print(f"Total strace lines: {len(lines)}")

    # Look for patterns that contain hex data after set_channel prints
    usb_data_lines = []
    marker_lines = []

    for i, line in enumerate(lines):
        # Look for ioctl calls with hex data
        if 'ioctl' in line and ('USBDEVFS' in line or '\\x' in line):
            usb_data_lines.append((i, line))
        # Look for write() calls that contain our marker text
        if 'Setting Channel' in line or 'MARKER' in line:
            marker_lines.append((i, line))

    print(f"USB-related ioctl lines: {len(usb_data_lines)}")
    print(f"Marker lines: {len(marker_lines)}")

    # Print all ioctl lines with hex data
    for idx, line in usb_data_lines:
        # Truncate very long lines
        if len(line) > 200:
            line = line[:200] + "..."
        print(f"  [{idx:5d}] {line}")


def main():
    print("=" * 60)
    print("SDK USB Capture via strace")
    print("=" * 60)

    # Check strace is available
    result = subprocess.run(["which", "strace"], capture_output=True, text=True)
    if result.returncode != 0:
        print("strace not found! Install with: sudo apt install strace")
        return

    print("\nRunning SDK with strace (this may take ~30 seconds)...")
    stdout, stderr = run_sdk_with_strace()

    print("\n--- SDK stdout ---")
    print(stdout)

    print("\n--- Parsing strace output ---")
    parse_strace_output(stderr)

    # Also save the full strace output for manual analysis
    strace_file = os.path.join(SCRIPT_DIR, "sdk_strace.log")
    with open(strace_file, 'w') as f:
        f.write(stderr)
    print(f"\nFull strace output saved to: {strace_file}")
    print(f"Lines: {len(stderr.splitlines())}")


if __name__ == "__main__":
    main()
