#!/usr/bin/env python3
"""Analyze raw buffer byte-by-byte to determine data format (8-bit vs 16-bit, interleaving)."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import (PicoScopeFull, EP_CMD_OUT, EP_RESP_IN,
                                   EP_DATA_IN, Range, Channel, Coupling)
import numpy as np

scope = PicoScopeFull()
scope.open()

scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
scope.set_timebase(5, 1000)

result = scope.capture_block(1000)

# Read raw data directly
scope._flush_all_buffers()
cmd1 = scope._build_capture_cmd1(1000, Channel.A, 5)
cmd2 = scope._build_capture_cmd2()
status_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')

scope._send_cmd(cmd1, read_response=False)
scope._send_cmd(cmd2, read_response=False)
scope._send_cmd(status_cmd, read_response=False)

time.sleep(0.02)
resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=2000))
status = resp[0] if resp else 0
if status == 0x33:
    for _ in range(50):
        time.sleep(0.05)
        scope.device.write(EP_CMD_OUT, status_cmd, timeout=1000)
        resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=500))
        if resp and resp[0] == 0x3b:
            status = 0x3b
            break

if status != 0x3b:
    print(f"Failed: status 0x{status:02x}")
    scope.close()
    exit(1)

read_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]).ljust(64, b'\x00')
scope.device.write(EP_CMD_OUT, read_cmd, timeout=1000)
raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=5000))

print(f"\n=== Raw Buffer Analysis ({len(raw)} bytes) ===\n")

# Header bytes
print(f"Header: {raw[0]:02x} {raw[1]:02x}")

# Analyze data region (skip header)
data = raw[2:]

# Split into even and odd bytes
even_bytes = np.array([data[i] for i in range(0, len(data), 2)], dtype=np.uint8)
odd_bytes = np.array([data[i] for i in range(1, len(data), 2)], dtype=np.uint8)

print(f"\nEven bytes (0, 2, 4, ...): mean={np.mean(even_bytes):.1f} std={np.std(even_bytes):.2f} min={np.min(even_bytes)} max={np.max(even_bytes)}")
print(f"Odd bytes  (1, 3, 5, ...): mean={np.mean(odd_bytes):.1f} std={np.std(odd_bytes):.2f} min={np.min(odd_bytes)} max={np.max(odd_bytes)}")

# Count unique values
from collections import Counter
even_counts = Counter(even_bytes)
odd_counts = Counter(odd_bytes)
print(f"\nEven byte values: {dict(sorted(even_counts.items()))}")
print(f"Odd byte values:  {dict(sorted(odd_counts.items()))}")

# Find non-zero region
all_bytes = np.frombuffer(data, dtype=np.uint8)
nonzero_mask = all_bytes != 0
first_nonzero = np.argmax(nonzero_mask)
last_nonzero = len(all_bytes) - 1 - np.argmax(nonzero_mask[::-1])
print(f"\nNon-zero region: bytes {first_nonzero} to {last_nonzero} ({last_nonzero - first_nonzero + 1} bytes)")

# Show the first 100 non-zero bytes as hex
nonzero_region = all_bytes[first_nonzero:min(first_nonzero+100, last_nonzero+1)]
print(f"\nFirst 100 non-zero bytes:")
for i in range(0, len(nonzero_region), 16):
    row = nonzero_region[i:i+16]
    hex_str = ' '.join(f'{b:02x}' for b in row)
    print(f"  +{first_nonzero+i:5d}: {hex_str}")

# Try interpretation 1: uint8 samples (each byte = one 8-bit ADC sample)
uint8_data = all_bytes[first_nonzero:last_nonzero+1]
uint8_nonzero = uint8_data[uint8_data != 0]
if len(uint8_nonzero) > 0:
    signed = uint8_nonzero.astype(np.int16) - 128
    print(f"\nAs uint8 (- 128): mean={np.mean(signed):.1f} std={np.std(signed):.2f}")

# Try interpretation 2: even bytes only (channel A in interleaved mode)
even_nonzero = even_bytes[even_bytes != 0]
if len(even_nonzero) > 0:
    signed_even = even_nonzero.astype(np.int16) - 128
    print(f"Even bytes only (- 128): mean={np.mean(signed_even):.1f} std={np.std(signed_even):.2f} n={len(even_nonzero)}")

# Try interpretation 3: odd bytes only
odd_nonzero = odd_bytes[odd_bytes != 0]
if len(odd_nonzero) > 0:
    signed_odd = odd_nonzero.astype(np.int16) - 128
    print(f"Odd bytes only (- 128): mean={np.mean(signed_odd):.1f} std={np.std(signed_odd):.2f} n={len(odd_nonzero)}")

# Try interpretation 4: int16 LE, extract high byte as 8-bit ADC
int16_data = np.frombuffer(raw[2:], dtype='<i2')
int16_nonzero = int16_data[int16_data != 0]
high_bytes = ((int16_nonzero.astype(np.uint16) >> 8) & 0xFF).astype(np.int16) - 128
low_bytes = (int16_nonzero.astype(np.uint16) & 0xFF).astype(np.int16) - 128
if len(high_bytes) > 0:
    print(f"\nInt16 high byte (- 128): mean={np.mean(high_bytes):.1f} std={np.std(high_bytes):.2f}")
    print(f"Int16 low byte (- 128):  mean={np.mean(low_bytes):.1f} std={np.std(low_bytes):.2f}")

scope.close()
