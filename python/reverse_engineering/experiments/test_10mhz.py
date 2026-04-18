#!/usr/bin/env python3
"""Test: capture d'un signal 10 MHz (OCXO) — vérification de faisabilité.
Sans signal réel, on vérifie que les captures à tb=0,1,2 fonctionnent
et on mesure la résolution temporelle."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
from picoscope_libusb_full import *
import numpy as np

scope = PicoScopeFull()
scope.open()

# OCXO typique: 0.5-1V pp → range 500mV ou 1V
# Pour le test sans signal, on utilise 1V (plus de marge)
scope.set_channel(Channel.A, True, Coupling.DC, Range.R_1V)
scope.set_channel(Channel.B, False, Coupling.DC, Range.R_5V)

print("\n" + "=" * 60)
print("Test de faisabilité: capture 10 MHz")
print("=" * 60)

for tb in [0, 1, 2]:
    info = scope.get_timebase_info(tb)
    interval_ns = info['sample_interval_ns']
    rate_mhz = info['sample_rate_hz'] / 1e6
    samples = 8064
    window_us = samples * interval_ns / 1000
    cycles_10mhz = window_us * 10  # 10 MHz = 10 cycles/µs

    print(f"\n--- Timebase {tb}: {interval_ns}ns = {rate_mhz:.0f} MS/s ---")
    print(f"    Fenêtre: {window_us:.1f} µs = {cycles_10mhz:.0f} cycles @ 10 MHz")
    print(f"    Points/cycle: {1000 / (interval_ns * 10):.1f}")

    scope.set_timebase(tb, samples)
    data = scope.capture_block(samples)

    if data and 'A' in data:
        s = data['A']
        print(f"    Capture OK: {len(s)} samples")
        print(f"    Min={s.min():.1f}mV, Max={s.max():.1f}mV, "
              f"Mean={s.mean():.1f}mV, Std={s.std():.1f}mV")

        # FFT pour voir s'il y a un signal
        fft = np.fft.rfft(s - s.mean())
        freqs = np.fft.rfftfreq(len(s), d=interval_ns * 1e-9)
        magnitudes = np.abs(fft)

        # Top 3 fréquences (hors DC)
        top_idx = np.argsort(magnitudes[1:])[-3:] + 1
        print(f"    Top freqs: ", end="")
        for idx in reversed(top_idx):
            f_mhz = freqs[idx] / 1e6
            mag = magnitudes[idx]
            print(f"{f_mhz:.2f}MHz({mag:.0f}) ", end="")
        print()

        if s.std() > 5:
            print(f"    → Signal détecté (std={s.std():.1f}mV)")
        else:
            print(f"    → Bruit seul (entrée flottante)")
    else:
        print(f"    ECHEC de capture")

    time.sleep(0.1)

# Test captures consécutives rapides à tb=0
print(f"\n--- Captures consécutives à tb=0 (100 MS/s) ---")
scope.set_timebase(0, 8064)
times = []
for i in range(10):
    t0 = time.time()
    data = scope.capture_block(8064)
    dt = (time.time() - t0) * 1000
    times.append(dt)
    if data and 'A' in data:
        ok = "OK"
    else:
        ok = "FAIL"
    if i < 3 or i == 9:
        print(f"    Capture {i+1:2d}: {dt:.0f}ms [{ok}]")

print(f"    Moyenne: {np.mean(times):.0f}ms/capture")
print(f"    10 captures: {sum(times)/1000:.1f}s total")

print(f"\n--- Résumé ---")
print(f"Pour un OCXO 10 MHz:")
print(f"  tb=0 (100 MS/s): 10 pts/cycle, 8064 samples = 806 cycles")
print(f"  tb=1 ( 50 MS/s):  5 pts/cycle, 8064 samples = 403 cycles")
print(f"  Recommandé: tb=0 ou tb=1 avec range 500mV ou 1V")

scope.close()
print("\nDone!")
