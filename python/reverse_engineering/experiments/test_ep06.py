#!/usr/bin/env python3
"""Debug EP 0x06: test if waveform upload works after FPGA init."""
import sys
import os
import time
import usb.core
import usb.util
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from picoscope_libusb_full import PicoScopeFull, EP_FW_OUT, EP_CMD_OUT, EP_RESP_IN

scope = PicoScopeFull()
scope.open()

print("\n=== EP 0x06 Waveform Upload Debug ===\n")

# Try clearing endpoint halt first
print("1. Trying to clear EP 0x06 halt...")
try:
    usb.util.clear_halt(scope.device, EP_FW_OUT)
    print("   Clear halt OK")
except Exception as e:
    print(f"   Clear halt failed: {e}")

# Try small write
print("2. Trying 64-byte write to EP 0x06...")
try:
    scope.device.write(EP_FW_OUT, bytes(64), timeout=2000)
    print("   64-byte write OK!")
except Exception as e:
    print(f"   64-byte write failed: {e}")

# Try medium write
print("3. Trying 8192-byte waveform write to EP 0x06...")
waveform = bytes([0xee, 0x07] * 4096)
try:
    scope.device.write(EP_FW_OUT, waveform, timeout=5000)
    print("   8192-byte write OK!")
except Exception as e:
    print(f"   8192-byte write failed: {e}")

# Maybe the channel setup needs to happen first to "unlock" EP 0x06?
print("\n4. Re-sending channel A setup...")
cmd1 = bytes([
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
    0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
    0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
]).ljust(64, b'\x00')
scope.device.write(EP_CMD_OUT, cmd1, timeout=1000)
time.sleep(0.05)
try:
    resp = scope.device.read(EP_RESP_IN, 64, timeout=500)
    print(f"   Channel setup response: {bytes(resp).hex()}")
except Exception as e:
    print(f"   No response: {e}")

# Try waveform again after channel setup
print("5. Trying waveform write AFTER channel setup...")
try:
    scope.device.write(EP_FW_OUT, waveform, timeout=5000)
    print("   Waveform write OK!")
except Exception as e:
    print(f"   Waveform write failed: {e}")

# Try clear halt again then write
print("\n6. Clear halt + retry...")
try:
    usb.util.clear_halt(scope.device, EP_FW_OUT)
    print("   Clear halt OK")
except:
    print("   Clear halt failed")

try:
    scope.device.write(EP_FW_OUT, waveform, timeout=5000)
    print("   Waveform write OK!")
except Exception as e:
    print(f"   Waveform write failed: {e}")

# Try as multiple smaller chunks
print("\n7. Trying as 4x 2048-byte chunks...")
for i in range(4):
    chunk = waveform[i*2048:(i+1)*2048]
    try:
        scope.device.write(EP_FW_OUT, chunk, timeout=2000)
        print(f"   Chunk {i+1}/4 OK")
    except Exception as e:
        print(f"   Chunk {i+1}/4 failed: {e}")
        break

scope.close()
