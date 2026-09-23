#!/usr/bin/env python3
# Ranks MP2K samples by simple noisiness metrics computed from the ROM data.
#
# Caveat: on Mother 3 this does NOT find the one sample known to sound gritty
# in sinc mode (the organ, 082ED1BC, ranks ~50th of 306). The top of the list
# is percussion, which is noisy by design. Kept as a starting point; see the
# commit that added it for what was tried. Metrics:
#   jag    RMS of adjacent-sample differences over RMS level (white noise ~1.4)
#   top    share of energy above 0.35x the sample's rate
#   flat   spectral flatness of the upper half-band (1 = noise-like/aliased,
#          near 0 = clean harmonics)
#   loopdb loop level in dB over the 8-bit quantization floor
#   pitch  mean playback rate over the driver's mixing rate (from
#          sample_stats.py): how far the game pitches it up
#   score  combines them; higher = more suspect
#   sample_noise.py SET_DIR stats.csv [driver_rate]
import csv, glob, os, struct, sys
import numpy as np
import gsfpy

def analyze(rom, wav):
    o = wav & 0x1FFFFFF
    typ, flags, freq, ls, size = struct.unpack('<HHIII', rom[o:o + 16])
    if size <= 16 or o + 16 + size > len(rom):
        return None
    d = np.frombuffer(rom[o + 16:o + 16 + size], dtype=np.int8).astype(float)
    looped = bool(flags & 0xC000) and 0 <= ls < size
    body = d[:min(size, 8192)]
    loop = d[ls:] if looped and size - ls >= 64 else body
    if not np.any(loop):
        loop = body
    rms = np.sqrt(np.mean(body ** 2)) + 1e-9
    jag = np.sqrt(np.mean(np.diff(body) ** 2)) / rms
    seg = body - body.mean()
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2 + 1e-12
    f = np.linspace(0, 0.5, len(X))
    top = X[f > 0.35].sum() / X.sum()
    band = X[f > 0.25]
    flat = np.exp(np.mean(np.log(band))) / np.mean(band)
    loopdb = 20 * np.log10(np.sqrt(np.mean(loop ** 2)) / 0.29 + 1e-9)
    return dict(hz=freq / 1024, size=size, looped=looped, jag=jag, top=top, flat=flat, loopdb=loopdb,
                maxstep=int(np.abs(np.diff(d)).max()) if size > 1 else 0)

def main():
    set_dir, stats_path = sys.argv[1:3]
    driver = float(sys.argv[3]) if len(sys.argv) > 3 else 15768
    lib = [f for f in glob.glob(os.path.join(set_dir, '*')) if f.lower().endswith('.gsflib')][0]
    rom = bytes(gsfpy.load(lib)[0]['data'])
    rows = []
    for r in csv.DictReader(open(stats_path)):
        m = analyze(rom, int(r['wav'], 16))
        if not m:
            continue
        m.update(wav=r['wav'], seconds=float(r['seconds']), pitch=float(r['meanrate']) / driver,
                 maxpitch=float(r['maxrate']) / driver, tracks=r['tracks'])
        # Grit needs rough data (jag, top, flat), gets worse the nearer the
        # loop sits to the 8-bit floor, and is exposed by pitching up
        rough = m['jag'] * (0.5 + m['top'] * 4) * (0.3 + m['flat'])
        quiet = 1 + max(0, 40 - m['loopdb']) / 20
        m['score'] = rough * quiet * max(1, m['pitch']) ** 0.5
        rows.append(m)
    rows.sort(key=lambda m: -m['score'])
    print(f'{"rank":>4} {"wav":8} {"score":>6} {"jag":>5} {"top%":>5} {"flat":>5} {"loopdB":>6} {"pitch":>5} {"max":>5} {"secs":>7}  tracks')
    for i, m in enumerate(rows):
        tr = m['tracks'].split()
        print(f'{i + 1:4d} {m["wav"]:8} {m["score"]:6.2f} {m["jag"]:5.2f} {100 * m["top"]:5.1f} {m["flat"]:5.2f} {m["loopdb"]:6.1f} '
              f'{m["pitch"]:5.2f} {m["maxpitch"]:5.2f} {m["seconds"]:7.1f}  {" ".join(tr[:6])}{" +%d" % (len(tr) - 6) if len(tr) > 6 else ""}')

if __name__ == '__main__':
    main()
