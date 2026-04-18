#!/usr/bin/env python3
"""Test channel setup with BOTH waveform tables using libusb1 async transfers.

Uses python-libusb1 (usb1 module) for true async USB transfer support,
matching the SDK's libusb_submit_transfer pattern exactly.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import (PicoScopeFull, EP_CMD_OUT, EP_RESP_IN,
                                   EP_DATA_IN, Range, Channel, Coupling,
                                   RANGE_MV, ADC_MAX)
import usb1
import numpy as np

EP_FW_OUT = 0x06

# Do normal init (FX2, ADC, FPGA, post-FPGA) using PyUSB, skip channel setup
scope = PicoScopeFull()
scope.force_full_init = True
scope._setup_channels = lambda: print("    [Skipped - will use usb1]")
scope.open()

# Release PyUSB's claim on the interface
print("\n=== Releasing PyUSB, switching to usb1 ===\n")
import usb.util
usb.util.release_interface(scope.device, 0)

# Now use usb1 for async channel setup
ctx = usb1.USBContext()
handle = ctx.openByVendorIDAndProductID(0x0ce9, 0x1007)
if handle is None:
    print("ERROR: Could not open device with usb1")
    sys.exit(1)

try:
    if handle.kernelDriverActive(0):
        handle.detachKernelDriver(0)
except:
    pass
handle.claimInterface(0)

waveform_data = bytes([0xee, 0x07] * 4096)  # 8192 bytes

# Channel A setup (SDK packet [0035])
cmd_a = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
]).ljust(64, b'\x00')

# Channel B setup (SDK packet [0038])
cmd_b = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x00, 0x00,
    0x59, 0x02, 0xdc, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
]).ljust(64, b'\x00')

# Follow-up (SDK packets [0037] and [0040])
follow_up = bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01]).ljust(64, b'\x00')

# Submit all 6 transfers as async
results = {}

def make_callback(name):
    def callback(transfer):
        status = transfer.getStatus()
        actual = transfer.getActualLength()
        results[name] = (status, actual)
        status_name = {
            usb1.TRANSFER_COMPLETED: "OK",
            usb1.TRANSFER_TIMED_OUT: "TIMEOUT",
            usb1.TRANSFER_ERROR: "ERROR",
            usb1.TRANSFER_CANCELLED: "CANCELLED",
            usb1.TRANSFER_STALL: "STALL",
            usb1.TRANSFER_NO_DEVICE: "NO_DEVICE",
            usb1.TRANSFER_OVERFLOW: "OVERFLOW",
        }.get(status, f"UNKNOWN({status})")
        print(f"  {name}: {status_name} ({actual} bytes)")
    return callback

print("Submitting 6 async transfers (SDK pattern)...")

# Create and submit in SDK order
t1 = handle.getTransfer()
t1.setBulk(EP_CMD_OUT, cmd_a, callback=make_callback("1_cmd_a"), timeout=5000)
t1.submit()

t2 = handle.getTransfer()
t2.setBulk(EP_FW_OUT, waveform_data, callback=make_callback("2_wf_a"), timeout=5000)
t2.submit()

t3 = handle.getTransfer()
t3.setBulk(EP_CMD_OUT, follow_up, callback=make_callback("3_follow_a"), timeout=5000)
t3.submit()

t4 = handle.getTransfer()
t4.setBulk(EP_CMD_OUT, cmd_b, callback=make_callback("4_cmd_b"), timeout=5000)
t4.submit()

t5 = handle.getTransfer()
t5.setBulk(EP_FW_OUT, waveform_data, callback=make_callback("5_wf_b"), timeout=5000)
t5.submit()

t6 = handle.getTransfer()
t6.setBulk(EP_CMD_OUT, follow_up, callback=make_callback("6_follow_b"), timeout=5000)
t6.submit()

# Process events until all transfers complete
start = time.time()
while len(results) < 6 and (time.time() - start) < 15:
    try:
        ctx.handleEventsTimeout(tv=1.0)
    except Exception as e:
        print(f"  Event error: {e}")
        break

elapsed = time.time() - start
print(f"\nCompleted {len(results)}/6 transfers in {elapsed:.1f}s")

# Drain ACKs
time.sleep(0.2)
for _ in range(10):
    try:
        handle.bulkRead(0x81, 64, timeout=100)
    except:
        break

# Check device status
status_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')
handle.bulkWrite(EP_CMD_OUT, status_cmd, timeout=1000)
time.sleep(0.05)
try:
    resp = handle.bulkRead(0x81, 64, timeout=1000)
    print(f"Device status: 0x{resp[0]:02x}")
except Exception as e:
    print(f"Status read error: {e}")

# Release usb1 and reclaim with PyUSB
handle.releaseInterface(0)
handle.close()
ctx.close()

# Re-claim with PyUSB
scope.device.set_configuration()
usb.util.claim_interface(scope.device, 0)

# Do priming
print("\n=== Priming ===")
scope.set_timebase(5, 1000)

# PGA test
print("\n=== PGA Test ===\n")
for range_val, name in [(Range.R_50MV, "50mV"), (Range.R_500MV, "500mV"),
                        (Range.R_5V, "5V"), (Range.R_20V, "20V")]:
    scope.set_channel(Channel.A, True, Coupling.DC, range_val)
    scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)
    scope.set_timebase(5, 1000)
    result = scope.capture_block(1000)
    if result and 'A' in result and result['A'] is not None:
        scaled = result['A']
        range_mv = RANGE_MV.get(range_val, 5000)
        raw = scaled * ADC_MAX / range_mv
        print(f"  {name:>5s}: raw_mean={np.mean(raw):.0f}")
    else:
        print(f"  {name:>5s}: FAILED")

scope.close()
