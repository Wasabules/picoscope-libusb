#!/usr/bin/env python3
"""Debug EP 0x06: try various methods to reset the endpoint after FPGA upload."""
import sys
import os
import time
import usb.core
import usb.util
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from picoscope_libusb_full import PicoScopeFull, EP_FW_OUT, EP_CMD_OUT, EP_RESP_IN

scope = PicoScopeFull()
scope.open()

waveform = bytes([0xee, 0x07] * 4096)

print("\n=== EP 0x06 Reset Methods ===\n")

# Method 1: USB CLEAR_FEATURE ENDPOINT_HALT (standard USB control request)
print("1. CLEAR_FEATURE ENDPOINT_HALT on EP 0x06...")
try:
    # bmRequestType=0x02 (host-to-device, standard, endpoint)
    # bRequest=0x01 (CLEAR_FEATURE)
    # wValue=0x00 (ENDPOINT_HALT)
    # wIndex=EP_FW_OUT (0x06)
    scope.device.ctrl_transfer(0x02, 0x01, 0x00, EP_FW_OUT, timeout=1000)
    print("   Clear halt OK")
except Exception as e:
    print(f"   Failed: {e}")

time.sleep(0.1)
print("   Trying waveform write...")
try:
    scope.device.write(EP_FW_OUT, waveform, timeout=3000)
    print("   WAVEFORM WRITE OK!")
except Exception as e:
    print(f"   Still failed: {e}")

# Method 2: Set alternate interface (resets all endpoints)
print("\n2. Trying set_interface_altsetting...")
try:
    scope.device.set_interface_altsetting(0, 0)
    print("   Set alt OK")
    time.sleep(0.1)
    scope.device.write(EP_FW_OUT, waveform, timeout=3000)
    print("   WAVEFORM WRITE OK!")
except Exception as e:
    print(f"   Failed: {e}")

# Method 3: Send the FPGA upload init command again (might re-enable EP 0x06)
print("\n3. Re-sending FPGA upload init command...")
try:
    scope.device.write(EP_CMD_OUT, bytes([0x04, 0x00, 0x95, 0x02, 0x00]), timeout=1000)
    time.sleep(0.1)
    print("   Upload cmd sent, trying waveform write...")
    scope.device.write(EP_FW_OUT, waveform, timeout=3000)
    print("   WAVEFORM WRITE OK!")
except Exception as e:
    print(f"   Failed: {e}")

# Method 4: Try writing with different sizes
print("\n4. Trying different write sizes on EP 0x06...")
for size in [1, 8, 64, 512, 1024]:
    try:
        data = bytes([0xee, 0x07] * (size // 2)) if size > 1 else bytes([0xee])
        scope.device.write(EP_FW_OUT, data[:size], timeout=1000)
        print(f"   {size} bytes: OK!")
    except Exception as e:
        print(f"   {size} bytes: {e}")

# Method 5: Reset device USB (not full init)
print("\n5. USB device reset...")
try:
    scope.device.reset()
    time.sleep(1.0)
    # Reclaim interface
    if scope.device.is_kernel_driver_active(0):
        scope.device.detach_kernel_driver(0)
    usb.util.claim_interface(scope.device, 0)
    print("   Reset OK, trying waveform write...")
    scope.device.write(EP_FW_OUT, waveform, timeout=3000)
    print("   WAVEFORM WRITE OK!")
except Exception as e:
    print(f"   Failed: {e}")

print("\nDone.")
