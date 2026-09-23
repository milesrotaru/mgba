#!/usr/bin/env python3
# Crude click detector: energy above a highpass in 1 ms windows, flagging
# windows far above the median of the surrounding 200 ms.
#   clicks.py file.wav [highpass_hz] [threshold_db]
import struct, sys
import numpy as np
from compare import load

def main():
    x, fs = load(sys.argv[1])
    hp = float(sys.argv[2]) if len(sys.argv) > 2 else 8000
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else 15
    s = x.sum(1)
    S = np.fft.rfft(s)
    f = np.fft.rfftfreq(len(s), 1 / fs)
    S[f < hp] = 0
    h = np.fft.irfft(S, len(s))
    w = int(fs * 0.001)
    e = np.array([np.mean(h[i:i + w] ** 2) for i in range(0, len(h) - w, w)]) + 1e-20
    ctx = 100
    hits = []
    for i in range(len(e)):
        lo, hi = max(0, i - ctx), min(len(e), i + ctx)
        med = np.median(e[lo:hi])
        r = 10 * np.log10(e[i] / med)
        if r > thr and 10 * np.log10(e[i]) > -110:
            hits.append((i * w / fs, r, 10 * np.log10(e[i])))
    print(f'{sys.argv[1]}: {len(hits)} windows > {thr} dB above local median (highpass {hp:.0f} Hz)')
    for t, r, lv in hits[:8]:
        print(f'  t={t:8.3f}s  +{r:.1f} dB (level {lv:.1f} dB)')

if __name__ == '__main__':
    main()
