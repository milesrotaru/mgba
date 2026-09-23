#!/usr/bin/env python3
# Compares two gsf2wav renders of the same track: lag, gain and residual,
# both lowpassed to a common band (default 7 kHz, under MP2K's usual Nyquist).
#   compare.py a.wav b.wav [cutoff_hz]
import struct, sys
import numpy as np

def load(p):
    d = open(p, 'rb').read()
    fs = struct.unpack('<I', d[24:28])[0]
    i = d.index(b'data')
    n = struct.unpack('<I', d[i + 4:i + 8])[0]
    return np.frombuffer(d[i + 8:i + 8 + n], dtype='<f4').reshape(-1, 2).astype(np.float64), fs

def lowpass(x, fs, fc):
    X = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(len(x), 1 / fs)
    X[f > fc] = 0
    X[f < 20] = 0  # the driver's floor() leaves a DC offset the high-precision mix doesn't have
    return np.fft.irfft(X, len(x), axis=0)

def main():
    a, fs = load(sys.argv[1])
    b, _ = load(sys.argv[2])
    fc = float(sys.argv[3]) if len(sys.argv) > 3 else 7000
    n = min(len(a), len(b))
    a, b = lowpass(a[:n], fs, fc), lowpass(b[:n], fs, fc)
    m = a[:, 0] + a[:, 1]
    k = b[:, 0] + b[:, 1]
    c = np.fft.irfft(np.fft.rfft(m, 2 * n) * np.conj(np.fft.rfft(k, 2 * n)))
    lag = int(np.argmax(np.abs(c)))
    if lag > n:
        lag -= 2 * n
    print(f'lag of A relative to B: {lag} samples ({lag / fs * 1000:.3f} ms)')
    if lag > 0:
        a = a[lag:]
        b = b[:len(a)]
    elif lag < 0:
        b = b[-lag:]
        a = a[:len(b)]
    for ch, name in ((0, 'L'), (1, 'R')):
        g = np.dot(a[:, ch], b[:, ch]) / np.dot(b[:, ch], b[:, ch])
        res = a[:, ch] - b[:, ch]
        print(f'{name}: gain A/B {20 * np.log10(g):+.3f} dB, residual (A-B) {10 * np.log10(np.mean(res ** 2) / np.mean(b[:, ch] ** 2)):.1f} dB rel. B')

if __name__ == '__main__':
    main()
