#!/usr/bin/env python3
"""
Extract idle-period heartbeat traffic from the SDK parametric trace.

Idle = gaps >1s between the 'active' command chains. We classify every
EP 0x01 OUT command by its first 4 bytes and tabulate cadence/content
during long quiet windows.
"""
import re
import sys
from pathlib import Path
from collections import Counter, defaultdict

DEFAULT = Path(__file__).parent / "sdk_param_trace.log"
log = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT

re_pkt = re.compile(r'^\[(\d+)\] t=(\d+) (?:SYNC|ASYNC) BULK OUT EP 0x01')
re_tx = re.compile(r'TX \(\d+ bytes\): ([0-9a-f ]+)')

lines = log.read_text().splitlines()
packets = []  # (idx, t_us, sig, payload_trimmed)
i = 0
while i < len(lines):
    m = re_pkt.match(lines[i])
    if m:
        idx = int(m.group(1)); t = int(m.group(2))
        for j in range(i + 1, min(i + 4, len(lines))):
            mm = re_tx.search(lines[j])
            if mm:
                toks = mm.group(1).strip().split()
                payload = list(toks)
                while payload and payload[-1] == '00':
                    payload.pop()
                # Signature — the 3-5 bytes that usually identify the subcommand.
                if len(payload) >= 4 and payload[0] == '02':
                    sig = ' '.join(payload[1:4])
                else:
                    sig = ' '.join(payload[:4])
                packets.append((idx, t, sig, payload))
                i = j
                break
    i += 1

print(f"Total EP 0x01 OUT packets: {len(packets)}")

# Overall sig frequency
sigs = Counter(p[2] for p in packets)
print("\nOverall signature frequency (top 12):")
for sig, n in sigs.most_common(12):
    print(f"  {n:>5}  {sig}")

# Define 'heartbeat' signatures empirically: the ones that appear at roughly
# uniform cadence during idle periods, not tied to phase starts.
HEARTBEAT_SIGS = {'85 07 97', '85 04 9a', '85 04 80'}

# Find idle runs = stretches where only heartbeat sigs appear for >= 2s
heartbeats = [p for p in packets if p[2] in HEARTBEAT_SIGS]
actives    = [p for p in packets if p[2] not in HEARTBEAT_SIGS]

print(f"\nHeartbeat packets: {len(heartbeats)}")
print(f"Active  packets:   {len(actives)}")

# Cadence of each heartbeat sig
print("\nCadence per heartbeat signature:")
for sig in sorted(HEARTBEAT_SIGS):
    ts = [p[1] for p in heartbeats if p[2] == sig]
    if len(ts) < 2:
        continue
    dts = [ts[i+1] - ts[i] for i in range(len(ts)-1)]
    # Filter out gaps >5s (stream periods) to get the true cadence
    dts_short = [d for d in dts if d < 5_000_000]
    if dts_short:
        avg = sum(dts_short) / len(dts_short)
        print(f"  {sig}: n={len(ts)}, avg gap {avg/1000:.1f} ms "
              f"(min {min(dts_short)/1000:.1f}, max {max(dts_short)/1000:.1f}, "
              f"outliers_removed={len(dts)-len(dts_short)})")

# Long idle gaps (>2s between active commands)
print("\nLong quiet windows (>2s between active packets):")
for i in range(1, len(actives)):
    gap = actives[i][1] - actives[i-1][1]
    if gap > 2_000_000:
        t0 = actives[i-1][1]
        t1 = actives[i][1]
        hb_count = sum(1 for h in heartbeats if t0 < h[1] < t1)
        print(f"  t={t0/1e6:7.2f}..{t1/1e6:7.2f}s  gap {gap/1e6:6.2f}s  "
              f"{hb_count:4d} heartbeats in between  "
              f"(idx {actives[i-1][0]} → {actives[i][0]})")

# Payload diversity inside heartbeats
print("\nContent diversity inside the 85 07 97 heartbeat:")
b_payloads = Counter()
for p in heartbeats:
    if p[2] == '85 07 97':
        b_payloads[' '.join(p[3])] += 1
for payload, n in b_payloads.most_common(8):
    print(f"  {n:>5}  {payload}")
