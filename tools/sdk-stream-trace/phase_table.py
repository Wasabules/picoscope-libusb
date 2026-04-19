#!/usr/bin/env python3
"""
Build a per-phase table: for each streaming cycle in the trace,
dump the cmd1 (85 08 85) and cmd2 (85 0c 86) bytes, plus any per-phase
special commands (9B LUT prime, ADV trigger, etc).

A streaming cycle = {cmd1=02 85 08 85..., cmd2=02 85 0c 86..., trigger=02 07 06..., stop=02 0a 00 85 04 99...}

Each cycle is assigned the N-th phase label from the program, in order.
"""
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
        t = int(m.group(2))
        for j in range(i+1, min(i+4, len(lines))):
            mm = re_tx.search(lines[j])
            if mm:
                toks = mm.group(1).strip().split()
                while toks and toks[-1] == '00':
                    toks.pop()
                events.append((t, toks))
                i = j
                break
    i += 1

PHASE_LABELS = [
    "baseline A+B DC 5V 1us",
    "A only B disabled",
    "B only A disabled",
    "A=100mV B=5V",
    "A=200mV B=5V",
    "A=500mV B=5V",
    "A=1V B=5V",
    "A=2V B=5V",
    "A=20V B=5V",
    "A=5V B=200mV",
    "A AC 5V B DC 5V",
    "A DC 5V B AC 5V",
    "ival=500ns=2MSps",
    "ival=2us=500kSps",
    "ival=5us=200kSps",
    "ival=10us=100kSps",
    "max_samples=10000",
    "max_samples=1000000",
    "overview_buf=1000 (FAILED at runtime)",
    "overview_buf=500000",
    "aggregate=0 (FAILED at runtime)",
    "auto_stop=1",
    "siggen sine 1kHz 2Vpp (no stream)",
    "siggen+stream sine 1kHz",
    "siggen square 10kHz 4Vpp +500mV (no stream)",
    "siggen DC +1V (no stream)",
    "siggen OFF (no stream)",
    "legacy run_streaming(10ms, 1000, 1)",
    "run_block timebase=3 2048 samples",
]

# Identify cycles: a cycle = cmd1 (sig starts 02 85 08 85) followed by cmd2 (02 85 0c 86)
# followed by trigger (02 07 06) followed by stop (02 0a 00 85 04 99).
cycles = []
i = 0
while i < len(events):
    (t, toks) = events[i]
    if len(toks) >= 4 and ' '.join(toks[:4]) == '02 85 08 85':
        cmd1 = toks
        cmd1_t = t
        cmd2 = None
        trig = None
        stop = None
        stop_t = None
        for j in range(i+1, min(i+10, len(events))):
            (tj, tj_toks) = events[j]
            if len(tj_toks) >= 4 and ' '.join(tj_toks[:4]) == '02 85 0c 86':
                cmd2 = tj_toks
            elif len(tj_toks) >= 3 and ' '.join(tj_toks[:3]) == '02 07 06':
                trig = tj_toks
        # find stop within reasonable time after cmd1
        for j in range(i+1, len(events)):
            (tj, tj_toks) = events[j]
            if tj - cmd1_t > 10_000_000:  # 10s after cmd1
                break
            if ' '.join(tj_toks[:5]) == '02 0a 00 85 04':
                stop = tj_toks
                stop_t = tj
                break
        cycles.append({
            't': cmd1_t,
            'cmd1': cmd1,
            'cmd2': cmd2,
            'trig': trig,
            'stop': stop,
            'stop_t': stop_t,
        })
        i = j if stop else i+1
    else:
        i += 1

print(f"Found {len(cycles)} streaming cycles\n")
print(f"{'#':>3}  {'t(s)':>7}  {'label':<38}  cmd1 (85 08 85 body)                             cmd2 (85 0c 86 body)")
print("-" * 150)
for k, c in enumerate(cycles):
    label = PHASE_LABELS[k] if k < len(PHASE_LABELS) else f"(cycle {k+1})"
    cmd1_body = ' '.join(c['cmd1'][1:]) if c['cmd1'] else "(none)"
    cmd2_body = ' '.join(c['cmd2'][1:]) if c['cmd2'] else "(none)"
    print(f"{k+1:>3}  {c['t']/1e6:7.1f}  {label:<38}  {cmd1_body:<48}  {cmd2_body}")

# Emit a diff table: look at per-byte variation across cmd1 bodies.
print("\n=== per-byte VARIATION across cmd1 bodies (bytes that differ) ===")
max_len = max(len(c['cmd1']) for c in cycles if c['cmd1'])
print(f"max cmd1 length: {max_len}")
for bpos in range(1, max_len):
    vals = set()
    for c in cycles:
        if c['cmd1'] and bpos < len(c['cmd1']):
            vals.add(c['cmd1'][bpos])
    if len(vals) > 1:
        # show which value per phase
        row = []
        for k, c in enumerate(cycles):
            if c['cmd1'] and bpos < len(c['cmd1']):
                row.append(c['cmd1'][bpos])
            else:
                row.append('--')
        print(f"  cmd1 byte {bpos:2d}: {len(vals)} distinct values  {row}")

print("\n=== per-byte VARIATION across cmd2 bodies (bytes that differ) ===")
valid = [c for c in cycles if c['cmd2']]
if valid:
    max_len = max(len(c['cmd2']) for c in valid)
    for bpos in range(1, max_len):
        vals = set()
        for c in valid:
            if bpos < len(c['cmd2']):
                vals.add(c['cmd2'][bpos])
        if len(vals) > 1:
            row = []
            for c in valid:
                if bpos < len(c['cmd2']):
                    row.append(c['cmd2'][bpos])
                else:
                    row.append('--')
            print(f"  cmd2 byte {bpos:2d}: {len(vals)} distinct values  {row}")
