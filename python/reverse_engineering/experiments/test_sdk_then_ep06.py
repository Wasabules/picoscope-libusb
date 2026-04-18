#!/usr/bin/env python3
"""Use SDK to init, then test EP 0x06 with raw libusb.

If EP 0x06 works after SDK init, the issue is in our init sequence.
"""
import ctypes
from ctypes import c_int16, c_int32, POINTER, byref
import time
import usb.core
import usb.util

# Step 1: Use SDK to initialize device
lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")
print("Opening PicoScope via SDK...", flush=True)
handle = lib.ps2000_open_unit()
print(f"Handle: {handle}", flush=True)
if handle <= 0:
    print("FAILED!")
    exit(1)
time.sleep(0.5)

# Do one capture so SDK completes init
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
print(f"SDK capture OK: A[0]={buf_a[0]}, B[0]={buf_b[0]}", flush=True)

# Close SDK (releases USB)
lib.ps2000_close_unit(c_int16(handle))
print("SDK closed.", flush=True)
time.sleep(1.0)

# Step 2: Now grab device with raw libusb
print("\nOpening with raw libusb...", flush=True)
dev = usb.core.find(idVendor=0x0ce9, idProduct=0x1007)
if dev is None:
    print("Device not found!")
    exit(1)

print(f"Found: Bus {dev.bus} Dev {dev.address}", flush=True)

try:
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
except:
    pass

dev.set_configuration()
usb.util.claim_interface(dev, 0)

# Test EP 0x06 write
waveform = bytes([0xee, 0x07] * 4096)
print("\nTesting EP 0x06 writes:", flush=True)

print("  Direct write (no prep)...", flush=True)
try:
    dev.write(0x06, waveform, timeout=3000)
    print("  OK!")
except Exception as e:
    print(f"  FAILED: {e}")

# Try with upload init command
print("  With 04 00 95 02 00 prep...", flush=True)
try:
    dev.write(0x01, bytes([0x04, 0x00, 0x95, 0x02, 0x00]), timeout=1000)
    time.sleep(0.05)
    dev.write(0x06, waveform, timeout=3000)
    print("  OK!")
except Exception as e:
    print(f"  FAILED: {e}")

# Try channel setup then waveform
print("  With channel setup command...", flush=True)
try:
    cmd = bytes([
        0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
        0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
        0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
    ]).ljust(64, b'\x00')
    dev.write(0x01, cmd, timeout=1000)
    time.sleep(0.05)
    dev.write(0x06, waveform, timeout=3000)
    print("  OK!")
except Exception as e:
    print(f"  FAILED: {e}")

usb.util.release_interface(dev, 0)
print("Done.", flush=True)
