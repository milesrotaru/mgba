#!/usr/bin/env python3
# Copyright (c) 2026 gsf2wav contributors
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Which samples a rip has holes in, measured against the full game ROM.
# GSF rippers zero every ROM byte the game didn't read while being ripped, so
# a sample played well above its recorded rate has zeroed gaps. For each
# sample a set plays (from sample_stats.py's CSV), this compares the rip's
# bytes with the ROM's over the part of the sample the game reaches, and lists
# the samples whose holes matter and the tracks that use them.
#
#   rom_holes.py mp2k|alphadream RIP.minigsf ROM.gba STATS.csv [--tracks-out FILE]
#
# The error columns are relative to the sample's own energy, in dB: 0 dB would
# mean the error is as loud as the sample. "as-is" reads the holes as zeros
# (what sinc and its relatives do with a rip); "filled" interpolates over them
# (gsf2wav's default without --sample-rom).
import csv, struct, sys
import numpy as np
import gsfpy

def fill(a):
    known = np.where(a != 0)[0]
    if not len(known):
        return a * 0
    return np.interp(np.arange(len(a)), known, a[known])

def main():
    args = sys.argv[1:]
    tracks_out = None
    if '--tracks-out' in args:
        i = args.index('--tracks-out')
        tracks_out = args[i + 1]
        del args[i:i + 2]
    kind, rip_path, rom_path, stats = args
    unsigned = kind == 'alphadream'
    img, _ = gsfpy.load(rip_path)
    rip = np.frombuffer(bytes(img['data']), np.uint8)
    romb = open(rom_path, 'rb').read()
    rom = np.frombuffer(romb, np.uint8)
    rows = list(csv.DictReader(open(stats)))
    out = []
    for r in rows:
        w = int(r['wav'], 16) - 0x08000000
        size = struct.unpack_from('<I', romb, w + 12)[0]
        d0 = w + 16
        if size <= 0 or d0 + size > len(rom) or d0 + size > len(rip):
            continue
        P = rip[d0:d0 + size]
        R = rom[d0:d0 + size]
        holes = (P == 0) & (R != 0)
        kept = np.where(P != 0)[0]
        last = kept[-1] if len(kept) else -1
        interior = int(holes[:last + 1].sum()) if last >= 0 else 0
        if not interior:
            out.append(dict(wav=r['wav'], interior=0, tail=int(holes.sum()), tracks=r['tracks']))
            continue
        seg = slice(0, last + 1)
        if unsigned:
            Rf = R[seg].astype(float) - 128
            Pf = np.where(P[seg] == 0, 0, P[seg].astype(float) - 128)
            fillf = fill(P[seg].astype(float)) - 128
            # Reading a zero byte as a sample means -128
            asis = P[seg].astype(float) - 128
        else:
            Rf = R[seg].view(np.int8).astype(float)
            asis = P[seg].view(np.int8).astype(float)
            fillf = fill(asis)
        sig = (Rf ** 2).sum() or 1
        ez = 10 * np.log10(max(((asis - Rf) ** 2).sum(), 1e-9) / sig)
        ef = 10 * np.log10(max(((fillf - Rf) ** 2).sum(), 1e-9) / sig)
        out.append(dict(wav=r['wav'], size=size, interior=interior, pct=100 * interior / (last + 1), ez=ez, ef=ef,
                        maxrate=float(r['maxrate']), tracks=r['tracks']))
    aff = [o for o in out if o['interior']]
    tail = sum(1 for o in out if not o['interior'] and o['tail'])
    print(f'{len(out)} samples played; {len(aff)} have holes inside the part the game plays; '
          f'{tail} only have an unplayed zeroed tail')
    print('total interior hole bytes:', sum(o['interior'] for o in aff))
    print('\nsamples with holes (error over the played part, as-is / filled):')
    for o in sorted(aff, key=lambda o: -o['ez']):
        print('%s  holes %5.1f%% of %6d  as-is %6.1f dB  filled %6.1f dB  maxrate %6.0f  tracks %s' %
              (o['wav'], o['pct'], o['size'], o['ez'], o['ef'], o['maxrate'], o['tracks'][:60]))
    tracks = sorted({t for o in aff for t in o['tracks'].split()})
    print('\ntracks using an affected sample:', ' '.join(tracks))
    if tracks_out:
        open(tracks_out, 'w').write('\n'.join(tracks) + '\n')

if __name__ == '__main__':
    main()
