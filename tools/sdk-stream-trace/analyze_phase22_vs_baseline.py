#!/usr/bin/env python3
"""
Diff the command chains around the phase 22 streaming cycle
(auto_stop=1) against an auto_stop=0 baseline (e.g. phase 13). The goal
is to discover any extra commands the SDK issues when auto_stop is set
that our driver currently omits.
"""
import re
from pathlib import Path

log = Path(__file__).parent / "sdk_param_trace.log"

re_pkt = re.compile(r'^\[(\d+)\] t=(\d+) (?:SYNC|ASYNC) BULK OUT EP 0x01')
re_tx = re.compile(r'TX \(\d+ bytes\): ([0-9a-f ]+)')

lines = log.read_text().splitlines()
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
                trimmed = list(toks)
                while trimmed and trimmed[-1] == '00':
                    trimmed.pop()
                packets.append((idx, t, trimmed))
                i = j
                break
    i += 1

HEARTBEATS = {
    ('85', '07', '97'),
    ('85', '04', '9a'),
    ('85', '04', '80'),
}

def is_heartbeat(toks):
    if len(toks) < 4 or toks[0] != '02':
        return False
    return (toks[1], toks[2], toks[3]) in HEARTBEATS

# Locate streaming cycle index by iterating cmd1 packets (02 85 08 85)
cycles = []
for k, (idx, t, toks) in enumerate(packets):
    if len(toks) >= 4 and toks[:4] == ['02', '85', '08', '85']:
        cycles.append(k)

print(f"Found {len(cycles)} cmd1 packets\n")

# Phase N cmd1 is at cycles[N-1] (for N <= 22). Collect non-heartbeat
# packets from cycles[N-1]-2 (pre-cmd1 prelude) up to next cycles[N] or
# first heartbeat run.
def get_phase_packets(phase_index):
    start_k = cycles[phase_index - 1]
    # Extend 4 packets before in case there's a prelude
    lo = max(0, start_k - 4)
    # Extend forward until we hit the next cmd1 or a stop packet
    hi = len(packets)
    for k in range(start_k + 1, len(packets)):
        tk = packets[k][2]
        # Next cmd1 = new phase
        if len(tk) >= 4 and tk[:4] == ['02', '85', '08', '85']:
            hi = k
            break
        # `02 0a 00 85 04 99` = stop, include it
        if len(tk) >= 6 and tk[:6] == ['02', '0a', '00', '85', '04', '99']:
            hi = k + 1
            break
    return [p for p in packets[lo:hi] if not is_heartbeat(p[2])]

def fmt_packet(idx, t, toks, ref_t):
    trimmed = ' '.join(toks[:16]) + ('…' if len(toks) > 16 else '')
    dt = (t - ref_t) / 1000.0
    return f"[{idx:4d}] +{dt:8.1f}ms  {trimmed}"

for phase in [13, 22]:
    print(f"===== PHASE {phase} (cmd1-centred) =====")
    pkts = get_phase_packets(phase)
    if not pkts: continue
    ref_t = pkts[0][1]
    for (idx, t, toks) in pkts:
        print("  " + fmt_packet(idx, t, toks, ref_t))
    print()

# Direct diff: side-by-side
p13 = get_phase_packets(13)
p22 = get_phase_packets(22)
print(f"===== SIDE-BY-SIDE: phase13 (n={len(p13)}) vs phase22 (n={len(p22)}) =====")
for i in range(max(len(p13), len(p22))):
    a = ' '.join(p13[i][2][:14]) if i < len(p13) else '—'
    b = ' '.join(p22[i][2][:14]) if i < len(p22) else '—'
    mark = '   ' if a == b else ' ≠ '
    print(f"  {i:2d}{mark}{a:<45}  vs  {b}")
