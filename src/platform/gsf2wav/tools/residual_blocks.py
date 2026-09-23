#!/usr/bin/env python3
# Residual (A-B, both lowpassed to a common band) per 20 ms block, to find
# localized mismatches like clicks, missed notes or desyncs.
#   residual_blocks.py a.wav b.wav [cutoff_hz] [top_n]
import sys
import numpy as np
from compare import load, lowpass

a, fs = load(sys.argv[1])
b, _ = load(sys.argv[2])
fc = float(sys.argv[3]) if len(sys.argv) > 3 else 7000
top = int(sys.argv[4]) if len(sys.argv) > 4 else 10
n = min(len(a), len(b))
a = lowpass(a[:n], fs, fc).sum(1)
b = lowpass(b[:n], fs, fc).sum(1)
blk = int(fs * 0.02)
rows = []
for i in range(0, n - blk, blk):
    s = np.mean(b[i:i + blk] ** 2)
    r = np.mean((a[i:i + blk] - b[i:i + blk]) ** 2)
    if s > 1e-8:
        rows.append((10 * np.log10(r / s), i / fs, 10 * np.log10(s)))
# Only blocks within 30 dB of the loudest matter; quieter ones are dominated by
# the driver's 8-bit noise floor
loud = max(r[2] for r in rows)
rows = [r for r in rows if r[2] > loud - 30]
v = np.array([r[0] for r in rows])
print(f'blocks: {len(rows)}, residual/signal median {np.median(v):.1f} dB, 90th pct {np.percentile(v, 90):.1f}, max {v.max():.1f}')
for r in sorted(rows, reverse=True)[:top]:
    print(f'  t={r[1]:7.2f}s  residual {r[0]:6.1f} dB rel. signal (signal {r[2]:.1f} dB)')
