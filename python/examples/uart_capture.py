#!/usr/bin/env python3
"""
Capture UART signal from /dev/ttyUSB0 on PicoScope CH A.
Sends 0x55 (alternating bit pattern) at 9600 baud, captures 3 blocks,
analyzes voltage levels, edges and bit period.
"""
import os
import sys
import time
import threading
import numpy as np
import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from picoscope_libusb_full import PicoScopeFull, Channel, Coupling, Range

BAUD = 9600
BIT_NS = int(1e9 / BAUD)
RANGE_MV = 5000
TB = int(os.environ.get('TB', '6'))  # 6 → 640 ns/sample, 5.16 ms window
DT_NS = 10 * (2**TB)

stop_tx = threading.Event()


TX_PAYLOAD = os.environ.get('TX_PAYLOAD', 'U').encode('latin-1')


def tx_loop():
    ser = serial.Serial('/dev/ttyUSB0', baudrate=BAUD, bytesize=8,
                        parity='N', stopbits=1, timeout=0)
    data = TX_PAYLOAD * max(1, 128 // len(TX_PAYLOAD))
    while not stop_tx.is_set():
        ser.write(data)
        ser.flush()
        time.sleep(0.005)
    ser.close()


def analyze(samples_mv, dt_ns):
    s = np.asarray(samples_mv, dtype=np.float32)
    vmin, vmax = float(s.min()), float(s.max())
    vpp = vmax - vmin
    vmid = (vmin + vmax) / 2.0

    # Schmitt thresholds ±20 % of Vpp around mid
    hi = vmid + 0.20 * vpp
    lo = vmid - 0.20 * vpp

    logic = np.zeros(s.size, dtype=np.uint8)
    state = 1 if s[0] > vmid else 0
    for i, v in enumerate(s):
        if state == 0 and v >= hi:
            state = 1
        elif state == 1 and v <= lo:
            state = 0
        logic[i] = state

    d = np.diff(logic.astype(np.int8))
    edges = np.nonzero(d)[0] + 1
    if edges.size < 2:
        return dict(vmin=vmin, vmax=vmax, vpp=vpp, edges=edges.size,
                    bit_ns=None, baud_est=None)

    gaps_samples = np.diff(edges)
    gaps_ns = gaps_samples * dt_ns
    # The shortest gap should be one bit (since 0x55 = alternating bits)
    bit_ns_est = float(np.min(gaps_ns))
    baud_est = 1e9 / bit_ns_est if bit_ns_est > 0 else None

    return dict(
        vmin=vmin, vmax=vmax, vpp=vpp,
        edges=int(edges.size),
        shortest_gap_ns=float(np.min(gaps_ns)),
        median_gap_ns=float(np.median(gaps_ns)),
        bit_ns_est=bit_ns_est,
        baud_est=baud_est,
    )


def main():
    print(f"[uart] BAUD={BAUD}, expected bit period={BIT_NS} ns")
    print(f"[uart] Scope: TB={TB} ({DT_NS} ns/sample), range=±{RANGE_MV/1000} V")
    print(f"[uart] Capture span = {8064*DT_NS/1e6:.2f} ms "
          f"(≈{8064*DT_NS/1e6 / (10 * BIT_NS / 1e6):.1f} UART frames)")

    tx = threading.Thread(target=tx_loop, daemon=True)
    tx.start()
    time.sleep(0.1)

    scope = PicoScopeFull()
    try:
        scope.open()
        scope.set_channel(Channel.A, True, Coupling.DC, Range.R_5V)
        scope.set_channel(Channel.B, False)
        scope.set_timebase(TB, samples=8064)
        scope.set_trigger(auto_trigger_ms=200)

        for k in range(3):
            t0 = time.time()
            res = scope.capture_block(samples=8064, channel=Channel.A)
            t1 = time.time()
            if res is None or 'A' not in res:
                print(f"[block {k}] capture failed")
                continue
            a = res['A']
            stats = analyze(a, DT_NS)
            print(f"\n[block {k}] {len(a)} samples in {(t1-t0)*1e3:.1f} ms")
            print(f"  Vmin={stats['vmin']:+.0f} mV  Vmax={stats['vmax']:+.0f} mV  "
                  f"Vpp={stats['vpp']:.0f} mV")
            print(f"  edges={stats['edges']}  "
                  f"shortest_gap={stats.get('shortest_gap_ns', 0):.0f} ns  "
                  f"median_gap={stats.get('median_gap_ns', 0):.0f} ns")
            if stats.get('bit_ns_est'):
                print(f"  bit_ns_est={stats['bit_ns_est']:.0f} ns  "
                      f"baud_est={stats['baud_est']:.0f}  "
                      f"(expected {BAUD})")

            np.save(f'/tmp/uart_block_{k}.npy', np.asarray(a, dtype=np.float32))

        # Dump first 400 samples of block 0 for eyeball check
        a0 = np.load('/tmp/uart_block_0.npy')
        print(f"\n[preview] first 80 samples of block 0 (each = {DT_NS} ns):")
        for i in range(0, 80, 8):
            row = a0[i:i+8]
            print("  " + " ".join(f"{v:+5.0f}" for v in row))

    finally:
        stop_tx.set()
        scope.close()
        tx.join(timeout=1.0)


if __name__ == '__main__':
    main()
