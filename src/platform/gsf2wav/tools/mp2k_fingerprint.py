#!/usr/bin/env python3
# Copyright (c) 2026 gsf2wav contributors
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Fingerprints a game's MP2K mixer (SoundMainRAM), to tell whether it's the
# revision mp2k.c reimplements. Works on a rip or a full ROM, without running
# anything.
#
#   mp2k_fingerprint.py [--ref INPUT] [--csv] INPUT...
#
# INPUT is a .gba ROM, a .gsf/.minigsf (its _lib chain is loaded), a .gsflib,
# or a folder (its first .minigsf or .gsf). --ref also counts the 32-bit words
# that differ from a reference game's mixer, which says how far a new
# revision is from the known one.
#
# How it works: mp2k_scan.py finds SoundMain. m4aSoundInit copies the mixer
# to IWRAM with CpuSet, and its literal pool holds the three CpuSet arguments
# in a row: the mixer's ROM address (Thumb bit set), the IWRAM buffer (the
# same one SoundMain jumps into) and the control word, which gives the size.
# Words that look like addresses are masked before hashing, since they move
# from game to game; everything else is code and constants.
#
# Two hashes come out. "window" covers the whole copy. "core" covers its first
# CORE_SIZE bytes, which in the revision Mother 3 and Wario Land 4 use is the
# mixer plus the routine that unlinks a finished voice; the rest of the copy is
# a sequencer helper that differs between those two games without affecting
# the mix. A known core hash means the same mixer code --verify has already
# checked; anything else means "unknown, run --verify". A rip's mixer copy is
# intact (init reads all of it), so a rip and its ROM give the same hashes.
import argparse, glob, hashlib, os, struct, sys
import gsfpy
from mp2k_scan import scan

CORE_SIZE = 0x3DC
# Core hashes whose mixer --verify has checked against the game, bit for bit
KNOWN = {
    '5bfa0c4bba80': 'SDK 3.0 revision: mp2k.c verified bit-exact (Mother 3, Wario Land 4)',
}


def load(path):
    if os.path.isdir(path):
        found = sorted(glob.glob(os.path.join(glob.escape(path), '*.minigsf'))) \
            or sorted(glob.glob(os.path.join(glob.escape(path), '*.gsf')))
        if not found:
            raise ValueError('no .minigsf or .gsf in folder')
        path = found[0]
    if path.lower().endswith(('.gba', '.agb', '.bin')):
        with open(path, 'rb') as f:
            return f.read()
    image, _ = gsfpy.load(path)
    return bytes(image['data'])


def locate(rom):
    """Returns (SoundMain, mixer ROM address, copy size), or None."""
    for soundmain, bx, ram in scan(rom):
        if not bx:
            continue
        buf = ram & ~1
        near = soundmain - 0x08000000
        # m4aSoundInit's pool sits a few KB after SoundMain; search there first
        for lo, hi in ((near, near + 0x4000), (0, len(rom))):
            for i in range(lo & ~3, min(hi, len(rom) - 11), 4):
                addr, dest, ctrl = struct.unpack_from('<III', rom, i)
                if dest == buf and addr & 1 and bx < addr < bx + 0x80 and ctrl >> 24 == 0x04:
                    return soundmain, addr & ~1, 4 * (ctrl & 0x1FFFFF)
    return None


def masked(rom, start, size):
    data = bytearray(rom[start - 0x08000000:start - 0x08000000 + size])
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from('<I', data, i)[0]
        if 0x02000000 <= w < 0x02040000 or 0x03000000 <= w < 0x03008000 or 0x08000000 <= w < 0x0A000000:
            data[i:i + 4] = b'\xAA' * 4
    return bytes(data)


def fingerprint(path):
    rom = load(path)
    loc = locate(rom)
    if not loc:
        found = [h for h in scan(rom)]
        return dict(status='no SoundMain' if not found else 'mixer copy not found')
    soundmain, start, size = loc
    data = masked(rom, start, size)
    core = hashlib.sha1(data[:CORE_SIZE]).hexdigest()[:12]
    return dict(status='ok', soundmain=soundmain, start=start, size=size, data=data, core=core,
                window=hashlib.sha1(data).hexdigest()[:12], known=KNOWN.get(core, ''),
                zeroed=data[:CORE_SIZE].count(0) > CORE_SIZE // 2)


def main():
    p = argparse.ArgumentParser(description='Fingerprint the MP2K mixer in GSF rips or ROMs')
    p.add_argument('inputs', nargs='+')
    p.add_argument('--ref', help='count differing words against this game\'s mixer')
    p.add_argument('--csv', action='store_true')
    a = p.parse_args()
    ref = fingerprint(a.ref) if a.ref else None
    if ref and ref['status'] != 'ok':
        sys.exit(f'--ref: {ref["status"]}')
    if a.csv:
        print('input,status,soundmain,mixer,size,core,window,diff_core,diff_window,known')
    for path in a.inputs:
        try:
            fp = fingerprint(path)
        except Exception as e:
            fp = dict(status=f'error: {e}')
        name = os.path.basename(path.rstrip('/\\'))
        if fp['status'] != 'ok':
            print(f'"{name}",{fp["status"]},,,,,,,,' if a.csv else f'{name}: {fp["status"]}')
            continue
        dc = dw = ''
        if ref:
            words = lambda x, y, n: sum(x[i:i + 4] != y[i:i + 4] for i in range(0, n, 4))
            dc = words(fp['data'], ref['data'], CORE_SIZE)
            dw = words(fp['data'], ref['data'], min(fp['size'], ref['size'])) + abs(fp['size'] - ref['size']) // 4
        if fp['zeroed']:
            fp['known'] = 'mixer copy mostly zero (rip never ran the init?)'
        if a.csv:
            print(f'"{name}",ok,{fp["soundmain"]:08X},{fp["start"]:08X},{fp["size"]:#x},{fp["core"]},'
                  f'{fp["window"]},{dc},{dw},"{fp["known"]}"')
        else:
            diff = f'  differs from ref in {dc} core / {dw} window words' if ref else ''
            print(f'{name}: SoundMain {fp["soundmain"]:08X}, mixer {fp["start"]:08X} ({fp["size"]:#x} bytes), '
                  f'core {fp["core"]}, window {fp["window"]}{diff}')
            print(f'    {fp["known"] or "unknown revision: run gsf2wav --verify on a track"}')


if __name__ == '__main__':
    main()
