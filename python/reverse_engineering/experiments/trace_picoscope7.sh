#!/bin/bash
# Launch PicoScope 7 under LD_PRELOAD to capture the exact USB dialogue.
# Usage: ./trace_picoscope7.sh [output-dir]
# When PicoScope 7 exits the trace is moved to $OUT/usb_trace.log.
set -e
cd "$(dirname "$0")"
OUT="${1:-trace_picoscope7}"
mkdir -p "$OUT"
rm -f usb_trace.log ep06_*.bin
echo "== Launching PicoScope 7 with LD_PRELOAD=./usb_interceptor.so =="
echo "   Trace will be saved to $OUT/usb_trace.log on exit."
echo "   Inside PicoScope 7:"
echo "     1. Connect the 2204A."
echo "     2. Set timebase to something sensible for UART (e.g. 10 us/div)."
echo "     3. Add 'Serial Decoding -> UART' at 9600 8N1 on the channel."
echo "     4. Run streaming (Play button) — let it capture several bursts."
echo "     5. Stop (square button)."
echo "     6. Quit PicoScope 7."
echo ""
LD_PRELOAD="$PWD/usb_interceptor.so" /opt/picoscope/lib/PicoScope.GTK
mv -f usb_trace.log "$OUT/usb_trace.log" 2>/dev/null || true
mv -f ep06_*.bin "$OUT/" 2>/dev/null || true
echo "== Done — trace in $OUT/usb_trace.log =="
ls -la "$OUT/"
