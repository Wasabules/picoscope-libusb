#!/usr/bin/env python3
"""
PicoScope 2204A - Driver libusb COMPLET
=======================================
Upload FX2 + FPGA firmware, séquence d'init complète.

Basé sur le reverse-engineering du protocole USB.
"""

import usb.core
import usb.util
import struct
import time
import threading
import numpy as np
import os
from typing import Optional, Dict, List, Tuple, Callable
from enum import IntEnum


# ============================================================================
# Constantes
# ============================================================================

PICO_VID = 0x0ce9
PICO_PID_2000 = 0x1007

# Endpoints bulk
EP_CMD_OUT = 0x01
EP_RESP_IN = 0x81
EP_DATA_IN = 0x82
EP_FW_OUT = 0x06

# Cypress FX2 vendor request
FX2_VENDOR_REQUEST = 0xA0
FX2_CPUCS_ADDR = 0xE600

TIMEOUT_MS = 5000
CMD_SIZE = 64
ADC_MAX = 32767  # SDK API range (-32767 to +32767) for compatibility
ADC_RESOLUTION = 8  # PS2204A has 8-bit ADC
ADC_CENTER = 128    # 8-bit mid-scale (unsigned)
ADC_HALF_RANGE = 128  # Half-range for 8-bit ADC (0-255 → ±128)
PS2000_MAX_VALUE = 32767  # Alias for GUI compatibility


# ============================================================================
# Enums
# ============================================================================

class Channel(IntEnum):
    A = 0
    B = 1
    C = 2
    D = 3
    EXTERNAL = 4
    NONE = 5


class TriggerDirection(IntEnum):
    """Direction du trigger"""
    RISING = 0
    FALLING = 1


class TimeUnit(IntEnum):
    """Unités de temps"""
    FS = 0
    PS = 1
    NS = 2
    US = 3
    MS = 4
    S = 5


class WaveType(IntEnum):
    """Types de forme d'onde"""
    SINE = 0
    SQUARE = 1
    TRIANGLE = 2
    RAMPUP = 3
    RAMPDOWN = 4
    DC_VOLTAGE = 5


class Range(IntEnum):
    R_10MV = 0
    R_20MV = 1
    R_50MV = 2
    R_100MV = 3
    R_200MV = 4
    R_500MV = 5
    R_1V = 6
    R_2V = 7
    R_5V = 8
    R_10V = 9
    R_20V = 10

# Effective ADC full-scale range in mV (empirically calibrated).
# Bank 0 ranges (50mV, 5V, 10V, 20V) calibrated using ADC delta ratio
# method with known DC input and 20V range as reference.
# Bank 1 ranges (100mV-2V) use nominal values.
# Note: 50mV and 5V share PGA (0,7,0) → same effective hardware range.
RANGE_MV = {
    Range.R_10MV: 10, Range.R_20MV: 20, Range.R_50MV: 3515,
    Range.R_100MV: 100, Range.R_200MV: 200, Range.R_500MV: 500,
    Range.R_1V: 1000, Range.R_2V: 2000, Range.R_5V: 3515,
    Range.R_10V: 9092, Range.R_20V: 20000
}

# Hardware gain encoding table for PGA/relay control.
# Each entry: (bank, selector, has_200mV_flag)
# bank: 1=high-sensitivity (100mV-2V), 0=low-sensitivity (50mV,5V-20V)
# selector: 3-bit PGA multiplexer setting within bank
# has_200mV_flag: extra bit for 500mV range hardware stage
# Verified against SDK USB trace (test_sdk_range_capture.py, 2026-02-10)
# Note: 50mV and 5V share the same PGA setting — 50mV uses digital scaling
_RANGE_HW_GAIN = {
    Range.R_50MV:  (0, 7, 0),
    Range.R_100MV: (1, 6, 0),
    Range.R_200MV: (1, 7, 0),
    Range.R_500MV: (1, 2, 1),
    Range.R_1V:    (1, 3, 0),
    Range.R_2V:    (1, 1, 0),
    Range.R_5V:    (0, 7, 0),
    Range.R_10V:   (0, 2, 0),
    Range.R_20V:   (0, 3, 0),
}

class Coupling(IntEnum):
    AC = 0
    DC = 1


# ============================================================================
# Driver
# ============================================================================

class PicoScopeFull:
    """Driver PicoScope 2204A complet avec FX2 + FPGA firmware."""

    def __init__(self, fx2_file: str = "fx2_firmware.txt", fpga_file: str = "fpga_firmware.bin",
                 force_full_init: bool = True):
        """
        Args:
            fx2_file: Path to FX2 firmware file
            fpga_file: Path to FPGA firmware file
            force_full_init: If True, always do full firmware upload.
                           Default is False to avoid unnecessary re-uploads.
                           Set to True if device is in bad state (status 0x7b).
        """
        self.device = None
        self.interface = 0
        self.fx2_file = fx2_file
        self.fpga_file = fpga_file
        self.force_full_init = force_full_init
        self._info = {}
        self._channels = {}
        self._range_a = Range.R_5V
        self._range_b = Range.R_5V
        self._current_samples = 1000
        self._current_timebase = 5
        self._capture_seq = 0  # Capture sequence counter (increments: 1,2,4,8...)
        self._initialized = False
        self._siggen_active = False
        self._siggen_freq = 0
        self._siggen_wave = WaveType.SINE
        # Streaming state
        self._streaming = False
        self._stream_thread = None
        self._stream_lock = threading.Lock()
        self._stream_buffer_a = None
        self._stream_buffer_b = None
        self._stream_write_pos = 0  # Monotonic write position (modulo capacity)
        self._stream_capacity = 0
        self._stream_callback = None
        self._stream_overflow = False
        self._stream_auto_stop = True
        self._stream_max_samples = 0
        # Streaming statistics
        self._stream_blocks = 0
        self._stream_samples_total = 0
        self._stream_start_time = 0.0
        self._stream_last_block_ms = 0.0

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *args):
        self.close()

    # ========================================================================
    # Firmware Detection
    # ========================================================================

    def _check_firmware_loaded(self) -> bool:
        """Check if FX2 firmware is already loaded by trying a simple bulk write.

        If firmware is loaded, bulk endpoints accept writes.
        If not, bulk write will fail with pipe error or stall.
        NOTE: We send a minimal command to avoid state corruption.
        """
        try:
            # Send a simple LED command (harmless, doesn't change state much)
            cmd = bytes([0x02, 0x01, 0x01, 0x00]).ljust(64, b'\x00')  # LED off
            self.device.write(EP_CMD_OUT, cmd, timeout=1000)
            # If write succeeded, firmware is loaded
            print("    FX2 loaded (bulk write OK)")
            # Flush any response
            try:
                self.device.read(EP_RESP_IN, 64, timeout=200)
            except:
                pass
            return True
        except usb.core.USBError as e:
            # Pipe error or stall = firmware not loaded
            if 'pipe' in str(e).lower() or e.errno == 32:
                return False
            print(f"    FX2 check error: {e}")
            return True
        except Exception:
            return False

    def _check_fpga_loaded(self) -> bool:
        """Check if FPGA is already configured.

        NOTE: Always return False to force full FPGA init sequence.
        Checking FPGA status before ADC init corrupts device state.
        """
        # Always upload FPGA to ensure clean state
        print("    Will upload FPGA (clean init)")
        return False

    # ========================================================================
    # FX2 Firmware
    # ========================================================================

    def _load_fx2_chunks(self) -> List[Tuple[int, bytes]]:
        """Charge les chunks de firmware FX2."""
        chunks = []
        path = os.path.join(os.path.dirname(__file__), self.fx2_file)

        if not os.path.exists(path):
            raise RuntimeError(f"FX2 firmware not found: {path}")

        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split()
                if len(parts) >= 3:
                    addr = int(parts[0], 16)
                    data = bytes.fromhex(parts[2])
                    chunks.append((addr, data))

        return chunks

    def _fx2_write_ram(self, addr: int, data: bytes):
        """Écrit dans la RAM FX2."""
        self.device.ctrl_transfer(
            bmRequestType=0x40,
            bRequest=FX2_VENDOR_REQUEST,
            wValue=addr,
            wIndex=0,
            data_or_wLength=data,
            timeout=TIMEOUT_MS
        )

    def _upload_fx2_firmware(self):
        """Upload le firmware FX2."""
        print("  [FX2] Loading firmware...")
        chunks = self._load_fx2_chunks()

        # Reset device to clear any residual state
        print("  [FX2] Resetting device...")
        try:
            self.device.reset()
            time.sleep(0.5)
        except Exception as e:
            print(f"  [FX2] Reset warning: {e}")

        print("  [FX2] Halting CPU...")
        self._fx2_write_ram(FX2_CPUCS_ADDR, bytes([0x01]))
        time.sleep(0.01)

        print(f"  [FX2] Uploading {len(chunks)} chunks...")
        for addr, data in chunks:
            if addr == FX2_CPUCS_ADDR:
                continue
            self._fx2_write_ram(addr, data)
            time.sleep(0.001)

        # Remember old address
        old_address = self.device.address

        print("  [FX2] Starting CPU...")
        self._fx2_write_ram(FX2_CPUCS_ADDR, bytes([0x00]))

        # Le device se réénumère après le démarrage du nouveau firmware
        print("  [FX2] Waiting for device re-enumeration...")

        # Release old handle immediately
        try:
            usb.util.release_interface(self.device, self.interface)
        except:
            pass
        try:
            usb.util.dispose_resources(self.device)
        except:
            pass
        self.device = None

        # Wait for device to disappear (re-enumeration in progress)
        time.sleep(0.5)

        # Wait for device to reappear, possibly with new address
        print("  [FX2] Re-connecting to device...")
        new_device = None
        for attempt in range(20):
            time.sleep(0.3)
            # Find any PicoScope device
            dev = usb.core.find(idVendor=PICO_VID, idProduct=PICO_PID_2000)
            if dev is not None:
                new_device = dev
                if dev.address != old_address:
                    print(f"    Found: Bus {dev.bus} Device {dev.address} (changed from {old_address})")
                    break
                else:
                    # Same address - might be OK if re-enumeration was quick
                    if attempt >= 3:  # Give it a few tries to change
                        print(f"    Found: Bus {dev.bus} Device {dev.address} (same address)")
                        break

        if new_device is None:
            raise RuntimeError("Device not found after FX2 firmware upload")

        self.device = new_device

        # Reconfigurer le device
        try:
            if self.device.is_kernel_driver_active(self.interface):
                self.device.detach_kernel_driver(self.interface)
        except:
            pass

        self.device.set_configuration()
        usb.util.claim_interface(self.device, self.interface)

    # ========================================================================
    # FPGA Firmware
    # ========================================================================

    def _load_fpga_firmware(self) -> bytes:
        """Charge le firmware FPGA."""
        path = os.path.join(os.path.dirname(__file__), self.fpga_file)

        if not os.path.exists(path):
            raise RuntimeError(f"FPGA firmware not found: {path}")

        with open(path, 'rb') as f:
            return f.read()

    def _upload_fpga_firmware(self):
        """Upload le firmware FPGA."""
        print("  [FPGA] Loading firmware...")
        firmware = self._load_fpga_firmware()
        print(f"  [FPGA] Size: {len(firmware)} bytes")

        # The firmware file may include a waveform table appended after the
        # FPGA bitstream (16384 bytes of ee07 pattern). Trim it to avoid
        # overflowing the FX2's FPGA upload handler and stalling EP 0x06.
        fpga_end = len(firmware)
        if len(firmware) > 169216:
            # Check if waveform table is appended (ee07 pattern at offset 169216)
            if firmware[169216:169220] == b'\xee\x07\xee\x07':
                fpga_end = 169216
                print(f"  [FPGA] Trimmed waveform table ({len(firmware) - fpga_end} bytes)")
        fpga_data = firmware[:fpga_end]

        # Commande spéciale avant upload FPGA
        print("  [FPGA] Sending upload command...")
        self._send_raw(EP_CMD_OUT, bytes([0x04, 0x00, 0x95, 0x02, 0x00]))
        time.sleep(0.05)

        # Upload en chunks de 32KB
        chunk_size = 32768
        offset = 0

        print("  [FPGA] Uploading...", end='', flush=True)
        while offset < len(fpga_data):
            chunk = fpga_data[offset:offset + chunk_size]
            self.device.write(EP_FW_OUT, chunk, timeout=TIMEOUT_MS * 2)
            offset += chunk_size
            print(".", end='', flush=True)

        print(" Done!")

        # FPGA configures during post-FPGA command exchange (no extra wait needed)
        time.sleep(0.1)

    # ========================================================================
    # Communication
    # ========================================================================

    def _send_cmd(self, data: bytes, read_response: bool = True) -> bytes:
        """Envoie une commande (padded à 64 bytes)."""
        cmd = data.ljust(CMD_SIZE, b'\x00')
        self.device.write(EP_CMD_OUT, cmd, timeout=TIMEOUT_MS)
        # Read response to prevent buffer overflow
        if read_response:
            time.sleep(0.03)  # Wait 30ms before reading (like standalone test)
            try:
                return bytes(self.device.read(EP_RESP_IN, 64, timeout=500))
            except:
                return b''
        return b''

    def _send_raw(self, ep: int, data: bytes) -> None:
        """Envoie des données brutes."""
        self.device.write(ep, data, timeout=TIMEOUT_MS)

    def _read_resp(self, size: int = CMD_SIZE) -> bytes:
        """Lit une réponse."""
        try:
            return bytes(self.device.read(EP_RESP_IN, size, timeout=TIMEOUT_MS))
        except usb.core.USBTimeoutError:
            return b''

    def _read_data(self, size: int) -> bytes:
        """Lit des données bulk."""
        try:
            return bytes(self.device.read(EP_DATA_IN, size, timeout=TIMEOUT_MS * 3))
        except usb.core.USBTimeoutError:
            return b''

    def _wait_ack(self) -> bool:
        """Attend un ACK."""
        resp = self._read_resp(1)
        return len(resp) == 1 and resp[0] == 0x01

    # ========================================================================
    # Connexion et initialisation
    # ========================================================================

    def open(self) -> bool:
        """Ouvre la connexion et initialise le device."""
        print("=" * 50)
        print("PicoScope 2204A - libusb Driver")
        print("=" * 50)

        # Trouver le device
        print("\n[1] Searching for device...")
        self.device = usb.core.find(idVendor=PICO_VID, idProduct=PICO_PID_2000)

        if self.device is None:
            raise RuntimeError("PicoScope 2204A not found")

        print(f"    Found: Bus {self.device.bus} Device {self.device.address}")

        # Détacher driver kernel
        try:
            if self.device.is_kernel_driver_active(self.interface):
                print("    Detaching kernel driver...")
                self.device.detach_kernel_driver(self.interface)
        except:
            pass

        # Configuration
        print("\n[2] Configuring USB...")
        try:
            self.device.set_configuration()
        except usb.core.USBError as e:
            # "Entity not found" means device needs FX2 firmware first,
            # or is already configured. Try to proceed.
            print(f"    set_configuration warning: {e}")
        usb.util.claim_interface(self.device, self.interface)

        # Check current device state
        # NOTE: Even if status is 0x3b, we do full init to ensure clean state
        # (data from previous session is stale, buffers need reset)
        print("\n[3] Checking firmware status...")
        fx2_loaded = self._check_firmware_loaded()
        if fx2_loaded and not self.force_full_init:
            print("    FX2 firmware loaded, skipping FX2 upload")
        else:
            print("  Force full init enabled - uploading FX2 firmware...")
            self._upload_fx2_firmware()
            # _upload_fx2_firmware already handles re-enumeration and reconnection

        # Init séquence 1
        print("\n[4] ADC initialization...")
        self._init_adc()

        # NOTE: Device info reading moved to AFTER FPGA config
        # Reading device info before FPGA interferes with FPGA configuration

        # Check if FPGA is already loaded
        print("\n[5] Checking FPGA status...")
        fpga_loaded = self._check_fpga_loaded()

        if not fpga_loaded or self.force_full_init:
            print("  Pre-FPGA configuration...")
            self._pre_fpga_config()
            print("  Uploading FPGA firmware...")
            self._upload_fpga_firmware()

            # Post-FPGA config
            print("\n[6] Post-FPGA configuration...")
            self._post_fpga_config()
        else:
            print("  FPGA already configured")
            # Still need to run post-FPGA config to ensure proper state
            print("\n[6] Running post-FPGA configuration...")
            self._post_fpga_config()

        # Setup channels after FPGA is configured
        print("\n[7] Setting up channels...")
        self._setup_channels()

        # Read device info AFTER FPGA config (doesn't interfere here)
        print("\n[8] Reading device info...")
        self._read_device_info()

        self._initialized = True
        print("\n" + "=" * 50)
        print("Device ready!")
        print("=" * 50)

        return True

    def close(self):
        """Ferme la connexion."""
        if self.device:
            try:
                usb.util.release_interface(self.device, self.interface)
                usb.util.dispose_resources(self.device)
            except:
                pass
            self.device = None

    def _init_adc(self):
        """Initialise l'ADC."""
        # Don't read responses for ADC init commands (like standalone test)
        self._send_cmd(bytes([
            0x02, 0x81, 0x03, 0x80, 0x08, 0x18,
            0x81, 0x03, 0xb2, 0xff, 0x18,
            0x81, 0x03, 0xb0, 0x00, 0xf8,
            0x81, 0x03, 0xb5, 0xff, 0xf8
        ]), read_response=False)
        time.sleep(0.05)

        self._send_cmd(bytes([
            0x02, 0x81, 0x03, 0xb0, 0xff, 0x80, 0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x81, 0x03, 0xb0, 0xff, 0x40, 0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x81, 0x03, 0xb0, 0xff, 0x20, 0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x81, 0x03, 0xb0, 0xff, 0x10, 0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x81, 0x03, 0xb0, 0xff, 0x08, 0x0c, 0x03, 0x0a, 0x00, 0x01
        ]), read_response=False)
        time.sleep(0.1)

        # Wait for ACK (0x01)
        try:
            resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=1000))
            if resp and resp[0] == 0x01:
                print("    ADC ACK received")
            else:
                print(f"    ADC response: {resp[:4].hex() if resp else 'empty'}")
        except Exception as e:
            print(f"    ADC ACK timeout: {e}")

    def _read_device_info(self):
        """Lit les informations du device."""
        for addr in [0x00, 0x40, 0x80, 0xc0]:
            # Don't auto-read response for these info commands
            self._send_cmd(bytes([0x02, 0x83, 0x02, 0x50, addr]), read_response=False)
            time.sleep(0.02)
            self._wait_ack()

            # The response from this command contains the device info
            resp = self._send_cmd(bytes([0x02, 0x03, 0x02, 0x50, 0x40]), read_response=True)
            time.sleep(0.02)

            if len(resp) >= 32 and addr == 0x00:
                try:
                    text = resp.decode('latin-1', errors='ignore')
                    if 'JO' in text:
                        idx = text.find('JO')
                        self._info['serial'] = text[idx:idx+10].strip('\x00')
                    for m in ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec']:
                        if m in text:
                            idx = text.find(m)
                            self._info['cal_date'] = text[idx-2:idx+5].strip('\x00')
                            break
                except:
                    pass

        self._info['variant'] = '2204A'
        print(f"    Serial: {self._info.get('serial', 'Unknown')}")
        print(f"    Cal Date: {self._info.get('cal_date', 'Unknown')}")

    def _pre_fpga_config(self):
        """Commands sent between EEPROM read and FPGA upload (SDK packets 19-24).

        These are critical for PGA initialization:
        1. FX2 register writes (0xe5 to addresses 0x07-0x0c)
        2. Single-byte reset command
        3. Initial PGA/range config (85 04 99 with default 20V range)
        4. ADC register config (81 03 b2 ff 08)
        """
        # Packet [0019]: Write 0xe5 to FX2 addresses 0x07-0x0c
        # 02 02 XX YY = write value YY to register XX
        self._send_cmd(bytes([
            0x02,
            0x02, 0x02, 0x07, 0xe5,
            0x02, 0x02, 0x08, 0xe5,
            0x02, 0x02, 0x09, 0xe5,
            0x02, 0x02, 0x0a, 0xe5,
            0x02, 0x02, 0x0b, 0xe5,
            0x02, 0x02, 0x0c, 0xe5,
        ]), read_response=False)
        time.sleep(0.05)

        # Read response (SDK gets 6 bytes: 26 5a d1 94 48 f8)
        try:
            resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=500))
            print(f"    Pre-FPGA reg response: {resp[:8].hex()}")
        except:
            pass

        # Packet [0021]: Single byte command (NOT padded to 64!)
        self.device.write(EP_CMD_OUT, bytes([0x01]), timeout=TIMEOUT_MS)
        time.sleep(0.05)

        # Read response (SDK gets 8 bytes: 01 01 00 00 00 00 00 00)
        try:
            resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=500))
            print(f"    Pre-FPGA status: {resp[:8].hex()}")
        except:
            pass

        # Packet [0023]: Initial PGA/range config (85 04 99 with 0x0a = 20V)
        self._send_cmd(bytes([
            0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a
        ]), read_response=False)

        # Packet [0024]: ADC register config
        self._send_cmd(bytes([
            0x02, 0x81, 0x03, 0xb2, 0xff, 0x08
        ]), read_response=False)
        time.sleep(0.05)

    def _post_fpga_config(self):
        """Configuration après upload FPGA - minimal comme standalone test."""
        # NO initial flush - go straight to config commands

        # Post-firmware config 1 (frame 23091 in capture)
        # Must NOT read response
        print("    Reading post-FPGA status...")
        self._send_cmd(bytes([
            0x02, 0x85, 0x04, 0x80, 0x00, 0x00, 0x00,
            0x81, 0x03, 0xb2, 0x00, 0x08
        ]), read_response=False)
        time.sleep(0.1)

        # Flush 3x with 300ms timeout - do all 3, don't break early
        for i in range(3):
            try:
                resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=300))
                if resp:
                    print(f"      Flush {i+1}: {resp[:8].hex()}")
            except usb.core.USBError as e:
                print(f"      Read failed: {e}")

        # Post-firmware config 2 (frame 23097) - triggers CAAC
        # Must NOT read response
        self._send_cmd(bytes([
            0x02, 0x85, 0x04, 0x80, 0x00, 0x00, 0x00,
            0x05, 0x04, 0x8f, 0x00, 0x10
        ]), read_response=False)
        time.sleep(0.3)

        # Read until CAAC (0xCAAC)
        caac_received = False
        for i in range(5):
            try:
                resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=500))
                print(f"      FPGA response: {resp[:4].hex() if len(resp) >= 4 else resp.hex()}")
                if len(resp) >= 2 and resp[:2] == b'\xca\xac':
                    caac_received = True
                    print("    CAAC received - FPGA configured!")
                    break
            except usb.core.USBError as e:
                print(f"      Read error: {e}")
                break

        if not caac_received:
            print("    Warning: No FPGA response")

    def _upload_waveform_table_async(self, label=""):
        """Upload the waveform DAC table (8192 bytes on EP 0x06) via a thread.

        The SDK uses async USB transfers to submit the waveform data on EP 0x06
        simultaneously with the channel setup command on EP 0x01. The 87 06
        sub-command in the channel setup tells the FX2 to accept waveform data,
        but this window only exists during/immediately after command processing.

        Returns a thread that's already started. Call thread.join() after
        sending the channel setup command.
        """
        waveform_data = bytes([0xee, 0x07] * 4096)  # 8192 bytes
        result = [None]

        def writer():
            try:
                self.device.write(EP_FW_OUT, waveform_data, timeout=5000)
                result[0] = True
            except usb.core.USBError as e:
                result[0] = False
                print(f"    Warning: waveform upload{' ' + label if label else ''}: {e}")

        t = threading.Thread(target=writer, daemon=True)
        t.start()
        return t, result

    def _setup_channels(self):
        """Configure les canaux après FPGA init.

        SDK sequence (from interceptor trace, all ASYNC):
        1. Channel A setup on EP 0x01 (85 21 compound with 87 06) [pkt 35]
        2. Waveform table on EP 0x06 (8192 bytes) - ASYNC with step 1 [pkt 36]
        3. Follow-up on EP 0x01 (85 05 82) [pkt 37]
        4. Channel B setup on EP 0x01 (85 21 compound with 87 06) [pkt 38]
        5. Waveform table on EP 0x06 (8192 bytes) - ASYNC with step 4 [pkt 39]
        6. Follow-up on EP 0x01 (85 05 82) [pkt 40]

        NO ACK reads between any of these (SDK reads first ACK much later).
        The 87 06 sub-command opens a timing window for EP 0x06 writes.
        We use threads to pre-submit the EP 0x06 write before the 87 06
        is processed, simulating the SDK's async USB transfer behavior.

        BOTH waveform tables must be uploaded — skipping channel B's waveform
        leaves the FPGA in a partial state that prevents PGA gain from working.
        """
        # EXACT SDK bytes for channel A setup (packet [0035])
        cmd_a = bytes([
            0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
            0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
            0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
            0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
        ])

        # EXACT SDK bytes for channel B setup (packet [0038])
        cmd_b = bytes([
            0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
            0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
            0x00, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x00, 0x00,
            0x59, 0x02, 0xdc, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
            0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
            0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
        ])

        # Follow-up command (SDK packets [0037] and [0040])
        follow_up = bytes([0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01])

        # --- Strategy: interleaved async pattern ---
        # We use a single thread that writes BOTH waveforms sequentially on
        # EP 0x06. The main thread sends EP 0x01 commands with ACK drains
        # between channels to prevent FX2 buffer backpressure (the FX2 can't
        # process new EP 0x01 commands if its EP 0x81 ACK buffer is full).
        waveform_data = bytes([0xee, 0x07] * 4096)  # 8192 bytes
        wf_results = [None, None]

        def dual_waveform_writer():
            """Write both waveform tables sequentially on EP 0x06."""
            try:
                self.device.write(EP_FW_OUT, waveform_data, timeout=5000)
                wf_results[0] = True
            except usb.core.USBError as e:
                wf_results[0] = False
                print(f"    Warning: waveform A upload: {e}")
                return
            try:
                self.device.write(EP_FW_OUT, waveform_data, timeout=5000)
                wf_results[1] = True
            except usb.core.USBError as e:
                wf_results[1] = False
                print(f"    Warning: waveform B upload: {e}")

        wf_thread = threading.Thread(target=dual_waveform_writer, daemon=True)
        wf_thread.start()
        time.sleep(0.001)  # Let thread submit first EP 0x06 transfer

        # Channel A: send setup command (87 06 triggers waveform A acceptance)
        self._send_cmd(cmd_a, read_response=False)       # [pkt 35]
        self._send_cmd(follow_up, read_response=False)    # [pkt 37]

        # Wait for waveform A to complete, then drain ACKs before channel B
        # This prevents FX2 EP 0x81 backpressure that blocks EP 0x01 commands
        time.sleep(0.1)
        for _ in range(5):
            try:
                self.device.read(EP_RESP_IN, 64, timeout=100)
            except usb.core.USBTimeoutError:
                break

        # Channel B: send setup command (87 06 triggers waveform B acceptance)
        self._send_cmd(cmd_b, read_response=False)        # [pkt 38]
        self._send_cmd(follow_up, read_response=False)    # [pkt 40]

        # Wait for both waveform uploads to complete
        wf_thread.join(timeout=12.0)
        if wf_results[0]:
            print("    Waveform table A uploaded OK")
        else:
            print("    Warning: Waveform table A upload failed")
        if wf_results[1]:
            print("    Waveform table B uploaded OK")
        else:
            print("    Warning: Waveform table B upload failed")

        # Follow-up B (no ACK read)
        self._send_cmd(follow_up, read_response=False)
        time.sleep(0.1)

        # NOW drain all pending ACKs (SDK reads first ACK much later)
        for _ in range(10):
            try:
                self.device.read(EP_RESP_IN, 64, timeout=100)
            except usb.core.USBTimeoutError:
                break

        # Check status
        resp = self._send_cmd(bytes([0x02, 0x01, 0x01, 0x80]))
        if resp:
            status = resp[0]
            print(f"    Channel setup status: 0x{status:02x}")
            if status == 0x3b:
                print("    Device ready for capture!")
            elif status == 0x33:
                print("    Device in capture pending state")
            elif status == 0x7b:
                print("    Warning: Overflow/error bit set (bit 6)")

    # ========================================================================
    # API publique
    # ========================================================================

    def get_info(self) -> Dict[str, str]:
        """Retourne les infos du device."""
        return self._info.copy()

    def get_all_info(self) -> Dict[str, str]:
        """Retourne toutes les infos du device (compatibilité GUI)."""
        return {
            'driver_version': 'libusb-1.0',
            'usb_version': '2.0',
            'hardware_version': '1',
            'variant': self._info.get('variant', '2204A'),
            'serial': self._info.get('serial', 'Unknown'),
            'calibration_date': self._info.get('cal_date', 'Unknown'),
        }

    def flash_led(self):
        """Fait clignoter la LED et retourne le status."""
        try:
            resp = self._send_cmd(bytes([0x02, 0x01, 0x01, 0x80]))
            if resp:
                status = resp[0]
                print(f"    LED OK, status: 0x{status:02x}")
            else:
                print("    LED OK, no response")
        except Exception as e:
            print(f"    LED command failed: {e}")
        time.sleep(0.1)

    def set_channel(self, channel: Channel, enabled: bool,
                    coupling: Coupling = Coupling.DC,
                    range_: Range = Range.R_5V):
        """Configure un canal.

        channel: Channel.A ou Channel.B
        enabled: True/False
        coupling: Coupling.DC ou Coupling.AC
        range_: Range.R_10MV à Range.R_20V

        Le byte canal encode: bit7=enabled, bits4-5=channel, bits0-3=range
        Le coupling est envoyé comme byte séparé après le channel byte.
        """
        if channel == Channel.A:
            self._range_a = range_
        else:
            self._range_b = range_

        self._channels[channel] = {
            'enabled': enabled,
            'coupling': coupling,
            'range': range_,
            'range_mv': RANGE_MV[range_]
        }

        # Channel byte: bit 7 = enabled, bits 4-5 = channel, bits 0-3 = range
        ch_byte = (0x80 if enabled else 0x00) | (int(channel) << 4) | (int(range_) & 0x0F)
        # Coupling: 0x01 = enabled (DC), 0x00 = disabled (AC)
        dc_byte = 0x01 if coupling == Coupling.DC else 0x00

        # Channel config is stored locally and applied via gain bytes in
        # the capture compound command (bytes 49-51 of cmd1).
        # The SDK does NOT re-send channel setup commands between captures -
        # it only changes gain bytes in the compound capture command.
        # IMPORTANT: Do NOT send 87 06 here - that tells the FPGA to expect
        # waveform data on EP 0x06, and without sending the data, the FPGA
        # state machine gets corrupted, disabling PGA gain control.

    def set_timebase(self, timebase: int, samples: int):
        """Configure la base de temps.

        timebase: 0-23 (0=10ns/100MSPS, 1=20ns, 2=40ns, 3+=80*(tb-2)ns)
        samples: number of samples to capture (max 8192)
        """
        samples = min(samples, 8192)
        self._current_samples = samples
        self._current_timebase = timebase

        # Priming mode: SDK uses 0x30 | (tb & 0x0F) for timebase byte
        if timebase <= 15:
            tb_byte = 0x30 | (timebase & 0x0F)
        else:
            tb_byte = 0x20 | (timebase & 0x0F)

        # Use current gain encoding for the priming command
        _, byte51, byte52 = self._encode_hw_gain_bytes()

        # SDK priming sequence (from trace packets 41-46):
        # 1. Set timebase with 0x3f (init), then alternate 0x33/0x3f
        # Positions 4-5 are always 0x14 0x00 (constant, NOT sample count)
        cmd = bytes([
            0x02,
            0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
            0x85, 0x07, 0x97, 0x00, 0x14, 0x00, tb_byte, byte51, byte52
        ])

        self._send_cmd(cmd)
        time.sleep(0.02)

    def disable_trigger(self):
        """Désactive le trigger (auto-trigger mode).

        Command from frame 49613: 02 07 06 00 40 00 00 02 01 00
        """
        self._send_cmd(bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]))
        time.sleep(0.02)

    def set_trigger(self, source: Channel = Channel.A, threshold_mv: float = 0.0,
                    direction: TriggerDirection = TriggerDirection.RISING,
                    delay_percent: float = 0, auto_trigger_ms: int = 1000):
        """Configure le trigger (compatible GUI API).

        source: Channel to trigger on
        threshold_mv: Trigger level in mV
        direction: TriggerDirection.RISING or FALLING
        delay_percent: Delay in percent (0-100) - not used in libusb driver
        auto_trigger_ms: Auto-trigger timeout in ms (0 = disabled/normal mode)
        """
        # If auto_trigger_ms is 0, we use normal trigger mode
        # Otherwise, we set up auto-trigger

        # Get range for the trigger channel
        range_mv = RANGE_MV.get(
            self._range_a if source == Channel.A else self._range_b, 5000)

        # Convert threshold to ADC value
        threshold_adc = int((threshold_mv / range_mv) * ADC_MAX)
        threshold_adc = max(-32768, min(32767, threshold_adc))

        # Trigger command format from captures
        th_hi = (threshold_adc >> 8) & 0xFF
        th_lo = threshold_adc & 0xFF

        # Direction as int (TriggerDirection is IntEnum)
        dir_val = int(direction)

        cmd = bytes([
            0x02, 0x07, 0x06,
            source,  # Trigger source channel
            0x40,    # Flags
            th_hi, th_lo,  # Threshold
            dir_val,  # Direction
            0x01,  # Enable
            0x00
        ])

        self._send_cmd(cmd)
        time.sleep(0.02)

    def get_timebase_info(self, timebase: int) -> Dict:
        """Retourne les informations sur une base de temps.

        Returns dict with sample_interval_ns and max_samples.
        PicoScope 2204A: 100 MSPS max = 10ns minimum interval.
        Formula: interval_ns = 10 * 2^tb (verified against SDK ps2000_get_timebase).
        Dual channel: tb=0 unavailable (single ADC alternates), max_samples halved.
        """
        interval_ns = 10 * (1 << timebase)

        ch_b_enabled = self._channels.get(Channel.B, {}).get('enabled', False)
        ch_a_enabled = self._channels.get(Channel.A, {}).get('enabled', True)
        both = ch_a_enabled and ch_b_enabled

        if both and timebase == 0:
            interval_ns = 0  # Unavailable for dual channel

        max_samples = 3968 if both else 8064

        return {
            'timebase': timebase,
            'sample_interval_ns': interval_ns,
            'sample_interval_us': interval_ns / 1000.0,
            'sample_rate_hz': 1e9 / interval_ns if interval_ns > 0 else 0,
            'max_samples': max_samples
        }

    @staticmethod
    def _interval_to_timebase(interval_ns: int) -> int:
        """Convertit un intervalle en nanosecondes vers l'index timebase le plus proche.

        Formula: interval = 10 * 2^tb, so tb = log2(interval/10).
        Capped à PS2200_MAX_TIMEBASE = 23.
        """
        import math
        if interval_ns <= 10:
            return 0
        tb = round(math.log2(interval_ns / 10.0))
        return min(23, max(0, tb))

    def stop(self):
        """Arrête l'acquisition en cours et vide les buffers."""
        # Config end command (observé dans capture_cmd2)
        try:
            self._send_cmd(bytes([0x02, 0x85, 0x04, 0x81, 0x00, 0x00, 0x00]))
        except usb.core.USBError:
            pass
        self._flush_all_buffers()
        # Arrêter le streaming si actif
        if hasattr(self, '_streaming') and self._streaming:
            self._streaming = False

    def capture_simple(self, samples: int = 1000) -> Optional[Dict]:
        """Capture simplifiée: trigger + read."""
        # Send trigger command
        self._send_cmd(bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00]))
        time.sleep(0.1)

        # Read data
        raw = self._read_data(16384)
        if len(raw) < 100:
            print(f"    capture_simple: only {len(raw)} bytes")
            return None

        # Parse data
        all_samples = np.frombuffer(raw, dtype='<i2')
        non_zero = np.count_nonzero(all_samples)
        print(f"    capture_simple: {len(raw)} bytes, {len(all_samples)} samples, {non_zero} non-zero")

        if non_zero > 1:
            nz_idx = np.where(all_samples != 0)[0]
            data_start = nz_idx[1] if nz_idx[0] == 0 else nz_idx[0]
            data = all_samples[data_start:nz_idx[-1]+1]
        else:
            data = all_samples[1:samples+1]

        # Convert to mV
        range_mv = RANGE_MV.get(self._range_a, 5000)
        data_mv = data.astype(float) * range_mv / 32767.0

        return {
            'samples': len(data_mv),
            'A': data_mv,
            'time_ns': np.arange(len(data_mv)) * 1000  # Placeholder
        }

    def test_read_raw(self):
        """Test: read data directly from EP 0x82 without capture commands."""
        print("\n    Testing direct read from EP 0x82...")

        # Check status first
        resp = self._send_cmd(bytes([0x02, 0x01, 0x01, 0x80]))
        if resp:
            print(f"    Status before read: 0x{resp[0]:02x}")

        # Try to read data
        try:
            raw = bytes(self.device.read(EP_DATA_IN, 16384, timeout=2000))
            print(f"    Got {len(raw)} bytes")
            if len(raw) >= 2:
                print(f"    First 20 bytes: {raw[:20].hex()}")

            # Parse as int16
            samples = np.frombuffer(raw, dtype='<i2')
            non_zero = np.count_nonzero(samples)
            print(f"    Total samples: {len(samples)}, Non-zero: {non_zero}")
            if non_zero > 0:
                nz_idx = np.where(samples != 0)[0]
                print(f"    Non-zero at indices: {nz_idx[:10]}...")
                print(f"    First non-zero values: {samples[nz_idx[:10]]}")
        except Exception as e:
            print(f"    Read error: {e}")

    def set_signal_generator(self, wave_type: WaveType = WaveType.SINE,
                             frequency_hz: float = 1000, amplitude_mv: int = 2000,
                             offset_mv: int = 0):
        """Configure le générateur de signal intégré.

        Protocole: opcode 0x85 0x0c 0x86
        freq_param = int(freq_hz * 0x400) encodé en little-endian 4 bytes
        Suivi de GET_DATA sub-command (0x85 0x05 0x87)

        wave_type: SINE, SQUARE, TRIANGLE, RAMPUP, RAMPDOWN, DC_VOLTAGE
        frequency_hz: 0 - 100000 Hz
        amplitude_mv: amplitude peak-to-peak en mV (non encodé dans cette commande)
        offset_mv: offset DC en mV (non encodé dans cette commande)
        """
        freq_param = int(frequency_hz * 0x400)

        cmd = bytes([
            0x02,
            0x85, 0x0c, 0x86,
            0x00,                                # flags
            (freq_param >> 0) & 0xFF,             # freq LSB
            (freq_param >> 8) & 0xFF,
            (freq_param >> 16) & 0xFF,
            (freq_param >> 24) & 0xFF,            # freq MSB
            int(wave_type) & 0xFF,                # waveform type
            0x00, 0x00, 0x00, 0x00,               # reserved
            0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00  # GET_DATA sub-command
        ])

        self._send_cmd(cmd)
        self._siggen_active = True
        self._siggen_freq = frequency_hz
        self._siggen_wave = wave_type
        time.sleep(0.05)

    def disable_signal_generator(self):
        """Désactive le générateur de signal (fréquence 0)."""
        self.set_signal_generator(WaveType.DC_VOLTAGE, 0, 0, 0)
        self._siggen_active = False

    def _poll_status(self, timeout_ms: int = 2000) -> int:
        """Poll device status until ready or timeout.

        Sends status poll command and reads response separately (like USB capture).
        Returns: status byte (0x33=pending, 0x3b=ready, 0x7b=error)
        """
        start = time.time()
        timeout_s = timeout_ms / 1000.0
        last_status = 0x00

        while (time.time() - start) < timeout_s:
            # Send poll command (from frame 49551: 02 01 01 80 00...)
            # Don't auto-read response - do it separately
            cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')
            self.device.write(EP_CMD_OUT, cmd, timeout=1000)

            # Read response separately with short timeout
            try:
                resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=200))
                if resp and len(resp) >= 1:
                    last_status = resp[0]
                    if last_status == 0x3b:  # Data ready
                        return last_status
                    elif last_status == 0x7b:  # Error state
                        return last_status
                    # 0x33 = pending, continue polling
            except usb.core.USBTimeoutError:
                pass

            time.sleep(0.02)  # Small delay between polls

        return last_status

    @staticmethod
    def _get_timebase_chan_bytes(timebase: int) -> Tuple[int, int]:
        """Return the channel config bytes for the 85 08 93 sub-command.

        These bytes vary per timebase. Values observed from SDK USB trace.
        The buffer sub-command (85 08 89) uses 2^timebase, computed inline.
        """
        # Observed from SDK trace: channel config bytes per timebase
        _TB_CHAN = {
            0: (0x27, 0x2f),
            1: (0x13, 0xa7),
            2: (0x09, 0xe3),
            3: (0x05, 0x01),
            4: (0x02, 0x90),  # Interpolated (midpoint of tb=3 and tb=5)
            5: (0x01, 0x57),
            6: (0x00, 0xbb),  # Interpolated
            7: (0x00, 0x6d),  # Interpolated
            8: (0x00, 0x46),  # Interpolated
            9: (0x00, 0x33),  # Interpolated
            10: (0x00, 0x28),
        }
        if timebase in _TB_CHAN:
            return _TB_CHAN[timebase]
        # For higher timebases, approximate by halving from nearest known value
        if timebase > 10:
            val = 40  # tb=10 value
            for _ in range(timebase - 10):
                val = max(val // 2, 1)
            return ((val >> 8) & 0xFF, val & 0xFF)
        return (0x01, 0x57)  # Default to tb=5 values

    def _encode_hw_gain_bytes(self) -> Tuple[int, int, int]:
        """Encode channel enables, coupling, and gain into bytes 50-52.

        Returns (byte50, byte51, byte52) for the run-block sub-command
        (85 07 97 00 14 00 byte50 byte51 byte52).

        Byte 50: 0x20 | (B_enabled << 1) | A_enabled
                 (SDK: 0x21 = single A, 0x23 = dual A+B)
        Byte 51: (B_dc << 7) | (A_dc << 6) | (B_bank << 5) | (A_bank << 4)
                 | (B_selector << 1) | B_200mV_flag
        Byte 52: (A_selector << 5) | (A_200mV_flag << 4)
        """
        ch_a = self._channels.get(Channel.A, {})
        ch_b = self._channels.get(Channel.B, {})

        a_en = 1 if ch_a.get('enabled', True) else 0
        b_en = 1 if ch_b.get('enabled', False) else 0

        byte50 = 0x20 | (b_en << 1) | a_en

        # Coupling: DC=1, AC=0
        a_dc = 1 if ch_a.get('coupling', Coupling.DC) == Coupling.DC else 0
        b_dc = 1 if ch_b.get('coupling', Coupling.DC) == Coupling.DC else 0

        # Gain encoding from PGA table
        if a_en:
            range_a = ch_a.get('range', Range.R_5V)
            a_bank, a_sel, a_200 = _RANGE_HW_GAIN.get(range_a, (0, 7, 0))
        else:
            # SDK uses sel=1 for disabled channels (not the range default)
            a_bank, a_sel, a_200 = 0, 1, 0

        if b_en:
            range_b = ch_b.get('range', Range.R_5V)
            b_bank, b_sel, b_200 = _RANGE_HW_GAIN.get(range_b, (0, 7, 0))
        else:
            b_bank, b_sel, b_200 = 0, 1, 0

        byte51 = (b_dc << 7) | (a_dc << 6) | (b_bank << 5) | (a_bank << 4) \
                 | (b_sel << 1) | b_200
        byte52 = (a_sel << 5) | (a_200 << 4)

        return byte50, byte51, byte52

    def _build_capture_cmd1(self, n_samples: int, channel: Channel, timebase: int) -> bytes:
        """Construit la compound command 1 de capture (config + run block).

        Structure (64 bytes, zero-padded):
        - Trigger/sample count (0x85 0x08 0x85) — bytes 9-10: 16-bit BE sample count
        - Channel config (0x85 0x08 0x93) — bytes 19-20: timebase-dependent
        - Buffer config (0x85 0x08 0x89) — bytes 29-30: 2^timebase in BE
        - Get data setup (0x85 0x05 0x82)
        - Timebase config (0x85 0x04 0x9a)
        - Run block (0x85 0x07 0x97) — bytes 49-51 encode channel enables/coupling/gain
        - Status config (0x85 0x05 0x95)
        """
        # Channel enables + coupling + gain (bytes 49-51)
        byte50, byte51, byte52 = self._encode_hw_gain_bytes()

        # Timebase-dependent channel and buffer bytes
        # Buffer value = 2^timebase (big-endian 16-bit, capped at 0xFFFF)
        # Channel bytes: lookup from SDK trace (values for tb 0-10 observed)
        chan_hi, chan_lo = self._get_timebase_chan_bytes(timebase)
        buf_val = min(1 << timebase, 0xFFFF)
        buf_hi = (buf_val >> 8) & 0xFF
        buf_lo = buf_val & 0xFF

        # Sample count as 16-bit big-endian (positions 9-10)
        n = min(n_samples, 8192)
        count_hi = (n >> 8) & 0xFF
        count_lo = n & 0xFF

        return bytes([
            0x02,
            0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, count_hi, count_lo,  # Sample count (BE)
            0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, chan_hi, chan_lo,  # Channel (TB-dependent)
            0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, buf_hi, buf_lo,  # Buffer (2^tb BE)
            0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,  # Get data setup
            0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,  # Timebase config
            0x85, 0x07, 0x97, 0x00, 0x14, 0x00, byte50, byte51, byte52,  # Run block + gain
            0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00  # Status
        ])

    def _build_capture_cmd2(self) -> bytes:
        """Construit la compound command 2 de capture (siggen/buffer/ADC config).

        Structure (64 bytes, zero-padded):
        - Signal gen config (0x85 0x0c 0x86)
        - Data request (0x85 0x05 0x87)
        - Buffer setup (0x85 0x0b 0x90)
        - ADC config (0x85 0x08 0x8a)
        - Config end (0x85 0x04 0x81)
        """
        return bytes([
            0x02,
            0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
            0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x02,
            0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00
        ])

    def _flush_all_buffers(self):
        """Vide tous les buffers USB en attente."""
        for _ in range(5):
            try:
                self.device.read(EP_RESP_IN, 64, timeout=50)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError:
                break
        for _ in range(3):
            try:
                self.device.read(EP_DATA_IN, 16384, timeout=50)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError:
                break

    def _parse_waveform_buffer(self, raw: bytes, n_samples: int) -> np.ndarray:
        """Parse le buffer brut de waveform en échantillons uint8.

        The PS2204A has an 8-bit ADC. The 16KB buffer contains individual
        bytes (uint8), each representing one ADC sample (0-255, centered at 128).

        Buffer structure (16384 bytes):
        - Bytes 0-1: header marker (0x57 0xA7)
        - Bytes 2-16383: 16382 uint8 samples
        - Partially filled: zero gap in the middle, data at the end
        - Fully filled (trigger_buf >= 0x20): all post-header bytes are valid

        Returns signed int16 array (values centered around 0, range ±128)
        for compatibility with the scaling code.
        """
        all_bytes = np.frombuffer(raw, dtype=np.uint8)

        if len(all_bytes) <= 2:
            return np.array([], dtype=np.int16)

        # Skip 2-byte header
        buf = all_bytes[2:]

        non_zero = np.nonzero(buf)[0]
        if len(non_zero) == 0:
            # All zeros: input is at exactly mid-scale
            result = buf[-n_samples:].astype(np.int16)
            return result - ADC_CENTER

        n_nz = len(non_zero)

        if n_nz == len(buf):
            # Buffer fully populated
            data = buf[-n_samples:] if n_samples < len(buf) else buf
            return data.astype(np.int16) - ADC_CENTER

        # Partially filled circular buffer.
        # Extract data using the non-zero index range. The data is contiguous
        # between the first and last non-zero bytes, possibly with some zeros
        # before/after (trailing zeros from circular buffer wrap).
        first_nz = non_zero[0]
        last_nz = non_zero[-1]
        valid_data = buf[first_nz:last_nz + 1]

        if n_samples >= len(valid_data):
            result = valid_data
        else:
            result = valid_data[-n_samples:]

        # Convert uint8 to signed int16 centered at 0
        return result.astype(np.int16) - ADC_CENTER

    def capture_block(self, samples: int = 1000, channel: Channel = Channel.A,
                      timeout_ms: int = 5000) -> Optional[Dict]:
        """Lance une acquisition bloc (simple ou double canal).

        Séquence USB observée :
        1. Envoie les compound commands de configuration
        2. Poll le status jusqu'à 0x3b (données prêtes)
        3. Envoie la commande trigger pour transférer les données
        4. Lit les données sur EP 0x82

        Si les deux canaux sont activés, retourne 'A' et 'B'.
        """
        ch_a_enabled = self._channels.get(Channel.A, {}).get('enabled', True)
        ch_b_enabled = self._channels.get(Channel.B, {}).get('enabled', False)
        both_channels = ch_a_enabled and ch_b_enabled

        n_samples = min(samples, 8192)

        # Flush pending data
        self._flush_all_buffers()

        # Config Command 1: channel + timebase + run block
        config_cmd1 = self._build_capture_cmd1(n_samples, channel, self._current_timebase)
        self.device.write(EP_CMD_OUT, config_cmd1.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.01)

        # Config Command 2: siggen/buffer/ADC
        config_cmd2 = self._build_capture_cmd2()
        self.device.write(EP_CMD_OUT, config_cmd2.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.02)

        # Flush config responses
        for _ in range(3):
            try:
                self.device.read(EP_RESP_IN, 64, timeout=100)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError:
                break

        # Poll status until ready
        status = self._poll_status(timeout_ms=min(timeout_ms, 5000))

        if status == 0x7b:
            print("    Error: Device in error state (0x7b), attempting recovery...")
            self._flush_all_buffers()
            self._setup_channels()
            time.sleep(0.1)
            self.device.write(EP_CMD_OUT, config_cmd1.ljust(64, b'\x00'), timeout=1000)
            time.sleep(0.01)
            self.device.write(EP_CMD_OUT, config_cmd2.ljust(64, b'\x00'), timeout=1000)
            time.sleep(0.02)
            self._flush_all_buffers()
            status = self._poll_status(timeout_ms=2000)
            if status != 0x3b:
                print(f"    Recovery failed (status=0x{status:02x})")
                return None

        if status != 0x3b:
            print(f"    Warning: Expected 0x3b, got 0x{status:02x}")

        # Trigger command: tells FPGA to transfer captured data to host
        trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
        self.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.02)

        # Read waveform data
        read_size = 32768 if both_channels else 16384
        raw = self._read_data(read_size)
        if len(raw) < 4:
            time.sleep(0.2)
            raw = self._read_data(read_size)
        if len(raw) < 4:
            print("    No data received")
            return None

        # Parse waveform
        data = self._parse_waveform_buffer(raw, n_samples * (2 if both_channels else 1))

        result = {'overflow': 0}

        # Time array
        tb_info = self.get_timebase_info(self._current_timebase)

        if both_channels and len(data) >= n_samples:
            # Dual channel: try sequential layout (A first, then B)
            half = len(data) // 2
            data_a = data[:half]
            data_b = data[half:]

            range_a_mv = RANGE_MV.get(self._range_a, 5000)
            range_b_mv = RANGE_MV.get(self._range_b, 5000)

            result['A'] = data_a.astype(float) * (range_a_mv / ADC_HALF_RANGE)
            result['B'] = data_b.astype(float) * (range_b_mv / ADC_HALF_RANGE)
            result['samples'] = half
            result['time_ns'] = np.arange(half) * tb_info['sample_interval_ns']
        else:
            # Single channel
            current_range = self._range_a if channel == Channel.A else self._range_b
            range_mv = RANGE_MV.get(current_range, 5000)
            ch_key = 'A' if channel == Channel.A else 'B'
            result[ch_key] = data[:n_samples].astype(float) * (range_mv / ADC_HALF_RANGE)
            result['samples'] = min(len(data), n_samples)
            result['time_ns'] = np.arange(result['samples']) * tb_info['sample_interval_ns']

        return result

    def diagnose_channel_layout(self):
        """Diagnostic: détermine si les données double canal sont séquentielles ou interleaved.

        Connecter un signal connu sur CH A, laisser CH B flottant.
        Compare les deux layouts possibles.
        """
        self.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
        self.set_channel(Channel.B, True, Coupling.DC, Range.R_5V)
        self.set_timebase(5, 2000)

        self._flush_all_buffers()
        config_cmd1 = self._build_capture_cmd1(4000, Channel.A, self._current_timebase)
        self.device.write(EP_CMD_OUT, config_cmd1.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.01)
        config_cmd2 = self._build_capture_cmd2()
        self.device.write(EP_CMD_OUT, config_cmd2.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.02)
        self._flush_all_buffers()
        status = self._poll_status(timeout_ms=3000)
        if status != 0x3b:
            print(f"    Diagnostic: status=0x{status:02x}, cannot proceed")
            return

        trigger_cmd = bytes([0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00])
        self.device.write(EP_CMD_OUT, trigger_cmd.ljust(64, b'\x00'), timeout=1000)
        time.sleep(0.02)

        raw = self._read_data(32768)
        if len(raw) < 100:
            print("    Diagnostic: not enough data")
            return

        samples = np.frombuffer(raw, dtype='<i2')
        half = len(samples) // 2

        # Sequential test
        seq_a = samples[:half]
        seq_b = samples[half:]

        # Interleaved test
        int_a = samples[0::2]
        int_b = samples[1::2]

        print(f"    Raw buffer: {len(samples)} samples")
        print(f"    Sequential: A(mean={seq_a.mean():.0f}, std={seq_a.std():.0f}) "
              f"B(mean={seq_b.mean():.0f}, std={seq_b.std():.0f})")
        print(f"    Interleaved: A(mean={int_a.mean():.0f}, std={int_a.std():.0f}) "
              f"B(mean={int_b.mean():.0f}, std={int_b.std():.0f})")
        print("    → Si un signal est sur CH A seulement, le layout avec la plus grande")
        print("      différence de std entre A et B est le bon.")

    # ========================================================================
    # Fast block capture (optimized for streaming)
    # ========================================================================

    def _fast_block_capture(self, cmd1: bytes, cmd2: bytes,
                            trigger_cmd: bytes) -> Optional[bytes]:
        """Optimized block capture for streaming — minimal overhead.

        Differences from capture_block():
        - No _flush_all_buffers() between blocks (pipeline is clean)
        - No sleeps after cmd1/cmd2
        - Skip ACK reads, go straight to status polling
        - Aggressive polling (no inter-poll sleep)
        - No error recovery (caller handles errors)

        Returns raw bytes from EP 0x82, or None on failure.
        """
        # Send commands immediately (no sleep between)
        self.device.write(EP_CMD_OUT, cmd1, timeout=1000)
        self.device.write(EP_CMD_OUT, cmd2, timeout=1000)

        # Poll status aggressively (skip ACK reads)
        poll_cmd = bytes([0x02, 0x01, 0x01, 0x80]).ljust(64, b'\x00')
        status = 0
        for _ in range(500):
            self.device.write(EP_CMD_OUT, poll_cmd, timeout=500)
            try:
                resp = bytes(self.device.read(EP_RESP_IN, 64, timeout=20))
                if resp and len(resp) >= 1:
                    if resp[0] == 0x3b:
                        status = 0x3b
                        break
                    elif resp[0] == 0x7b:
                        return None  # Error state
            except usb.core.USBTimeoutError:
                pass

        if status != 0x3b:
            return None

        # Trigger + read
        self.device.write(EP_CMD_OUT, trigger_cmd, timeout=1000)
        try:
            return bytes(self.device.read(EP_DATA_IN, 16384, timeout=2000))
        except usb.core.USBTimeoutError:
            return None

    # ========================================================================
    # Streaming — native hardware streaming protocol
    # ========================================================================

    def _build_streaming_cmd1(self, n_samples: int) -> bytes:
        """Build compound command 1 for native streaming mode.

        Key differences from block capture cmd1:
        - Byte 37: 0x41 (streaming flag) instead of 0x01
        - Chan bytes [19:20]: fixed (0x00, 0x06) instead of timebase-dependent
        - Buffer bytes [27:30]: (0x0f, 0x42, 0x40) streaming-specific config
        - Sample count [9:10]: max_samples parameter
        - Gain bytes [50:52]: same as block mode
        """
        byte50, byte51, byte52 = self._encode_hw_gain_bytes()

        n = min(n_samples, 0xFFFF)
        count_hi = (n >> 8) & 0xFF
        count_lo = n & 0xFF

        return bytes([
            0x02,
            0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, count_hi, count_lo,
            0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x06,  # Fixed chan bytes
            0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x0f, 0x42, 0x40,  # Streaming buffer
            0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x41,  # 0x41 = streaming mode
            0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
            0x85, 0x07, 0x97, 0x00, 0x14, 0x00, byte50, byte51, byte52,
            0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
        ])

    def _build_streaming_cmd2(self) -> bytes:
        """Build compound command 2 for native streaming mode.

        Uses the SDK streaming cmd2 observed values (fast streaming variant).
        Differences from block cmd2:
        - Siggen bytes [7:10]: streaming rate config
        - ADC config byte [47]: 0x05 instead of block's value
        """
        return bytes([
            0x02,
            0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0xb0, 0x19, 0xa6, 0x10, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
            0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x03, 0x05,
            0x00, 0x02, 0x0c, 0x03, 0x0a, 0x00, 0x00,
            0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        ])

    def _streaming_start_hw(self, n_samples: int) -> bool:
        """Start hardware streaming mode.

        Sends streaming cmd1 + cmd2 + streaming trigger.
        Data then arrives continuously on EP 0x82 in small packets.
        """
        self._flush_all_buffers()

        cmd1 = self._build_streaming_cmd1(n_samples)
        self.device.write(EP_CMD_OUT, cmd1.ljust(64, b'\x00'), timeout=1000)

        cmd2 = self._build_streaming_cmd2()
        self.device.write(EP_CMD_OUT, cmd2.ljust(64, b'\x00'), timeout=1000)

        # Flush ACKs
        for _ in range(3):
            try:
                self.device.read(EP_RESP_IN, 64, timeout=50)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError:
                break

        # Streaming trigger (different from block trigger)
        # Block: 02 07 06 00 40 00 00 02 01 00
        # Stream: 02 07 06 00 00 00 00 01 00 00
        trigger = bytes([0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00])
        self.device.write(EP_CMD_OUT, trigger.ljust(64, b'\x00'), timeout=1000)

        return True

    def _streaming_stop_hw(self):
        """Stop hardware streaming mode.

        Sends the stop command (opcode 0x0a) observed from SDK trace.
        """
        try:
            stop_cmd = bytes([0x02, 0x0a, 0x00, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
            self.device.write(EP_CMD_OUT, stop_cmd.ljust(64, b'\x00'), timeout=1000)
            time.sleep(0.01)
            # Send it again (SDK sends it twice)
            stop_cmd2 = bytes([0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a])
            self.device.write(EP_CMD_OUT, stop_cmd2.ljust(64, b'\x00'), timeout=1000)
        except usb.core.USBError:
            pass

        # Flush any remaining data
        self._flush_all_buffers()

    def run_streaming(self, sample_interval_us: int = 100,
                      callback: Optional[Callable] = None,
                      max_samples: int = 100000,
                      auto_stop: bool = True,
                      overview_buffer_size: int = 50000,
                      mode: str = 'fast'):
        """Start streaming acquisition.

        Two modes:
        - 'fast': Optimized rapid block captures (~500 kS/s, 8064 samples/block,
          ~16ms/block). Small gaps between blocks but very high throughput.
        - 'native': Hardware continuous streaming (~100 S/s, truly continuous
          with no gaps). Good for very slow monitoring.

        sample_interval_us: desired sample interval in microseconds (used for
            timebase selection in 'fast' mode; ignored in 'native' mode)
        callback: called for each data chunk, signature: callback(data_dict)
        max_samples: ring buffer capacity (also auto-stop limit if auto_stop=True)
        auto_stop: stop after max_samples (True) or wrap ring buffer (False)
        overview_buffer_size: ignored (SDK compatibility)
        mode: 'fast' (default) or 'native'
        """
        if self._streaming:
            print("    Streaming already active, stopping first...")
            self.stop_streaming()

        # Timebase for fast mode: use explicit value if set, otherwise compute
        if mode == 'fast' and sample_interval_us > 0:
            target_ns = sample_interval_us * 1000
            computed_tb = self._interval_to_timebase(target_ns)
            # Clamp to tested range (tb 0-10 have verified chan/buf bytes)
            self._current_timebase = min(computed_tb, 10)

        self._stream_max_samples = max_samples
        self._stream_auto_stop = auto_stop
        self._stream_callback = callback
        self._stream_overflow = False
        self._stream_capacity = max_samples
        self._stream_mode = mode

        # Ring buffer
        with self._stream_lock:
            self._stream_buffer_a = np.zeros(max_samples, dtype=np.float64)
            self._stream_buffer_b = np.zeros(max_samples, dtype=np.float64)
            self._stream_write_pos = 0

        # Stats
        self._stream_blocks = 0
        self._stream_samples_total = 0
        self._stream_start_time = time.time()
        self._stream_last_block_ms = 0.0

        self._streaming = True
        if mode == 'native':
            self._stream_thread = threading.Thread(
                target=self._streaming_loop_native, daemon=True)
        else:
            self._stream_thread = threading.Thread(
                target=self._streaming_loop_fast, daemon=True)
        self._stream_thread.start()

        tb_info = self.get_timebase_info(self._current_timebase)
        print(f"    Streaming started ({mode}): tb={self._current_timebase}, "
              f"interval={tb_info['sample_interval_ns']}ns, buffer={max_samples}")

    def _streaming_loop_fast(self):
        """Thread worker: optimized rapid block capture streaming.

        Achieves ~500 kS/s by pre-building commands and minimizing USB overhead.
        Each cycle: send cmd1+cmd2, poll status, trigger, read 16KB → ~16ms.
        Block of 8064 samples captured at full ADC rate, with ~13ms gap between blocks.
        """
        ch_a_enabled = self._channels.get(Channel.A, {}).get('enabled', True)
        ch_b_enabled = self._channels.get(Channel.B, {}).get('enabled', False)
        both_channels = ch_a_enabled and ch_b_enabled
        block_size = 3968 if both_channels else 8064
        range_a_mv = RANGE_MV.get(self._range_a, 5000)
        range_b_mv = RANGE_MV.get(self._range_b, 5000)
        scale_a = range_a_mv / ADC_HALF_RANGE
        scale_b = range_b_mv / ADC_HALF_RANGE

        # Pre-build commands once
        cmd1 = self._build_capture_cmd1(block_size, Channel.A, self._current_timebase)
        cmd2 = self._build_capture_cmd2()
        trigger_cmd = bytes([
            0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00
        ]).ljust(64, b'\x00')

        # Single initial flush
        self._flush_all_buffers()
        errors = 0

        while self._streaming:
            t0 = time.time()

            try:
                raw = self._fast_block_capture(cmd1, cmd2, trigger_cmd)
            except usb.core.USBError as e:
                errors += 1
                if errors > 5:
                    print(f"    Streaming: too many errors ({e}), stopping")
                    break
                self._flush_all_buffers()
                time.sleep(0.05)
                continue

            if raw is None or len(raw) < 4:
                errors += 1
                if errors > 10:
                    print(f"    Streaming: too many failures, stopping")
                    break
                self._flush_all_buffers()
                time.sleep(0.02)
                continue

            errors = 0  # Reset on success

            # Parse waveform buffer (uint8 ADC data)
            data = self._parse_waveform_buffer(raw, block_size)
            n = min(len(data), block_size)
            if n == 0:
                continue

            # Scale to mV
            mv_a = data[:n].astype(np.float64) * scale_a

            # Write to ring buffer
            with self._stream_lock:
                self._ring_write(self._stream_buffer_a, mv_a)

            # Stats
            self._stream_blocks += 1
            self._stream_samples_total += n
            self._stream_last_block_ms = (time.time() - t0) * 1000

            # Callback
            if self._stream_callback:
                try:
                    self._stream_callback({'A': mv_a, 'samples': n})
                except Exception as e:
                    print(f"    Streaming callback error: {e}")

        self._streaming = False

    def _streaming_loop_native(self):
        """Thread worker: native hardware streaming (~100 S/s continuous).

        Uses the FPGA's native streaming mode discovered from SDK USB trace.
        Data arrives continuously in small packets without poll-trigger cycles.
        """
        range_a_mv = RANGE_MV.get(self._range_a, 5000)
        scale_a = range_a_mv / ADC_HALF_RANGE

        try:
            self._streaming_start_hw(self._stream_max_samples)
        except usb.core.USBError as e:
            print(f"    Streaming start failed: {e}")
            self._streaming = False
            return

        consecutive_timeouts = 0

        while self._streaming:
            t0 = time.time()
            try:
                raw = bytes(self.device.read(EP_DATA_IN, 512, timeout=200))
                consecutive_timeouts = 0
            except usb.core.USBTimeoutError:
                consecutive_timeouts += 1
                if consecutive_timeouts > 50:
                    print(f"    Native streaming: too many timeouts, stopping")
                    break
                continue
            except usb.core.USBError as e:
                print(f"    Native streaming USB error: {e}")
                break

            if not raw:
                continue

            # Parse raw uint8 samples
            samples = np.frombuffer(raw, dtype=np.uint8).astype(np.int16) - ADC_CENTER
            mv_a = samples.astype(np.float64) * scale_a
            n = len(mv_a)

            with self._stream_lock:
                self._ring_write(self._stream_buffer_a, mv_a)

            self._stream_blocks += 1
            self._stream_samples_total += n
            self._stream_last_block_ms = (time.time() - t0) * 1000

            if self._stream_callback:
                try:
                    self._stream_callback({'A': mv_a, 'samples': n})
                except Exception as e:
                    print(f"    Streaming callback error: {e}")

        try:
            self._streaming_stop_hw()
        except usb.core.USBError:
            pass
        self._streaming = False

    def _ring_write(self, buf: np.ndarray, data: np.ndarray):
        """Write data to ring buffer. Must hold _stream_lock."""
        cap = self._stream_capacity
        wp = self._stream_write_pos
        n = len(data)

        if self._stream_auto_stop and wp >= cap:
            self._streaming = False
            return

        if self._stream_auto_stop:
            n = min(n, cap - wp)

        idx = wp % cap
        end_idx = idx + n
        if end_idx <= cap:
            buf[idx:end_idx] = data[:n]
        else:
            first = cap - idx
            buf[idx:cap] = data[:first]
            buf[0:n - first] = data[first:n]

        self._stream_write_pos = wp + n

    def stop_streaming(self):
        """Stop streaming acquisition."""
        self._streaming = False
        if self._stream_thread is not None:
            self._stream_thread.join(timeout=5.0)
            self._stream_thread = None

    def get_streaming_data(self) -> Optional[Dict]:
        """Return all streaming data collected so far."""
        with self._stream_lock:
            wp = self._stream_write_pos
            if wp == 0:
                return None

            cap = self._stream_capacity
            n = min(wp, cap)

            if wp <= cap:
                # Buffer not wrapped yet
                result = {
                    'samples': n,
                    'A': self._stream_buffer_a[:n].copy(),
                }
            else:
                # Wrapped: reconstruct in chronological order
                idx = wp % cap
                result = {
                    'samples': cap,
                    'A': np.concatenate([
                        self._stream_buffer_a[idx:cap],
                        self._stream_buffer_a[:idx],
                    ]),
                }

            result['overflow'] = 1 if self._stream_overflow else 0
            return result

    def get_latest_values(self, n_samples: int = 1024) -> Optional[Dict]:
        """Return the N most recent streaming samples (for GUI)."""
        with self._stream_lock:
            wp = self._stream_write_pos
            if wp == 0:
                return None

            cap = self._stream_capacity
            n = min(n_samples, min(wp, cap))

            # Read the last n samples from the ring buffer
            idx = wp % cap
            if idx >= n:
                data_a = self._stream_buffer_a[idx - n:idx].copy()
            else:
                # Wraps around
                tail = self._stream_buffer_a[cap - (n - idx):cap]
                head = self._stream_buffer_a[:idx]
                data_a = np.concatenate([tail, head])

            return {
                'samples': n,
                'A': data_a,
                'overflow': 1 if self._stream_overflow else 0,
            }

    def get_streaming_stats(self) -> Dict:
        """Return streaming performance statistics."""
        elapsed = time.time() - self._stream_start_time if self._stream_start_time else 0
        return {
            'blocks': self._stream_blocks,
            'total_samples': self._stream_samples_total,
            'elapsed_s': elapsed,
            'samples_per_sec': self._stream_samples_total / elapsed if elapsed > 0 else 0,
            'blocks_per_sec': self._stream_blocks / elapsed if elapsed > 0 else 0,
            'last_block_ms': self._stream_last_block_ms,
            'avg_block_ms': (elapsed * 1000 / self._stream_blocks) if self._stream_blocks > 0 else 0,
            'write_pos': self._stream_write_pos,
            'capacity': self._stream_capacity,
        }

    def is_streaming(self) -> bool:
        """Return True if streaming is active."""
        return self._streaming


# ============================================================================
# Test
# ============================================================================

def main():
    try:
        with PicoScopeFull() as scope:
            print("\nDevice Info:")
            for k, v in scope.get_info().items():
                print(f"  {k}: {v}")

            # ============================================================
            # Test 1: Block capture
            # ============================================================
            print("\n" + "=" * 50)
            print("Test 1: Block capture x5")
            print("=" * 50)

            scope.flash_led()
            scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
            scope.set_timebase(5, 1000)

            successes = 0
            for i in range(5):
                print(f"\n  === Capture {i+1} ===")
                data = scope.capture_block(1000)
                if data and 'A' in data and len(data['A']) > 10:
                    samples = data['A']
                    print(f"    SUCCESS - {len(samples)} samples, "
                          f"Min={samples.min():.1f}mV, Max={samples.max():.1f}mV")
                    successes += 1
                else:
                    print("    FAILED - no data")
                time.sleep(0.1)

            print(f"\nBlock capture: {successes}/5 succeeded")

            # ============================================================
            # Test 2: Signal generator
            # ============================================================
            print("\n" + "=" * 50)
            print("Test 2: Signal Generator (1kHz sine)")
            print("=" * 50)

            scope.set_signal_generator(WaveType.SINE, 1000, 2000)
            time.sleep(0.5)

            data = scope.capture_block(2000)
            if data and 'A' in data:
                s = data['A']
                print(f"    Siggen capture: {len(s)} samples, "
                      f"Vpp={s.max()-s.min():.0f}mV, "
                      f"Min={s.min():.0f}mV, Max={s.max():.0f}mV")
            scope.disable_signal_generator()

            # ============================================================
            # Test 3: Native streaming
            # ============================================================
            print("\n" + "=" * 50)
            print("Test 3: Fast streaming (3 seconds)")
            print("=" * 50)

            chunks_received = [0]

            def on_chunk(data):
                chunks_received[0] += 1

            scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
            scope.set_timebase(5, 8064)
            scope.run_streaming(
                sample_interval_us=1,  # Use fastest rate (tb=0 → tb clamped to set value)
                callback=on_chunk,
                max_samples=500000,
                auto_stop=False
            )

            # Stream for 3 seconds, print stats every second
            for sec in range(3):
                time.sleep(1.0)
                stats = scope.get_streaming_stats()
                print(f"    {sec+1}s: {stats['total_samples']} samples, "
                      f"{stats['samples_per_sec']:.0f} S/s, "
                      f"{stats['blocks']} chunks, "
                      f"avg {stats['avg_block_ms']:.1f}ms/chunk")

            scope.stop_streaming()
            stats = scope.get_streaming_stats()
            print(f"    Final: {stats['total_samples']} total samples in "
                  f"{stats['elapsed_s']:.1f}s = {stats['samples_per_sec']:.0f} S/s")

            stream_data = scope.get_streaming_data()
            if stream_data:
                s = stream_data['A']
                print(f"    Buffer: {stream_data['samples']} samples, "
                      f"Min={s.min():.0f}mV, Max={s.max():.0f}mV, "
                      f"Mean={s.mean():.0f}mV")
            else:
                print("    Streaming: no data")

            # ============================================================
            # Test 4: Different timebases
            # ============================================================
            print("\n" + "=" * 50)
            print("Test 4: Timebase info")
            print("=" * 50)

            for tb in [0, 1, 2, 3, 5, 10, 15, 20]:
                info = scope.get_timebase_info(tb)
                print(f"    TB {tb:2d}: {info['sample_interval_ns']:8.0f}ns = "
                      f"{info['sample_rate_hz']/1e6:.2f} MSPS")

            scope.stop()

            print("\n" + "=" * 50)
            print("All tests completed!")
            print("=" * 50)

    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()


# Alias for GUI compatibility - allows drop-in replacement of picoscope2000
PicoScope2000 = PicoScopeFull


if __name__ == "__main__":
    main()
