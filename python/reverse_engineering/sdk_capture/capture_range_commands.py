#!/usr/bin/env python3
"""Capture USB commands the SDK sends for different voltage ranges.

Uses the USB interceptor (LD_PRELOAD) to log USB traffic for each
range setting, then parses the log to find the channel config commands.
"""
import subprocess
import os
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
INTERCEPTOR = os.path.join(SCRIPT_DIR, "usb_interceptor.so")
LOG_FILE = os.path.join(SCRIPT_DIR, "usb_trace.log")

# Range values matching SDK enum
RANGES = [
    (0, "10mV"),
    (1, "20mV"),
    (2, "50mV"),
    (3, "100mV"),
    (4, "200mV"),
    (5, "500mV"),
    (6, "1V"),
    (7, "2V"),
    (8, "5V"),
    (9, "10V"),
    (10, "20V"),
]


def run_sdk_range_test(range_val, range_name):
    """Run SDK set_channel with a specific range and capture USB commands."""
    # Python script to run with interceptor
    test_script = f"""
import ctypes
from ctypes import c_int16
lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")
handle = lib.ps2000_open_unit()
if handle > 0:
    # Set Channel A with range {range_val} ({range_name})
    lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16({range_val}))
    import time; time.sleep(0.2)
    lib.ps2000_close_unit(c_int16(handle))
else:
    print("Failed to open")
"""

    # Remove old log
    try:
        os.remove(LOG_FILE)
    except FileNotFoundError:
        pass

    # Run with interceptor
    env = os.environ.copy()
    env["LD_PRELOAD"] = INTERCEPTOR
    env["USB_TRACE_LOG"] = LOG_FILE

    result = subprocess.run(
        ["python3", "-c", test_script],
        env=env,
        capture_output=True,
        text=True,
        timeout=15,
        cwd=SCRIPT_DIR
    )

    if result.returncode != 0:
        print(f"  ERROR: {result.stderr[:200]}")
        return None

    # Parse the log for bulk write commands
    if not os.path.exists(LOG_FILE):
        print(f"  No log file generated")
        return None

    with open(LOG_FILE, 'r') as f:
        lines = f.readlines()

    # Find bulk_transfer OUT lines (EP 0x01)
    bulk_out_cmds = []
    for line in lines:
        if "bulk_transfer" in line and "ep=0x01" in line.lower():
            bulk_out_cmds.append(line.strip())
        elif "bulk_transfer" in line and "endpoint=1" in line.lower():
            bulk_out_cmds.append(line.strip())
        elif "bulk" in line.lower() and ("OUT" in line or "0x01" in line):
            bulk_out_cmds.append(line.strip())

    return lines, bulk_out_cmds


def main():
    print("=" * 70)
    print("SDK Range Command Capture")
    print("=" * 70)

    if not os.path.exists(INTERCEPTOR):
        print(f"ERROR: Interceptor not found at {INTERCEPTOR}")
        print("Build with: gcc -shared -fPIC -o usb_interceptor.so usb_interceptor.c -ldl -lusb-1.0")
        return

    # First, do a baseline capture with 5V range and get ALL bulk writes
    print("\nCapturing SDK commands for 5V range (baseline)...")
    result = run_sdk_range_test(8, "5V")
    if result:
        lines, cmds = result
        print(f"  Total log lines: {len(lines)}")
        print(f"  Bulk OUT commands: {len(cmds)}")

        # Print ALL lines that contain hex data (likely command data)
        print("\n  --- All log lines ---")
        for i, line in enumerate(lines):
            print(f"  [{i:3d}] {line.strip()[:120]}")

    # Now try different ranges
    for range_val, range_name in RANGES:
        print(f"\n{'=' * 70}")
        print(f"Range: {range_name} (value={range_val})")
        print(f"{'=' * 70}")

        result = run_sdk_range_test(range_val, range_name)
        if result:
            lines, cmds = result
            print(f"  Log lines: {len(lines)}, Bulk OUT: {len(cmds)}")

            # Look for channel-related commands (0x85 0x21 pattern)
            for line in lines:
                if "8521" in line.replace(" ", "").lower() or "85 21" in line.lower():
                    print(f"  CHANNEL CMD: {line.strip()[:120]}")

            # Also look for 0x85 0x04 0x9b pattern (channel config wrapper)
            for line in lines:
                if "85049b" in line.replace(" ", "").lower() or "85 04 9b" in line.lower():
                    print(f"  CONFIG: {line.strip()[:120]}")

            # Print ALL lines for inspection (first range only already printed above)
            if range_val != 8:
                # Print just the key lines with hex data
                for line in lines:
                    s = line.strip()
                    if len(s) > 20 and any(c in s for c in ['0x', 'bulk', 'control']):
                        print(f"  {s[:120]}")


if __name__ == "__main__":
    main()
