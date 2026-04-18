#!/usr/bin/env python3
"""Test PGA with FULL firmware upload (including waveform table) and no waveform during channel setup."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from picoscope_libusb_full import (PicoScopeFull, EP_CMD_OUT, EP_RESP_IN,
                                   EP_DATA_IN, EP_FW_OUT, Range, Channel,
                                   Coupling, RANGE_MV, ADC_MAX)
import numpy as np

scope = PicoScopeFull()
scope.force_full_init = True

# Monkey-patch: upload FULL firmware (no trim)
original_upload_fpga = scope._upload_fpga_firmware
def upload_fpga_full():
    """Upload FULL FPGA firmware including waveform table."""
    print("  [FPGA] Loading firmware (FULL, no trim)...")
    firmware = scope._load_fpga_firmware()
    print(f"  [FPGA] Size: {len(firmware)} bytes (including waveform table)")

    print("  [FPGA] Sending upload command...")
    scope._send_raw(EP_CMD_OUT, bytes([0x04, 0x00, 0x95, 0x02, 0x00]))
    time.sleep(0.05)

    chunk_size = 32768
    offset = 0
    print("  [FPGA] Uploading...", end='', flush=True)
    while offset < len(firmware):
        chunk = firmware[offset:offset + chunk_size]
        scope.device.write(EP_FW_OUT, chunk, timeout=10000)
        offset += chunk_size
        print(".", end='', flush=True)
    print(" Done!")
    time.sleep(0.1)

scope._upload_fpga_firmware = upload_fpga_full

# Monkey-patch: channel setup without waveform uploads (no 87 06)
def setup_channels_no_wf():
    """Channel setup without waveform uploads or 87 06."""
    # Channel A setup (no 87 06, no waveform)
    cmd_a = bytes([
        0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
        0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    ])
    scope._send_cmd(cmd_a, read_response=False)
    scope._send_cmd(bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01]),
                    read_response=False)

    # Channel B setup (no 87 06, no waveform)
    cmd_b = bytes([
        0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
        0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x00, 0x00,
        0x59, 0x02, 0xdc, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
    ])
    scope._send_cmd(cmd_b, read_response=False)
    scope._send_cmd(bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01]),
                    read_response=False)

    time.sleep(0.1)
    for _ in range(10):
        try:
            scope.device.read(EP_RESP_IN, 64, timeout=100)
        except:
            break

    resp = scope._send_cmd(bytes([0x02, 0x01, 0x01, 0x80]))
    if resp:
        print(f"    Status: 0x{resp[0]:02x}")

scope._setup_channels = setup_channels_no_wf
scope.open()

# PGA test
print("\n=== PGA Test (full firmware, no channel waveforms) ===\n")
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
