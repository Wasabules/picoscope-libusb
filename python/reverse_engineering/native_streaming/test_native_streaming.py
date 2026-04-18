#!/usr/bin/env python3
"""Test native hardware streaming protocol directly.
Sends the exact SDK streaming commands and reads raw data from EP 0x82."""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
from picoscope_libusb_full import *
import time

scope = PicoScopeFull()
scope.open()
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)

print("\n=== Test 1: Native streaming raw data ===")
scope._flush_all_buffers()

# Build and send streaming commands
cmd1 = scope._build_streaming_cmd1(10000)
print(f"cmd1 ({len(cmd1)} bytes): {cmd1.hex()}")

cmd2 = scope._build_streaming_cmd2()
print(f"cmd2 ({len(cmd2)} bytes): {cmd2.hex()}")

scope.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
scope.device.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)

# Flush ACKs
for _ in range(5):
    try:
        resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=50))
        print(f"  ACK: {resp.hex()}")
    except:
        break

# Send streaming trigger
trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00])
scope.device.write(EP_CMD_OUT, trigger.ljust(64, b'\x00'), timeout=1000)
print("Trigger sent, reading data...")

# Read data aggressively
total_bytes = 0
reads = 0
t0 = time.time()
raw_samples = []

for i in range(200):
    try:
        data = bytes(scope.device.read(EP_DATA_IN, 4096, timeout=200))
        reads += 1
        total_bytes += len(data)
        if i < 10:
            print(f"  Read {i}: {len(data)} bytes: {data[:40].hex()}")
        raw_samples.extend(data)
    except Exception as e:
        if i < 10:
            print(f"  Read {i}: {e}")

elapsed = time.time() - t0
print(f"\nRead {total_bytes} bytes in {reads} reads over {elapsed:.2f}s")
print(f"Data rate: {total_bytes/elapsed:.0f} bytes/s = {total_bytes/elapsed:.0f} samples/s")

# Analyze the raw bytes
import numpy as np
if raw_samples:
    arr = np.array(raw_samples, dtype=np.uint8)
    print(f"\nRaw byte analysis:")
    print(f"  Total: {len(arr)} bytes")
    print(f"  Min: {arr.min()}, Max: {arr.max()}, Mean: {arr.mean():.1f}")
    print(f"  Zeros: {np.sum(arr == 0)}")
    print(f"  Value 0x7c (124): {np.sum(arr == 0x7c)}")
    print(f"  Value 0x7d (125): {np.sum(arr == 0x7d)}")
    print(f"  Value 0x80 (128): {np.sum(arr == 0x80)}")
    # Check for header bytes
    for v in [0x00, 0x01, 0x57, 0xa7]:
        count = np.sum(arr == v)
        if count > 0:
            print(f"  Value 0x{v:02x}: {count} occurrences")

# Stop streaming
stop_cmd = bytes([0x02, 0x0a, 0x00, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
scope.device.write(EP_CMD_OUT, stop_cmd.ljust(64, b'\x00'), timeout=1000)
stop_cmd2 = bytes([0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
scope.device.write(EP_CMD_OUT, stop_cmd2.ljust(64, b'\x00'), timeout=1000)
scope._flush_all_buffers()

print("\n=== Test 2: Try status polling during streaming ===")
time.sleep(0.5)

# Start streaming again
scope._flush_all_buffers()
scope.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
scope.device.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)
for _ in range(5):
    try:
        scope.device.read(EP_RESP_IN, 64, timeout=50)
    except:
        break

scope.device.write(EP_CMD_OUT, trigger.ljust(64, b'\x00'), timeout=1000)

# Try polling status to see if the device acts differently
total_data = 0
for i in range(30):
    # Try reading status
    try:
        poll_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')
        scope.device.write(EP_CMD_OUT, poll_cmd, timeout=100)
        resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=100))
        if i < 5:
            print(f"  Status poll {i}: 0x{resp[0]:02x}")
    except:
        pass

    # Try reading data
    try:
        data = bytes(scope.device.read(EP_DATA_IN, 4096, timeout=100))
        total_data += len(data)
        if i < 5:
            print(f"  Data read {i}: {len(data)} bytes")
    except:
        pass

print(f"  Total data with polling: {total_data} bytes")

# Stop
scope.device.write(EP_CMD_OUT, stop_cmd.ljust(64, b'\x00'), timeout=1000)
scope.device.write(EP_CMD_OUT, stop_cmd2.ljust(64, b'\x00'), timeout=1000)

# Test 3: Try block-trigger approach during streaming
print("\n=== Test 3: Block trigger during streaming mode ===")
time.sleep(0.5)
scope._flush_all_buffers()
scope.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
scope.device.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)
for _ in range(5):
    try:
        scope.device.read(EP_RESP_IN, 64, timeout=50)
    except:
        break

# Send BLOCK trigger instead of streaming trigger
block_trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
scope.device.write(EP_CMD_OUT, block_trigger.ljust(64, b'\x00'), timeout=1000)

total_data = 0
for i in range(10):
    try:
        data = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=500))
        total_data += len(data)
        print(f"  Block read {i}: {len(data)} bytes, first 20: {data[:20].hex()}")
    except Exception as e:
        print(f"  Block read {i}: {e}")

print(f"  Total data with block trigger: {total_data} bytes")

scope.device.write(EP_CMD_OUT, stop_cmd.ljust(64, b'\x00'), timeout=1000)
scope.device.write(EP_CMD_OUT, stop_cmd2.ljust(64, b'\x00'), timeout=1000)
scope._flush_all_buffers()

scope.close()
print("\nDone!")
