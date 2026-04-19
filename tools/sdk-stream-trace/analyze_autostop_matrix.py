#!/usr/bin/env python3
"""
Decode how the SDK packs (max_samples, interval) into cmd1 when auto_stop=1.

Walks the sdk_autostop_trace.log file, locates each phase by the N
consecutive `ps2000_flash_led` commands (in-band marker), and extracts
the cmd1 packet that follows the phase's run_streaming_ns call.

For each phase, prints:
  - the expected (max_samples, interval, auto_stop) from the test script
  - the observed cmd1[6..10] (5-byte BE sample count)
  - the observed cmd1[28..30] (3-byte BE interval field)
  - the observed cmd2[47], cmd2[48]

Then tries to fit the encoding:
  hypothesis A: cmd1[28..30] = max_samples * interval_ns / 100 - 1
  hypothesis B: cmd1[28..30] = max_samples * interval_ticks / 10 - 1
  hypothesis C: cmd1[28..30] = max_samples / 10 - 1  (independent of interval)
  hypothesis D: cmd1[28..30] = max_samples (identity)
"""
import re
from pathlib import Path

LOG = Path(__file__).parent / "sdk_autostop_trace.log"

# Phase script (mirrors sdk_autostop_matrix.c)
PHASES = [
    # (phase, max_samples, interval_ns, auto_stop)
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

print(f"Parsed {len(packets)} EP 0x01 OUT packets.")

# flash_led opcode = 02 06 00 85 04 98 00 (byte signature) — walks 4 bytes
# Actually let's dump first few unique headers to find flash_led.
from collections import Counter
heads = Counter(' '.join(p[2][:5]) for p in packets)
print("\nTop 10 header signatures:")
for h, n in heads.most_common(10):
    print(f"  {n:>4}  {h}")

# Flash LED signature (from phase_table.py reading): try to match.
# In the existing sdk_param_trace, flash_led = '02 04 00 85 04 98' typically.
FLASH_SIG_PREFIXES = [
    ('02', '06', '00', '85', '04', '98'),
    ('02', '04', '00', '85', '04', '98'),
    ('02', '05', '00', '85', '04', '98'),
]

def is_flash(toks):
    if len(toks) < 6: return False
    return tuple(toks[:6]) in FLASH_SIG_PREFIXES

# cmd1 signature: 02 85 08 85 …
def is_cmd1(toks):
    return len(toks) >= 4 and toks[:4] == ['02', '85', '08', '85']

# cmd2 signature: the long compound starting with 02 85 (not 08 85 08 89 etc)
# The SDK's cmd2 is characteristically large (>30 bytes of content). Easier:
# we pick the packet right after cmd1 that starts with 02 and isn't flash.
def is_cmd2(toks):
    return len(toks) >= 4 and toks[:2] == ['02', '85'] and toks[2] != '08'

# Walk packets, detect runs of flash_led → phase index = run length.
# Then, after the run + some setup, find cmd1 and record it.
phase_markers = []  # list of (phase_index, packet_index_after_run)
k = 0
while k < len(packets):
    if is_flash(packets[k][2]):
        run_start = k
        while k < len(packets) and is_flash(packets[k][2]):
            k += 1
        run_len = k - run_start
        phase_markers.append((run_len, k))  # k is index AFTER the run
    else:
        k += 1

print(f"\nDetected {len(phase_markers)} flash-LED phase markers:")
for (n, pos) in phase_markers[:15]:
    print(f"  phase {n:2d} starts at pkt idx {pos}")

# Simpler matcher: there are len(PHASES) cmd1 packets in the trace (one per
# run_streaming_ns); pair them positionally with PHASES.
cmd1_positions = [k for k, p in enumerate(packets) if is_cmd1(p[2])]
print(f"\nFound {len(cmd1_positions)} cmd1 packets (expected {len(PHASES)}).")
rows = []
for (phase, ms, iv_ns, as_flag), k in zip(PHASES, cmd1_positions):
    cmd1 = packets[k][2]
    cmd2 = None
    for jj in range(k + 1, min(k + 12, len(packets))):
        if is_cmd2(packets[jj][2]):
            cmd2 = packets[jj][2]
            break
    rows.append((phase, ms, iv_ns, as_flag, cmd1, cmd2))

def hex_to_int(toks, lo, hi_incl):
    """Convert toks[lo..hi_incl] (inclusive) to big-endian integer."""
    val = 0
    for t in toks[lo:hi_incl + 1]:
        val = (val << 8) | int(t, 16)
    return val

print("\n" + "=" * 100)
print(f"{'Phase':>5}  {'ms':>8}  {'iv_ns':>6}  {'as':>3}  {'cmd1[6..10] (5B BE)':>22}  {'cmd1[28..30] (3B BE)':>22}  {'c2[47]':>6}  {'c2[48]':>6}")
print("=" * 100)
data = []
for (phase, ms, iv_ns, as_flag, cmd1, cmd2) in rows:
    sc = hex_to_int(cmd1, 6, 10)
    iv = hex_to_int(cmd1, 28, 30)
    c2_47 = cmd2[47] if cmd2 and len(cmd2) > 47 else '--'
    c2_48 = cmd2[48] if cmd2 and len(cmd2) > 48 else '--'
    print(f"{phase:>5}  {ms:>8}  {iv_ns:>6}  {as_flag:>3}  {sc:>22}  {iv:>22}  {c2_47:>6}  {c2_48:>6}")
    data.append((phase, ms, iv_ns, as_flag, sc, iv))

print("\n" + "=" * 100)
print("Hypothesis test for cmd1[28..30] when auto_stop=1 (ignoring phase 1):")
print("=" * 100)

def fmt_pass(ok):
    return 'OK   ' if ok else 'FAIL '

for (phase, ms, iv_ns, as_flag, sc, iv) in data:
    if as_flag == 0: continue
    iv_ticks = iv_ns // 10
    hA = ms * iv_ns // 100 - 1               # total_ns / 100 − 1
    hB = ms * iv_ticks // 10 - 1             # same, expressed in ticks
    hC = ms // 10 - 1
    hD = ms
    hE = ms * iv_ticks - 1                   # total ticks − 1
    hF = ms * iv_ns // 10 - 1                # total ns / 10 − 1 (= total ticks − 1)
    hG = (ms * iv_ns) // 1000 - 1            # total µs − 1
    print(f"phase {phase:>2}: ms={ms:>7}  iv_ns={iv_ns:>5}  observed_iv={iv:>9}  "
          f"[A:total/100-1={hA:>9} {fmt_pass(hA==iv)}]  "
          f"[E:ticks-1={hE:>9} {fmt_pass(hE==iv)}]  "
          f"[G:µs-1={hG:>9} {fmt_pass(hG==iv)}]")
