#!/usr/bin/env python3
"""Re-extract cmd1 from the original sdk_param_trace.log to check whether
phase 22's cmd1[28..30] really was 0f 42 3f or if that was a parsing error
in the previous analysis.

sdk_param_trace.log phases (from sdk_stream_param.c):
  phase 17: as=0 ms=10000  iv=1us
  phase 22: as=1 ms=100000 iv=1us
"""
import re
from pathlib import Path

LOG = Path(__file__).parent / "sdk_param_trace.log"
re_pkt = re.compile(r'^\[(\d+)\] t=(\d+) (?:SYNC|ASYNC) BULK OUT EP 0x01')
re_tx = re.compile(r'TX \(\d+ bytes\): ([0-9a-f ]+)')

lines = LOG.read_text().splitlines()
packets = []
i = 0
while i < len(lines):
    m = re_pkt.match(lines[i])
    if m:
        idx = int(m.group(1)); t = int(m.group(2))
        for j in range(i + 1, min(i + 4, len(lines))):
            mm = re_tx.search(lines[j])
            if mm:
                toks = mm.group(1).strip().split()
                packets.append((idx, t, toks))
                i = j
                break
    i += 1

def is_cmd1(toks):
    return len(toks) >= 4 and toks[:4] == ['02', '85', '08', '85']

cmd1_pos = [k for k, p in enumerate(packets) if is_cmd1(p[2])]
print(f"Found {len(cmd1_pos)} cmd1 packets in old trace.")

# In the original script there are 30 phases; phase 17 and phase 22 are the
# 17th and 22nd cmd1 from the streaming side. Some phases (siggen, etc.)
# don't emit cmd1. Let me dump all cmd1 packets with their key bytes.
print(f"\n{'idx':>5} {'sc(6..10)':>12}  {'iv(28..30)':>12}  full_cmd1_head")
for k in cmd1_pos:
    cmd1 = packets[k][2]
    sc = int(''.join(cmd1[6:11]), 16)
    iv = int(''.join(cmd1[28:31]), 16)
    print(f"{packets[k][0]:>5} {sc:>12}  {iv:>12}  {' '.join(cmd1[:32])}")
