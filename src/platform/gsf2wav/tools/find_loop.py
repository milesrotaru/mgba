#!/usr/bin/env python3
# Finds an MP2K track's loop from a long gsf2wav render. The sequencer ticks
# once per video frame, so the loop is a whole number of frames; the period is
# found by envelope autocorrelation, snapped to frames, and the loop start is
# the earliest time from which the audio repeats one period later (compared
# with a fractional-sample shift, since a frame count rarely lands on a whole
# number of output samples).
#   find_loop.py long.wav [min_period_s]
#
# The period estimate is reliable; the loop start is not yet (on 006 it
# reported 226 s for a 52.7 s loop). Reading the sequencer's GOTO would be
# the robust way.
import sys
import numpy as np
from compare import load

FRAME = 280896 / 16777216  # seconds per GBA video frame

def main():
    x, fs = load(sys.argv[1])
    minp = float(sys.argv[2]) if len(sys.argv) > 2 else 10
    m = x.sum(1)
    hop = int(fs * 0.01)
    env = np.sqrt(np.convolve(m ** 2, np.ones(hop) / hop, 'same')[::hop])
    e = env - env.mean()
    ac = np.correlate(e, e, 'full')[len(e) - 1:]
    ac /= ac[0]
    lo = int(minp * 100)
    lag = (lo + int(np.argmax(ac[lo:len(e) // 2]))) / 100
    # snap to whole frames, trying neighbours by waveform residual
    best = None
    n = len(m)
    M = np.fft.rfft(m)
    f = np.fft.rfftfreq(n, 1 / fs)
    for frames in range(int(round(lag / FRAME)) - 3, int(round(lag / FRAME)) + 4):
        p = frames * FRAME
        shifted = np.fft.irfft(M * np.exp(2j * np.pi * f * p), n)  # shifted[t] = m[t + p]
        valid = n - int(np.ceil(p * fs)) - 1
        blk = int(fs * 0.5)
        res = []
        for i in range(0, valid - blk, blk):
            s = np.mean(m[i:i + blk] ** 2)
            r = np.mean((m[i:i + blk] - shifted[i:i + blk]) ** 2)
            res.append(10 * np.log10(r / s) if s > 1e-12 else -200)
        res = np.array(res)
        # loop start: first block after which everything within the valid range repeats
        good = res < -40
        start = None
        for i in range(len(good)):
            if good[i:].all():
                start = i
                break
        score = np.median(res[start:]) if start is not None else 0
        if best is None or score < best[0]:
            best = (score, frames, p, start, res)
    score, frames, p, start, res = best
    if start is None:
        print(f'period ~{p:.3f} s ({frames} frames) but no clean repeat found; worst block {res.max():.1f} dB')
        return
    t0 = start * 0.5
    print(f'period {p:.4f} s ({frames} frames); loop starts by {t0:.1f} s (0.5 s resolution); '
          f'repeat residual median {score:.1f} dB; intro + 2 loops = {t0 + 2 * p:.3f} s')

if __name__ == '__main__':
    main()
