#!/usr/bin/env python3
"""Use SDK for init, then our capture commands for range testing.

This test:
1. SDK init + one capture (properly loads firmware, waveform table, etc.)
2. SDK close
3. Open device with raw libusb (no firmware upload)
4. Upload waveform table to EP 0x06 (should work after SDK init)
5. Test captures at different ranges with our compound commands
"""
import ctypes
from ctypes import c_int16, c_int32, POINTER, byref
import time
import usb.core
import usb.util
import numpy as np

EP_CMD_OUT = 0x01
EP_RESP_IN = 0x81
EP_DATA_IN = 0x82
EP_FW_OUT = 0x06

# ========================================
# Step 1: SDK init
# ========================================
lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")
print("=== Step 1: SDK Init ===", flush=True)
handle = lib.ps2000_open_unit()
print(f"Handle: {handle}", flush=True)
if handle <= 0:
    print("FAILED!")
    exit(1)

# Set channels and do one capture
lib.ps2000_set_channel(c_int16(handle), c_int16(0), c_int16(1), c_int16(1), c_int16(8))
lib.ps2000_set_channel(c_int16(handle), c_int16(1), c_int16(1), c_int16(1), c_int16(8))
time_indisposed = c_int32(0)
lib.ps2000_run_block(c_int16(handle), c_int16(200), c_int16(5), c_int16(1), byref(time_indisposed))
for _ in range(50):
    time.sleep(0.05)
    if lib.ps2000_ready(c_int16(handle)):
        break
buf_a = (c_int16 * 200)()
buf_b = (c_int16 * 200)()
overflow = c_int16(0)
lib.ps2000_get_values(c_int16(handle), ctypes.cast(buf_a, POINTER(c_int16)),
                      ctypes.cast(buf_b, POINTER(c_int16)),
                      None, None, byref(overflow), c_int32(200))
print(f"SDK capture OK: A[0]={buf_a[0]}", flush=True)

lib.ps2000_close_unit(c_int16(handle))
print("SDK closed.\n", flush=True)
time.sleep(1.0)

# ========================================
# Step 2: Raw libusb
# ========================================
print("=== Step 2: Raw Libusb ===", flush=True)
dev = usb.core.find(idVendor=0x0ce9, idProduct=0x1007)
if dev is None:
    print("Device not found!")
    exit(1)
try:
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
except:
    pass
dev.set_configuration()
usb.util.claim_interface(dev, 0)
print(f"Connected: Bus {dev.bus} Dev {dev.address}", flush=True)

# Upload waveform table
waveform = bytes([0xee, 0x07] * 4096)
try:
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("Waveform table uploaded OK!", flush=True)
except Exception as e:
    print(f"Waveform upload failed: {e}", flush=True)

# Send channel setup + priming (matching SDK init sequence)
# Channel A setup
cmd = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
]).ljust(64, b'\x00')
dev.write(EP_CMD_OUT, cmd, timeout=1000)
time.sleep(0.05)
try:
    dev.read(EP_RESP_IN, 64, timeout=500)
except:
    pass

# Upload waveform for channel A
try:
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("Waveform A uploaded OK!", flush=True)
except Exception as e:
    print(f"Waveform A upload failed: {e}", flush=True)

# Follow-up
dev.write(EP_CMD_OUT, bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01]).ljust(64, b'\x00'), timeout=1000)
time.sleep(0.02)
try:
    dev.read(EP_RESP_IN, 64, timeout=500)
except:
    pass


def flush():
    for _ in range(5):
        try:
            dev.read(EP_RESP_IN, 64, timeout=50)
        except:
            break
    for _ in range(3):
        try:
            dev.read(EP_DATA_IN, 16384, timeout=50)
        except:
            break


def poll_status(timeout_ms=5000):
    start = time.time()
    while (time.time() - start) * 1000 < timeout_ms:
        dev.write(EP_CMD_OUT, bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.01)
        try:
            resp = bytes(dev.read(EP_RESP_IN, 64, timeout=500))
            if resp and resp[0] in (0x3b, 0x7b):
                return resp[0]
        except:
            pass
        time.sleep(0.05)
    return 0


def raw_capture(byte50, byte51, byte52):
    flush()

    cmd1 = bytes([
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0xc8,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x57,
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x20,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, byte50, byte51, byte52,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
    ])
    cmd2 = bytes([
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x02,
        0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00
    ])

    dev.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)
    dev.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    for _ in range(3):
        try:
            dev.read(EP_RESP_IN, 64, timeout=100)
        except:
            break

    status = poll_status(5000)
    if status != 0x3b:
        return None

    trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
    dev.write(EP_CMD_OUT, trigger.ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.02)

    try:
        raw = bytes(dev.read(EP_DATA_IN, 16384, timeout=15000))
    except:
        return None

    all_samples = np.frombuffer(raw, dtype='<i2')
    if len(all_samples) > 1:
        buf = all_samples[1:]
        non_zero = buf[buf != 0]
        if len(non_zero) > 0:
            return non_zero
    return None


# ========================================
# Step 3: Range test with exact SDK bytes
# ========================================
print("\n=== Step 3: Range Test ===\n", flush=True)

tests = [
    ("A=50mV, B=5V",  0x23, 0xd4, 0xc0),
    ("A=100mV, B=5V", 0x23, 0xd4, 0xe0),
    ("A=500mV, B=5V", 0x23, 0xd4, 0x60),
    ("A=2V, B=5V",    0x23, 0xc4, 0xe0),
    ("A=5V, B=5V",    0x23, 0xc4, 0x40),
    ("A=20V, B=5V",   0x23, 0xc4, 0x20),
]

print(f"{'Config':>16s}  {'RawMean':>10s}  {'RawStd':>8s}  {'N':>5s}")
print("-" * 50)

results = []
for label, b50, b51, b52 in tests:
    data = raw_capture(b50, b51, b52)
    if data is not None:
        mean = np.mean(data)
        std = np.std(data)
        print(f"{label:>16s}  {mean:>10.1f}  {std:>8.1f}  {len(data):>5d}")
        results.append((label, mean))
    else:
        print(f"{label:>16s}  FAILED")
        results.append((label, 0))

print()
if len(results) >= 2:
    means = [r[1] for r in results if r[1] != 0]
    spread = max(means) - min(means) if means else 0
    if spread > 500:
        print(f"PGA WORKS! Raw ADC spread: {spread:.0f}")
    else:
        print(f"PGA NOT working. Spread: {spread:.0f}")

usb.util.release_interface(dev, 0)
