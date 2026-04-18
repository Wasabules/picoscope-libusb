#!/usr/bin/env python3
"""
Offline firmware extractor for PicoScope 2204A.

Reads a usbmon pcap/pcapng recorded while the official Pico software was
talking to the scope, and writes fx2.bin / fpga.bin / waveform.bin /
stream_lut.bin into the same directory the driver searches at open-time.

Usage:
    ./extract-from-pcap.py capture.pcap [--out DIR]

DIR defaults to:
    $PS2204A_FIRMWARE_DIR
    $XDG_CONFIG_HOME/picoscope-libusb/firmware
    $HOME/.config/picoscope-libusb/firmware
"""
import argparse
import os
import subprocess
import sys
from collections import OrderedDict


def default_out_dir():
    env = os.environ.get("PS2204A_FIRMWARE_DIR")
    if env:
        return env
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        return os.path.join(xdg, "picoscope-libusb", "firmware")
    home = os.environ.get("HOME")
    if home:
        return os.path.join(home, ".config", "picoscope-libusb", "firmware")
    raise SystemExit("cannot determine output directory; pass --out")


def run_tshark(pcap, display_filter):
    cmd = ["tshark", "-r", pcap, "-Y", display_filter, "-T", "pdml"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        raise SystemExit(f"tshark failed: {r.stderr.strip()}")
    return r.stdout


def _attr(line, key):
    probe = f'{key}="'
    i = line.find(probe)
    if i < 0:
        return None
    i += len(probe)
    j = line.find('"', i)
    return line[i:j] if j > i else None


def extract_fx2(pcap):
    """Return ordered dict {addr: data} for every control OUT with bReq=0xA0."""
    pdml = run_tshark(
        pcap, "usb.bmRequestType == 0x40 and usb.setup.bRequest == 160")
    chunks = OrderedDict()
    cur_addr = None
    for line in pdml.splitlines():
        v = _attr(line, "show") if "usb.setup.wValue" in line else None
        if v is None and "usb.setup.wValue" in line:
            v = _attr(line, "value")
            if v and len(v) == 4:
                v = int(v[2:4] + v[0:2], 16)
        elif v is not None:
            try:
                v = int(v, 0)
            except ValueError:
                v = None
        if "usb.setup.wValue" in line and v is not None:
            cur_addr = v
        elif "usb.data_fragment" in line and cur_addr is not None:
            data_hex = _attr(line, "value")
            if data_hex:
                chunks[cur_addr] = bytes.fromhex(data_hex)
            cur_addr = None
    return chunks


def pack_fx2(chunks):
    """Match the format load_firmware() expects: [addr_be16][len_u8][data]+.
    CPU-halt / CPU-release chunks (wValue == 0xE600) are dropped — the driver
    emits those itself."""
    buf = bytearray()
    kept = 0
    for addr, data in chunks.items():
        if addr == 0xE600:
            continue
        if len(data) > 255:
            raise SystemExit(
                f"FX2 chunk at 0x{addr:04X} is {len(data)} B (>255). "
                "Capture is not byte-level — re-record with usbmon, not usbmon_mon.")
        buf.append((addr >> 8) & 0xff)
        buf.append(addr & 0xff)
        buf.append(len(data))
        buf += data
        kept += 1
    return bytes(buf), kept


def extract_ep06(pcap):
    """Return a list of bulk OUT payloads on EP 0x06, in capture order."""
    pdml = run_tshark(pcap, "usb.endpoint_address == 0x06 and usb.data_len > 0")
    out = []
    for line in pdml.splitlines():
        if "usb.capdata" in line:
            data_hex = _attr(line, "value")
            if data_hex:
                out.append(bytes.fromhex(data_hex))
    return out


def split_ep06(chunks):
    """Separate the FPGA bitstream from the 8 KiB channel LUT.

    The scope uploads the FPGA as many large (>= 1 KiB) bulk OUTs, then one
    8 KiB LUT per channel setup. Stream LUT and waveform LUT are the same
    8 KiB blob on 2204A, so the first small one we see covers both."""
    fpga = bytearray()
    lut = None
    for c in chunks:
        if lut is None and len(c) == 8192:
            lut = c
        elif len(c) >= 1024:
            fpga += c
    return bytes(fpga), lut


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
    print(f"wrote {path} ({len(data)} bytes)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", help="usbmon capture file")
    ap.add_argument("--out", default=None,
                    help="output directory (default: driver's search path)")
    args = ap.parse_args()

    out = args.out or default_out_dir()
    os.makedirs(out, exist_ok=True)

    print(f"reading {args.pcap} ...")
    fx2_chunks = extract_fx2(args.pcap)
    ep06 = extract_ep06(args.pcap)
    print(f"  FX2 control chunks: {len(fx2_chunks)}")
    print(f"  EP 0x06 bulk OUTs : {len(ep06)}")

    fx2, kept = pack_fx2(fx2_chunks)
    fpga, lut = split_ep06(ep06)

    if not fx2 or not fpga or not lut:
        print("WARNING: capture is incomplete:", file=sys.stderr)
        if not fx2:  print("  no FX2 chunks found (looking for bReq=0xA0)",
                           file=sys.stderr)
        if not fpga: print("  no FPGA bitstream found (large EP 0x06 OUTs)",
                           file=sys.stderr)
        if not lut:  print("  no waveform LUT found (8 KiB EP 0x06 OUT)",
                           file=sys.stderr)
        print("  Re-record a full 'open scope' session with the official SDK.",
              file=sys.stderr)

    if fx2:  write(os.path.join(out, "fx2.bin"), fx2)
    if fpga: write(os.path.join(out, "fpga.bin"), fpga)
    if lut:
        write(os.path.join(out, "waveform.bin"), lut)
        write(os.path.join(out, "stream_lut.bin"), lut)

    if fx2 and fpga and lut:
        print("\ndone. Verify:")
        print(f"  ls -l {out}")


if __name__ == "__main__":
    main()
