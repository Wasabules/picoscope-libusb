#!/usr/bin/env python3
"""List all OUT 0x01 commands that are NOT the 85 07 97 / 85 04 9a heartbeat —
these are the 'active' protocol commands: cmd1, trigger, cmd2, stop, etc.
Phase boundaries become visible by gaps in this active stream."""
import re

path = "/home/utilisateur/picoscope-libusb/tools/sdk-stream-trace/sdk_param_trace.log"
with open(path) as f:
    lines = f.read().splitlines()

re_pkt = re.compile(r'^\[(\d+)\] t=(\d+) (?:SYNC|ASYNC) BULK (OUT|IN ) EP (0x\d+)')
re_tx = re.compile(r'TX \(\d+ bytes\): ([0-9a-f ]+)')

events = []
i = 0
while i < len(lines):
    m = re_pkt.match(lines[i])
    if m and m.group(4) == '0x01' and m.group(3).strip() == 'OUT':
        idx = int(m.group(1))
        t = int(m.group(2))
        for j in range(i+1, min(i+4, len(lines))):
            mm = re_tx.search(lines[j])
            if mm:
                toks = mm.group(1).strip().split()
                while toks and toks[-1] == '00':
                    toks.pop()
                events.append((idx, t, toks))
                i = j
                break
    i += 1

HB = {'02 85 07 97', '02 85 04 9a'}
active = []
for (idx, t, toks) in events:
    sig6 = ' '.join(toks[:4])
    if sig6 not in HB:
        active.append((idx, t, toks))

print(f"{len(active)} active (non-heartbeat) commands out of {len(events)} OUT 0x01")
prev_t = active[0][1] if active else 0
for (idx, t, toks) in active:
    dt = (t - prev_t) / 1000.0  # ms
    sig = ' '.join(toks[:12])
    mark = " <-- BOUNDARY (>200ms)" if dt > 200 else ""
    print(f"[{idx:04d}] dt={dt:7.1f}ms  t={t/1e6:8.3f}s  {sig}{mark}")
    prev_t = t
