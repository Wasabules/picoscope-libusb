#!/usr/bin/env python3
"""Test: After SDK init, use channel setup WITHOUT 87 06 sub-command.

If the waveform table from SDK init is still in device memory,
we don't need to re-upload it. The 87 06 sub-command is what
stalls EP 0x06 — so skip it entirely.

Also tests: channel setup WITH 87 06 but waveform sent via threads
(simulating SDK's async transfer behavior).
"""
import time
import threading
import ctypes
from ctypes import c_int16, c_int32, POINTER, byref
import usb.core
import usb.util
import numpy as np

EP_CMD_OUT = 0x01
EP_RESP_IN = 0x81
EP_DATA_IN = 0x82
EP_FW_OUT = 0x06


def sdk_init():
    """Full SDK init + one capture + close."""
    lib = ctypes.CDLL("/opt/picoscope/lib/libps2000.so")
    handle = lib.ps2000_open_unit()
    if handle <= 0:
        print("SDK open failed!")
        return False
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
    return True


def raw_open():
    dev = usb.core.find(idVendor=0x0ce9, idProduct=0x1007)
    if dev is None:
        return None
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except:
        pass
    dev.set_configuration()
    usb.util.claim_interface(dev, 0)
    return dev


def flush(dev):
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


def do_capture(dev, label):
    """Send exact SDK capture commands (packets 47-52) and read data."""
    flush(dev)

    # Packet [0047] - compound cmd1
    cmd1 = bytes([
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0xc8,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x57,
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x20,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x23, 0xd4, 0xc0,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
    ]).ljust(64, b'\x00')

    # Packet [0048] - compound cmd2
    cmd2 = bytes([
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x02,
        0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    ]).ljust(64, b'\x00')

    # Packet [0049] - status poll
    status_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')

    # Send all 3 commands without reading ACKs in between (matching SDK async behavior)
    dev.write(EP_CMD_OUT, cmd1, timeout=1000)
    dev.write(EP_CMD_OUT, cmd2, timeout=1000)
    dev.write(EP_CMD_OUT, status_cmd, timeout=1000)

    # Now read response (SDK reads just 1 byte: 0x3b)
    time.sleep(0.02)
    try:
        resp = bytes(dev.read(EP_RESP_IN, 64, timeout=2000))
        status = resp[0] if resp else 0
    except:
        status = 0

    if status == 0x3b:
        # Data ready - send read command
        read_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]).ljust(64, b'\x00')
        dev.write(EP_CMD_OUT, read_cmd, timeout=1000)
        try:
            raw = bytes(dev.read(EP_DATA_IN, 16384, timeout=5000))
            samples = np.frombuffer(raw, dtype='<i2')
            if len(samples) > 1:
                data = samples[1:]
                non_zero = data[data != 0]
                if len(non_zero) > 0:
                    print(f"  {label}: OK! mean={np.mean(non_zero):.1f} std={np.std(non_zero):.1f} n={len(non_zero)}")
                    return True
                else:
                    print(f"  {label}: All zeros (n={len(data)})")
                    return False
        except Exception as e:
            print(f"  {label}: Data read error: {e}")
            return False
    elif status == 0x33:
        # Not ready yet, try polling
        for _ in range(50):
            time.sleep(0.05)
            dev.write(EP_CMD_OUT, status_cmd, timeout=1000)
            try:
                resp = bytes(dev.read(EP_RESP_IN, 64, timeout=500))
                if resp and resp[0] == 0x3b:
                    read_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]).ljust(64, b'\x00')
                    dev.write(EP_CMD_OUT, read_cmd, timeout=1000)
                    raw = bytes(dev.read(EP_DATA_IN, 16384, timeout=5000))
                    samples = np.frombuffer(raw, dtype='<i2')
                    if len(samples) > 1:
                        data = samples[1:]
                        non_zero = data[data != 0]
                        if len(non_zero) > 0:
                            print(f"  {label}: OK (polled)! mean={np.mean(non_zero):.1f} n={len(non_zero)}")
                            return True
                    print(f"  {label}: Data all zeros after poll")
                    return False
                elif resp and resp[0] == 0x7b:
                    print(f"  {label}: FAILED (0x7b during poll)")
                    return False
            except:
                pass
        print(f"  {label}: FAILED (poll timeout)")
        return False
    else:
        print(f"  {label}: FAILED (status=0x{status:02x})")
        return False


# ========================================
# MAIN
# ========================================

print("=== SDK Init ===")
if not sdk_init():
    exit(1)
print("SDK done.\n")

dev = raw_open()
if dev is None:
    print("Device not found!")
    exit(1)
print(f"Connected: Bus {dev.bus} Dev {dev.address}\n")

# Test 1: Capture immediately (no channel setup at all)
print("=== Test 1: Capture with NO channel setup (SDK state preserved?) ===")
do_capture(dev, "no-setup")

# Test 2: Priming commands (like SDK packets 41-46), then capture
print("\n=== Test 2: Priming + capture (no channel setup) ===")
# Send set_timebase (packet 41)
dev.write(EP_CMD_OUT, bytes([
    0x02, 0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
    0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x3f, 0x04, 0x40
]).ljust(64, b'\x00'), timeout=1000)
time.sleep(0.01)

# Send priming (packets 42-46 pattern)
for _ in range(3):
    dev.write(EP_CMD_OUT, bytes([
        0x02, 0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x33, 0x04, 0x40
    ]).ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)
    dev.write(EP_CMD_OUT, bytes([
        0x02, 0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x33, 0x04, 0x40,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x3f, 0x04, 0x40
    ]).ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)

# Drain any ACKs
for _ in range(10):
    try:
        dev.read(EP_RESP_IN, 64, timeout=50)
    except:
        break

do_capture(dev, "primed")

# Test 3: Channel setup WITHOUT 87 06 sub-command
print("\n=== Test 3: Channel setup WITHOUT 87 06, then capture ===")
ch_setup_no_87 = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    # No 87 06 sub-command - just pad with zeros
]).ljust(64, b'\x00')
dev.write(EP_CMD_OUT, ch_setup_no_87, timeout=1000)
time.sleep(0.02)
try:
    resp = bytes(dev.read(EP_RESP_IN, 64, timeout=500))
    print(f"  Ch setup ACK: {resp[0]:02x}")
except:
    print("  No ACK after ch setup")

# Follow-up command (packet 37)
dev.write(EP_CMD_OUT, bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01]).ljust(64, b'\x00'), timeout=1000)
time.sleep(0.01)

# Priming
dev.write(EP_CMD_OUT, bytes([
    0x02, 0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
    0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x3f, 0x04, 0x40
]).ljust(64, b'\x00'), timeout=1000)
time.sleep(0.01)

for _ in range(3):
    dev.write(EP_CMD_OUT, bytes([
        0x02, 0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x33, 0x04, 0x40
    ]).ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.005)

# Drain ACKs
for _ in range(10):
    try:
        dev.read(EP_RESP_IN, 64, timeout=50)
    except:
        break

do_capture(dev, "no-87-06")

# Test 4: Channel setup WITH 87 06 + THREADED waveform upload
print("\n=== Test 4: Channel setup WITH 87 06 + threaded waveform ===")
ch_setup_full = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
]).ljust(64, b'\x00')

waveform = bytes([0xee, 0x07] * 4096)
wf_result = [None]


def waveform_writer():
    try:
        dev.write(EP_FW_OUT, waveform, timeout=3000)
        wf_result[0] = "OK"
    except Exception as e:
        wf_result[0] = f"FAILED: {e}"


# Start waveform writer thread BEFORE sending channel setup
t = threading.Thread(target=waveform_writer)
t.start()
time.sleep(0.001)  # tiny delay to let thread start
dev.write(EP_CMD_OUT, ch_setup_full, timeout=1000)
t.join(timeout=5.0)
print(f"  Waveform (thread pre-start): {wf_result[0]}")

if wf_result[0] != "OK":
    # Try the other way: channel setup first, then thread
    wf_result[0] = None
    dev.write(EP_CMD_OUT, ch_setup_full, timeout=1000)
    t2 = threading.Thread(target=waveform_writer)
    t2.start()
    t2.join(timeout=5.0)
    print(f"  Waveform (thread post-cmd): {wf_result[0]}")

if wf_result[0] == "OK":
    time.sleep(0.02)
    try:
        dev.read(EP_RESP_IN, 64, timeout=500)
    except:
        pass
    # Follow-up + priming
    dev.write(EP_CMD_OUT, bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01]).ljust(64, b'\x00'), timeout=1000)
    time.sleep(0.01)
    dev.write(EP_CMD_OUT, bytes([
        0x02, 0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x3f, 0x04, 0x40
    ]).ljust(64, b'\x00'), timeout=1000)
    for _ in range(3):
        dev.write(EP_CMD_OUT, bytes([
            0x02, 0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x33, 0x04, 0x40
        ]).ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.005)
    for _ in range(10):
        try:
            dev.read(EP_RESP_IN, 64, timeout=50)
        except:
            break
    do_capture(dev, "threaded-wf")

usb.util.release_interface(dev, 0)
print("\nDone.")
