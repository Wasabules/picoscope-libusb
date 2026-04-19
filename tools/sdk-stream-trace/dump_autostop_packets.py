#!/usr/bin/env python3
"""Dump full cmd1/cmd2/trigger for each autostop phase to find what
actually changes when auto_stop transitions 0→1."""
import re
from pathlib import Path

LOG = Path(__file__).parent / "sdk_autostop_trace.log"

PHASES = [
    ( 1,  100000,   1000, 0),
    ( 2,   10000,   1000, 1),
    ( 3,  100000,   1000, 1),
    ( 4, 1000000,   1000, 1),
    ( 5,  100000,    500, 1),
    ( 6,  100000,   2000, 1),
    ( 7,  100000,  10000, 1),
    ( 8,   10000,  10000, 1),
    ( 9,   50000,   1000, 1),
    (10,  200000,   5000, 1),
]

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
def is_cmd2(toks):
    return len(toks) >= 4 and toks[:2] == ['02', '85'] and toks[2] != '08' and toks[:4] != ['02', '85', '04', '9a']

# find cmd1 packets
cmd1_pos = [k for k, p in enumerate(packets) if is_cmd1(p[2])]

def chunkhex(toks, start=0):
    out = []
    for i, t in enumerate(toks):
        out.append(t)
        if (i - start + 1) % 8 == 0:
            out.append(' ')
    return ' '.join(out)

# Collect around each cmd1: the 6 packets before (to spot any auto_stop-specific
# preamble), cmd1 itself, the following cmd2 and trigger.
for (phase, ms, iv_ns, as_flag), k in zip(PHASES, cmd1_pos):
    print(f"\n========== PHASE {phase:>2} : ms={ms} iv={iv_ns}ns as={as_flag} ==========")
    # show 3 preceding packets and 6 following packets
    lo = max(0, k - 3)
    hi = min(len(packets), k + 8)
    for kk in range(lo, hi):
        toks = packets[kk][2]
        # trim trailing zeros for readability
        trimmed = list(toks)
        while trimmed and trimmed[-1] == '00':
            trimmed.pop()
        mark = ' *cmd1*' if kk == k else ''
        pad_len = len(toks) - len(trimmed)
        print(f"  [{packets[kk][0]:5d}]{mark:8s} n={len(toks)} pad0={pad_len}  {' '.join(trimmed[:40])}")

# Now dump the full cmd1 (all 64 bytes) for phase 1, 3 and 4 to compare
# auto_stop=0 vs auto_stop=1.
print("\n\n===== FULL cmd1 byte-by-byte comparison =====")
ref_cmd1 = packets[cmd1_pos[0]][2]  # phase 1 (as=0)
print(f"phase 1 (as=0, ms={PHASES[0][1]}, iv={PHASES[0][2]}ns): {' '.join(ref_cmd1)}")
for (phase, ms, iv_ns, as_flag), k in zip(PHASES[1:], cmd1_pos[1:]):
    cmd1 = packets[k][2]
    diffs = [(i, ref_cmd1[i], cmd1[i]) for i in range(min(len(ref_cmd1), len(cmd1)))
             if ref_cmd1[i] != cmd1[i]]
    print(f"phase {phase:>2}: ms={ms} iv={iv_ns} as={as_flag}")
    print(f"  diffs vs phase1: {diffs}")
