#!/usr/bin/env python3
"""Benchmark: minimal block capture cycle time."""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
from picoscope_libusb_full import *
import time

scope = PicoScopeFull()
scope.open()
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)

# Pre-build commands once
timebase = 5
block_size = 8064  # Max for single channel
cmd1 = scope._build_capture_cmd1(block_size, Channel.A, timebase)
cmd2 = scope._build_capture_cmd2()
trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]).ljust(64, b'\x00')

print(f"Block size: {block_size}, Timebase: {timebase}")
print(f"Expected capture time: {block_size * 10 * (1 << timebase) / 1e6:.2f}ms")

# Test 1: Full capture_block() for reference
print("\n=== Test 1: capture_block() (reference) ===")
times = []
for i in range(10):
    t0 = time.time()
    data = scope.capture_block(block_size)
    dt = (time.time() - t0) * 1000
    times.append(dt)
    if data and 'A' in data:
        n = len(data['A'])
    else:
        n = 0
    if i < 3:
        print(f"  Block {i}: {dt:.1f}ms, {n} samples")

avg = sum(times) / len(times)
print(f"  Average: {avg:.1f}ms/block → {block_size / (avg/1000):.0f} S/s effective")

# Test 2: Fast cycle - no flush between blocks, minimal sleeps
print("\n=== Test 2: Fast block cycle (no flush, minimal sleep) ===")
scope._flush_all_buffers()  # One flush at start

times = []
total_samples = 0
for i in range(20):
    t0 = time.time()

    # Send commands immediately
    scope.device.write(EP_CMD_OUT, cmd1, timeout=1000)
    scope.device.write(EP_CMD_OUT, cmd2, timeout=1000)

    # Quick ACK flush (short timeout)
    for _ in range(2):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=20)
        except:
            break

    # Poll status aggressively
    status = 0
    poll_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')
    for _ in range(200):
        scope.device.write(EP_CMD_OUT, poll_cmd, timeout=500)
        try:
            resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=50))
            if resp and resp[0] == 0x3b:
                status = 0x3b
                break
            elif resp and resp[0] == 0x7b:
                status = 0x7b
                break
        except:
            pass
        time.sleep(0.001)  # 1ms between polls

    if status != 0x3b:
        print(f"  Block {i}: FAILED status=0x{status:02x}")
        scope._flush_all_buffers()
        continue

    # Trigger + read
    scope.device.write(EP_CMD_OUT, trigger_cmd, timeout=1000)
    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=2000))
    except:
        raw = b''

    dt = (time.time() - t0) * 1000
    times.append(dt)
    total_samples += len(raw)
    if i < 3:
        print(f"  Block {i}: {dt:.1f}ms, {len(raw)} bytes, status=0x{status:02x}")

if times:
    avg = sum(times) / len(times)
    sps = block_size / (avg / 1000)
    print(f"  Average: {avg:.1f}ms/block → {sps:.0f} S/s effective")
    print(f"  Total: {len(times)} blocks, {total_samples} bytes in {sum(times)/1000:.1f}s")

# Test 3: Even faster - no ACK reads, poll immediately
print("\n=== Test 3: Fastest cycle (skip ACK, immediate poll) ===")
scope._flush_all_buffers()

times = []
for i in range(20):
    t0 = time.time()

    scope.device.write(EP_CMD_OUT, cmd1, timeout=1000)
    scope.device.write(EP_CMD_OUT, cmd2, timeout=1000)

    # Skip ACK reads entirely, go straight to status poll
    status = 0
    for _ in range(500):
        scope.device.write(EP_CMD_OUT, poll_cmd, timeout=500)
        try:
            resp = bytes(scope.device.read(EP_RESP_IN, 64, timeout=20))
            if resp and len(resp) >= 1:
                if resp[0] == 0x3b:
                    status = 0x3b
                    break
                elif resp[0] == 0x7b:
                    status = 0x7b
                    break
        except:
            pass

    if status != 0x3b:
        print(f"  Block {i}: FAILED status=0x{status:02x}")
        scope._flush_all_buffers()
        time.sleep(0.1)
        continue

    scope.device.write(EP_CMD_OUT, trigger_cmd, timeout=1000)
    try:
        raw = bytes(scope.device.read(EP_DATA_IN, 16384, timeout=2000))
    except:
        raw = b''

    dt = (time.time() - t0) * 1000
    times.append(dt)
    if i < 3:
        print(f"  Block {i}: {dt:.1f}ms, {len(raw)} bytes")

if times:
    avg = sum(times) / len(times)
    sps = block_size / (avg / 1000)
    print(f"  Average: {avg:.1f}ms/block → {sps:.0f} S/s effective")

scope.close()
print("\nDone!")
