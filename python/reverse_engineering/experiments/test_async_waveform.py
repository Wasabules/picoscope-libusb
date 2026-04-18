#!/usr/bin/env python3
"""Test: send channel setup + waveform upload in quick succession.

Hypothesis: the FX2 expects waveform data on EP 0x06 immediately after
the channel setup command on EP 0x01 (within a tight timing window).
The SDK uses async API so both are submitted nearly simultaneously.
"""
import ctypes
from ctypes import c_int16, c_int32, POINTER, byref
import time
import usb.core
import usb.util

EP_CMD_OUT = 0x01
EP_RESP_IN = 0x81
EP_FW_OUT = 0x06

# SDK init
lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")
print("SDK init...", flush=True)
handle = lib.ps2000_open_unit()
if handle <= 0:
    print("FAILED!")
    exit(1)
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
lib.ps2000_close_unit(c_int16(handle))
time.sleep(1.0)
print("SDK done.\n", flush=True)

# Raw libusb
dev = usb.core.find(idVendor=0x0ce9, idProduct=0x1007)
if dev is None:
    exit(1)
try:
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
except:
    pass
dev.set_configuration()
usb.util.claim_interface(dev, 0)
print(f"Connected.\n", flush=True)

waveform = bytes([0xee, 0x07] * 4096)

channel_a_setup = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
]).ljust(64, b'\x00')

# Test 1: Direct waveform (no channel setup)
print("Test 1: Direct waveform write (no channel setup first)")
try:
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("  OK!\n")
except Exception as e:
    print(f"  FAILED: {e}\n")

# Test 2: Channel setup, NO ACK READ, immediate waveform
print("Test 2: Channel setup + immediate waveform (no ACK read)")
try:
    dev.write(EP_CMD_OUT, channel_a_setup, timeout=1000)
    # NO sleep, NO ACK read — write waveform immediately
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("  OK!")
    # Now read the ACK
    try:
        resp = dev.read(EP_RESP_IN, 64, timeout=500)
        print(f"  ACK: {bytes(resp).hex()}")
    except:
        print("  No ACK")
    print()
except Exception as e:
    print(f"  FAILED: {e}\n")

# Test 3: Channel setup, 10ms sleep, waveform
print("Test 3: Channel setup + 10ms sleep + waveform")
try:
    dev.write(EP_CMD_OUT, channel_a_setup, timeout=1000)
    time.sleep(0.01)
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("  OK!\n")
except Exception as e:
    print(f"  FAILED: {e}\n")

# Test 4: Channel setup, ACK read, waveform
print("Test 4: Channel setup + ACK read + waveform")
try:
    dev.write(EP_CMD_OUT, channel_a_setup, timeout=1000)
    time.sleep(0.03)
    try:
        resp = dev.read(EP_RESP_IN, 64, timeout=500)
        print(f"  ACK: {bytes(resp).hex()}")
    except:
        print("  No ACK")
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("  OK!\n")
except Exception as e:
    print(f"  FAILED: {e}\n")

# Test 5: Just the 85 04 9b part, then waveform
print("Test 5: Only 85 04 9b command (without 85 21), then waveform")
try:
    small_cmd = bytes([0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00]).ljust(64, b'\x00')
    dev.write(EP_CMD_OUT, small_cmd, timeout=1000)
    time.sleep(0.02)
    try:
        dev.read(EP_RESP_IN, 64, timeout=500)
    except:
        pass
    dev.write(EP_FW_OUT, waveform, timeout=3000)
    print("  OK!\n")
except Exception as e:
    print(f"  FAILED: {e}\n")

usb.util.release_interface(dev, 0)
print("Done.")
