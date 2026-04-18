#!/usr/bin/env python3
"""Parse usb_trace.log produced by usb_interceptor.so and compute:
- per-endpoint packet counts and byte totals
- inter-packet gap distribution
- hotspot sequences (repeating patterns that look like a streaming poll loop)
- the key question: is EP 0x82 (waveform) read back-to-back with another
  read, or does it alternate with EP 0x01/0x81 command/ack traffic?

Usage: ./analyze_trace.py path/to/usb_trace.log [--head N]
"""
import re
import sys
from collections import Counter, defaultdict

LINE_RE = re.compile(
    r"^\[(\d+)\] t=(-?\d+) (SYNC|ASYNC) (BULK|CTRL|INTR|CTRL|IN ) .*EP 0x([0-9a-fA-F]{2})"
)


def parse(path):
    events = []  # list of (idx, t_us, dir, ep, extra)
    with open(path, "r", errors="replace") as f:
        for ln in f:
            m = LINE_RE.match(ln.strip())
            if not m:
                continue
            idx = int(m.group(1))
            t_us = int(m.group(2))
            kind = m.group(3)  # SYNC / ASYNC
            xfer = m.group(4)  # BULK / CTRL
            ep = int(m.group(5), 16)
            # detect direction from tag like 'OUT' or 'IN '
            if " OUT " in ln or " OUT EP" in ln:
                dir_ = "OUT"
            elif " IN " in ln or "IN  EP" in ln:
                dir_ = "IN"
            else:
                dir_ = "?"
            events.append((idx, t_us, kind, xfer, dir_, ep, ln.strip()))
    return events


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    events = parse(path)
    if not events:
        print("No events parsed.")
        return
    t0 = events[0][1]
    tN = events[-1][1]
    dur_ms = (tN - t0) / 1000.0
    print(f"Events: {len(events)}  duration: {dur_ms:.1f} ms")

    # Per-endpoint summary
    by_ep = Counter()
    for _, _, _, _, dir_, ep, _ in events:
        by_ep[(dir_, ep)] += 1
    print("\nPer endpoint:")
    for (d, ep), n in sorted(by_ep.items()):
        print(f"  {d:3} EP 0x{ep:02x}: {n}")

    # Gap analysis on EP 0x82 (waveform reads) — core metric
    wf_times = [t for _, t, _, _, d, ep, _ in events if ep == 0x82 and d == "IN"]
    if len(wf_times) >= 2:
        gaps = [wf_times[i + 1] - wf_times[i] for i in range(len(wf_times) - 1)]
        gaps.sort()
        q = lambda p: gaps[int(len(gaps) * p)]
        print(f"\nEP 0x82 (waveform) reads: {len(wf_times)}")
        print(f"  gap median  {q(0.5) / 1000:.3f} ms")
        print(f"  gap p10     {q(0.1) / 1000:.3f} ms")
        print(f"  gap p90     {q(0.9) / 1000:.3f} ms")
        print(f"  gap max     {gaps[-1] / 1000:.3f} ms")
        print(f"  gap min     {gaps[0] / 1000:.3f} ms")

    # Interleaving: for each EP 0x82 read, what was the previous 8 events?
    print("\nLast 10 events before each EP 0x82 read (first 5 occurrences):")
    wf_idxs = [i for i, e in enumerate(events) if e[5] == 0x82 and e[4] == "IN"]
    for k, i in enumerate(wf_idxs[:5]):
        print(f"  -- wf read at t={events[i][1] / 1000:.3f} ms:")
        for j in range(max(0, i - 10), i + 1):
            _, t, _, xfer, d, ep, _ = events[j]
            print(f"    t={t / 1000:8.3f} ms  {d:3} EP 0x{ep:02x} ({xfer})")

    # Look for repeating patterns (length 3..8) in the EP sequence
    epseq = [(ev[4], ev[5]) for ev in events]
    print("\nMost common 5-event windows:")
    w = 5
    wins = Counter(tuple(epseq[i:i + w]) for i in range(len(epseq) - w))
    for pat, n in wins.most_common(6):
        desc = " -> ".join(f"{d} 0x{ep:02x}" for d, ep in pat)
        print(f"  {n:5}×  {desc}")

    # Any simultaneous (overlapping) outstanding transfers? (async only hint)
    async_submits = [(t, d, ep) for _, t, kind, _, d, ep, _
                     in events if kind == "ASYNC"]
    if async_submits:
        print(f"\nAsync activity: {len(async_submits)} events — SDK uses async API.")


if __name__ == "__main__":
    main()
