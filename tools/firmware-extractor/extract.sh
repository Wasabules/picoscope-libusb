#!/usr/bin/env bash
# Firmware extractor — runs the official Pico SDK once under LD_PRELOAD to
# capture the FX2 / FPGA / LUT blobs off the USB bus.
#
# Requirements on this machine:
#   - The scope, freshly plugged in (firmware only uploads on fresh power-up).
#   - The official Pico SDK installed (for libps2000.so).
#   - gcc, python3, libusb-1.0 headers (to build the shim if needed).
#
# Output goes to $PS2204A_FIRMWARE_DIR, $XDG_CONFIG_HOME/picoscope-libusb/firmware,
# or ~/.config/picoscope-libusb/firmware — whichever resolves first.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIM="$HERE/libps_intercept.so"

find_sdk() {
    for p in \
        /opt/picoscope/lib/libps2000.so \
        /usr/lib/libps2000.so \
        /usr/lib/x86_64-linux-gnu/libps2000.so; do
        [[ -e "$p" ]] && { echo "$p"; return 0; }
    done
    return 1
}

SDK_LIB="$(find_sdk)" || {
    echo "error: libps2000.so not found — install the official Pico SDK" >&2
    echo "  (https://www.picotech.com/downloads/linux)" >&2
    exit 1
}

if [[ ! -e "$SHIM" ]]; then
    echo "[extract] building shim…"
    make -C "$HERE"
fi

OUT_DIR="${PS2204A_FIRMWARE_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/picoscope-libusb/firmware}"
mkdir -p "$OUT_DIR"
echo "[extract] output dir: $OUT_DIR"
echo "[extract] SDK lib:    $SDK_LIB"
echo "[extract] shim:       $SHIM"
echo

LD_PRELOAD="$SHIM" \
LD_LIBRARY_PATH="$(dirname "$SDK_LIB")" \
PS2204A_FIRMWARE_DIR="$OUT_DIR" \
python3 - <<PY
import ctypes, sys, time
lib = ctypes.CDLL("$SDK_LIB")
print("[extract] opening scope via SDK…", flush=True)
h = lib.ps2000_open_unit()
print(f"[extract] handle={h}", flush=True)
if h <= 0:
    sys.stderr.write("ps2000_open_unit() failed — unplug/replug the scope and retry\n")
    sys.exit(2)
time.sleep(0.5)
lib.ps2000_close_unit(ctypes.c_int16(h))
print("[extract] done.", flush=True)
PY

echo
echo "[extract] checking result:"
for f in fx2.bin fpga.bin waveform.bin stream_lut.bin; do
    if [[ -s "$OUT_DIR/$f" ]]; then
        size=$(stat -c%s "$OUT_DIR/$f")
        echo "  ✓ $f ($size bytes)"
    else
        echo "  ✗ $f missing"
    fi
done
