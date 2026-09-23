#!/usr/bin/env python3
# Octave-band energy of one or more WAVs (dB relative to the first file's total).
#   bands.py a.wav [b.wav ...]
import struct, sys
import numpy as np

def load(p):
    d = open(p, 'rb').read()
    fs = struct.unpack('<I', d[24:28])[0]
    i = d.index(b'data')
    n = struct.unpack('<I', d[i + 4:i + 8])[0]
    x = np.frombuffer(d[i + 8:i + 8 + n], dtype='<f4').reshape(-1, 2).astype(np.float64)
    return x[:, 0] + x[:, 1], fs

edges = [20, 125, 250, 500, 1000, 2000, 4000, 6000, 7000, 7900, 9000, 12000, 16000, 20000, 24000]
ref = None
print('band (Hz)      ' + ''.join(f'{p.split("/")[-1][:14]:>16}' for p in sys.argv[1:]))
rows = []
for p in sys.argv[1:]:
    x, fs = load(p)
    X = np.abs(np.fft.rfft(x)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / fs)
    tot = X[(f > 20)].sum()
    if ref is None:
        ref = tot
    rows.append([10 * np.log10(X[(f >= lo) & (f < hi)].sum() / ref + 1e-30) for lo, hi in zip(edges, edges[1:])])
for i, (lo, hi) in enumerate(zip(edges, edges[1:])):
    print(f'{lo:>6}-{hi:<6}  ' + ''.join(f'{r[i]:16.1f}' for r in rows))
