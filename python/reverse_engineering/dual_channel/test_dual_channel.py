#!/usr/bin/env python3
"""Test dual-channel capture to determine data layout (sequential vs interleaved)."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import (PicoScopeFull, EP_CMD_OUT, EP_RESP_IN,
                                   EP_DATA_IN, Range, Channel, Coupling,
                                   RANGE_MV, ADC_HALF_RANGE, ADC_CENTER)
import numpy as np

scope = PicoScopeFull()
scope.open()

# === Test 1: Single channel baseline ===
print("\n=== Test 1: Single channel CH A (CH B disabled) ===\n")
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
scope.set_timebase(5, 1000)

result = scope.capture_block(1000)
if result and 'A' in result:
    s = result['A']
    print(f"  CH A: {len(s)} samples, mean={np.mean(s):+.1f}mV, std={np.std(s):.1f}mV")
else:
    print("  FAILED")

# === Test 2: Both channels enabled ===
print("\n=== Test 2: Both channels enabled (floating inputs) ===\n")
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, True, Coupling.DC, Range.R_5V)
scope.set_timebase(5, 1000)

result = scope.capture_block(1000)
if result:
    for ch in ['A', 'B']:
        if ch in result and result[ch] is not None:
            s = result[ch]
            print(f"  CH {ch}: {len(s)} samples, mean={np.mean(s):+.1f}mV, std={np.std(s):.1f}mV")
        else:
            print(f"  CH {ch}: no data")
    print(f"  Total samples reported: {result.get('samples', 'N/A')}")
else:
    print("  FAILED")

# === Test 3: Raw buffer analysis for dual channel ===
print("\n=== Test 3: Raw buffer analysis (dual channel) ===\n")

# Do a manual capture to inspect the raw buffer
scope._flush_all_buffers()
cmd1 = scope._build_capture_cmd1(2000, Channel.A, 5)
cmd2 = scope._build_capture_cmd2()

scope.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
time.sleep(0.01)
scope.device.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)
time.sleep(0.02)

# Flush config responses
for _ in range(3):
    try:
        scope.device.read(EP_RESP_IN, 64, timeout=100)
    except:
        break

status = scope._poll_status(timeout_ms=3000)
print(f"  Status: 0x{status:02x}")

if status == 0x3b:
    trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
    scope.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.05)

    # Try reading 32KB (dual channel might send 2x16KB)
    raw = bytes()
    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 32768, timeout=5000))
    except Exception as e:
        print(f"  Read 32KB error: {e}")

    print(f"  Raw buffer size: {len(raw)} bytes")

    if len(raw) >= 16384:
        # Analyze as uint8
        all_bytes = np.frombuffer(raw, dtype=np.uint8)

        # First 16KB
        first_half = all_bytes[:16384]
        fh_data = first_half[2:]  # skip header
        fh_nonzero = fh_data[fh_data != 0]
        fh_mean = np.mean(fh_nonzero.astype(np.int16) - ADC_CENTER) if len(fh_nonzero) > 0 else 0

        print(f"\n  First 16KB: header=0x{first_half[0]:02x}{first_half[1]:02x}, "
              f"non-zero={len(fh_nonzero)}/{len(fh_data)}, "
              f"mean(centered)={fh_mean:+.1f}")

        if len(raw) >= 32768:
            # Second 16KB
            second_half = all_bytes[16384:32768]
            sh_data = second_half[2:]  # skip header
            sh_nonzero = sh_data[sh_data != 0]
            sh_mean = np.mean(sh_nonzero.astype(np.int16) - ADC_CENTER) if len(sh_nonzero) > 0 else 0

            print(f"  Second 16KB: header=0x{second_half[0]:02x}{second_half[1]:02x}, "
                  f"non-zero={len(sh_nonzero)}/{len(sh_data)}, "
                  f"mean(centered)={sh_mean:+.1f}")
        else:
            print(f"  Only got {len(raw)} bytes (no second 16KB block)")

        # Check interleaved pattern (A, B, A, B, ...)
        even_bytes = fh_data[::2]  # bytes at even indices
        odd_bytes = fh_data[1::2]  # bytes at odd indices
        even_nz = even_bytes[even_bytes != 0]
        odd_nz = odd_bytes[odd_bytes != 0]
        if len(even_nz) > 0 and len(odd_nz) > 0:
            even_mean = np.mean(even_nz.astype(np.int16) - ADC_CENTER)
            odd_mean = np.mean(odd_nz.astype(np.int16) - ADC_CENTER)
            print(f"\n  Interleaved check (first 16KB):")
            print(f"    Even indices: mean={even_mean:+.1f}, n={len(even_nz)}")
            print(f"    Odd indices:  mean={odd_mean:+.1f}, n={len(odd_nz)}")

        # Show first few bytes of data region
        print(f"\n  First 32 data bytes (after header): {' '.join(f'{b:02x}' for b in fh_data[:32])}")
        if len(raw) >= 32768:
            sh_data_start = all_bytes[16386:16418]  # Second block data region
            print(f"  Second block first 32 bytes:       {' '.join(f'{b:02x}' for b in sh_data_start)}")

# === Test 4: Different ranges on A and B ===
print("\n=== Test 4: Different ranges (A=5V, B=500mV) ===\n")
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, True, Coupling.DC, Range.R_500MV)
scope.set_timebase(5, 1000)

result = scope.capture_block(1000)
if result:
    for ch in ['A', 'B']:
        if ch in result and result[ch] is not None:
            s = result[ch]
            print(f"  CH {ch}: {len(s)} samples, mean={np.mean(s):+.1f}mV, std={np.std(s):.1f}mV")
else:
    print("  FAILED")

scope.close()
print("\n=== Dual channel tests complete ===")
