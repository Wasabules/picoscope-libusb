#!/usr/bin/env python3
"""
Extract every cmd2 packet (`02 85 0c 86 ...`) from the SDK parametric
trace and report how each byte position varies across the 22 streaming
cycles. The goal is to confirm (or refute) which bytes are true
variables vs. opaque constants safe to zero in our driver.
"""
import re
import sys
from pathlib import Path

DEFAULT = Path(__file__).parent / "sdk_param_trace.log"
log = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT

re_pkt = re.compile(r'^\[(\d+)\] t=(\d+) (?:SYNC|ASYNC) BULK OUT EP 0x01')
re_tx = re.compile(r'TX \(\d+ bytes\): ([0-9a-f ]+)')

cmd2_packets = []
lines = log.read_text().splitlines()
i = 0
while i < len(lines):
    m = re_pkt.match(lines[i])
    if m:
        idx = int(m.group(1)); t = int(m.group(2))
        for j in range(i + 1, min(i + 4, len(lines))):
            mm = re_tx.search(lines[j])
            if mm:
                toks = mm.group(1).strip().split()
                if len(toks) >= 4 and toks[:4] == ['02', '85', '0c', '86']:
                    cmd2_packets.append((idx, t, toks))
                i = j
                break
    i += 1

print(f"Found {len(cmd2_packets)} cmd2 packets\n")

# Trim trailing zeros for readability
payloads = []
for idx, t, toks in cmd2_packets:
    trimmed = list(toks)
    while trimmed and trimmed[-1] == '00':
        trimmed.pop()
    payloads.append((idx, t, trimmed))

# Find max effective length
maxlen = max(len(p[2]) for p in payloads)
print(f"Effective max length (trailing zeros trimmed): {maxlen}\n")

# Per-byte variation
print(f"{'pos':>3}  {'#vals':>5}  values (ordered by cycle, -- = past end)")
print("-" * 100)
var_positions = []
for pos in range(maxlen):
    col = []
    for idx, t, toks in payloads:
        col.append(toks[pos] if pos < len(toks) else '--')
    distinct = set(v for v in col if v != '--')
    if len(distinct) > 1:
        var_positions.append(pos)
        print(f"{pos:>3}  {len(distinct):>5}  {' '.join(col)}")

print(f"\nVariable byte positions: {var_positions}")

# Focus analysis on bytes 7..9 (the opaque counter hypothesis)
print("\n=== Bytes 7..9 across cycles (counter hypothesis) ===")
print(f"{'cycle':>5}  {'idx':>5}  {'t_us':>10}  byte7 byte8 byte9  dec(BE 24-bit)")
prev24 = None
for c, (idx, t, toks) in enumerate(payloads):
    b7 = toks[7] if len(toks) > 7 else '00'
    b8 = toks[8] if len(toks) > 8 else '00'
    b9 = toks[9] if len(toks) > 9 else '00'
    v = int(b7, 16) * 65536 + int(b8, 16) * 256 + int(b9, 16)
    delta = f"  Δ={v - prev24:+d}" if prev24 is not None else ""
    print(f"{c+1:>5}  {idx:>5}  {t:>10}   {b7}    {b8}    {b9}    {v:>10}{delta}")
    prev24 = v

# Byte 47 (auto_stop) analysis
print("\n=== Byte 47 (auto_stop) across cycles ===")
print(f"{'cycle':>5}  {'idx':>5}  byte47")
for c, (idx, t, toks) in enumerate(payloads):
    b47 = toks[47] if len(toks) > 47 else '00'
    print(f"{c+1:>5}  {idx:>5}  {b47}")
