#!/usr/bin/env python3
# Copyright (c) 2026 gsf2wav contributors
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Regression check against real rips: renders a fixed matrix of tracks and
# options (every mix mode, --sample-rom, --verify, muting, bit depths, the PSG-
# only and fallback paths, both drivers) and writes a manifest of md5s, so two
# builds can be compared byte for byte. The synthetic suite (test/run.py)
# can't catch a change in what a real game sounds like; this can.
#
#   regress.py run GSF2WAV OUTDIR [--rips DIR] [--wine] [--jobs N]
#   regress.py diff OLD_OUTDIR NEW_OUTDIR
#
# The rips and ROMs aren't in the repo (they're the games' data). --rips
# defaults to rips/ at the repo root, laid out as fetch_set.py leaves it:
#   rips/mother3/  Mother 3 (Japan) set
#   rips/wl4/      Wario Land 4 set
#   rips/mlss/     Mario & Luigi: Superstar Saga set
#   rips/roms/     the user's own dumps, named as below (only the
#                  --sample-rom cases need them)
# Cases whose files are missing are skipped and listed.
#
# Typical use: build the old commit, "run" it into one folder, build the new
# one, "run" it into another, then "diff". A refactor should change nothing;
# an intended change should only touch the cases it's meant to. --wine runs a
# Windows build under Wine (WINEPREFIX from the environment).
import argparse, concurrent.futures, glob, hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
ROMS = {
    'm3': 'Mother 3 (Japan).gba',
    'ml': 'Mario & Luigi - Superstar Saga (USA).gba',
    'wl': 'Wario Land 4 (USA, Europe).gba',
}
# Short, deterministic renders: the first 20 s plus a 2 s fade, float output
BASE = ['-v', '-r', '48000', '-b', '32f', '-l', '20', '-f', '2']

# (name, options, set, track prefix). 'ROM:m3' in the options becomes the path
# of that ROM. Track prefixes match the start of the file name.
CASES = [
    # Mother 3 (MP2K at 15768 Hz). 006 has rip holes and the organ sample;
    # 203 has the hole only the ROM can fix; 240 is PSG only.
    ('m6_sinc', [], 'mother3', '006'),
    ('m6_blam', ['--mix', 'blam'], 'mother3', '006'),
    ('m6_linear', ['--mix', 'linear'], 'mother3', '006'),
    ('m6_lerp', ['--mix', 'lerp'], 'mother3', '006'),
    ('m6_lin_samples', ['--linear-samples', '082ED1BC'], 'mother3', '006'),
    ('m6_romsamples', ['--mix', 'blam', '--sample-rom', 'ROM:m3'], 'mother3', '006'),
    ('m6_nofill', ['--no-fill-holes'], 'mother3', '006'),
    ('m6_nohifi', ['--no-hifi'], 'mother3', '006'),
    ('m6_hold', ['--no-hifi', '--fifo-hold'], 'mother3', '006'),
    ('m6_bw_driver', ['--bandwidth', 'driver'], 'mother3', '006'),
    ('m6_bw_8k', ['--bandwidth', '8000'], 'mother3', '006'),
    ('m6_cutoff', ['--source-cutoff', '0.4'], 'mother3', '006'),
    ('m6_ramp0', ['--ramp', '0'], 'mother3', '006'),
    ('m6_mute', ['--mute', 'psg,1,4'], 'mother3', '006'),
    ('m6_solo', ['--solo-sample', '082ED1BC'], 'mother3', '006'),
    ('m6_stats', ['--sample-stats'], 'mother3', '006'),
    ('m6_verify', ['--verify'], 'mother3', '006'),
    ('m6_gain', ['-g', '-6', '--no-volume-tag'], 'mother3', '006'),
    ('m4_blam_s24', ['-b', '24', '--mix', 'blam'], 'mother3', '004'),
    ('m4_blam_s16', ['-b', '16', '-r', '44100', '--mix', 'blam'], 'mother3', '004'),
    ('m240_psgonly', [], 'mother3', '240'),
    ('m203_rom', ['--mix', 'blam', '--sample-rom', 'ROM:m3'], 'mother3', '203'),
    # Wario Land 4 (MP2K at 13379 Hz)
    ('w2_blam', ['--mix', 'blam'], 'wl4', '002'),
    ('w58_sinc', [], 'wl4', '058'),
    ('w2_verify', ['--verify'], 'wl4', '002'),
    ('w6_romsamples', ['--sample-rom', 'ROM:wl'], 'wl4', '006a'),
    # Mario & Luigi (AlphaDream). 05 has the holes that distorted its high notes.
    ('ml5_sinc', [], 'mlss', '05 '),
    ('ml5_blam', ['--mix', 'blam'], 'mlss', '05 '),
    ('ml5_linear', ['--mix', 'linear'], 'mlss', '05 '),
    ('ml5_lerp', ['--mix', 'lerp'], 'mlss', '05 '),
    ('ml5_rom', ['--mix', 'blam', '--sample-rom', 'ROM:ml'], 'mlss', '05 '),
    ('ml5_nohifi', ['--no-hifi'], 'mlss', '05 '),
    ('ml5_verify', ['--verify'], 'mlss', '05 '),
    ('ml5_stats', ['--sample-stats'], 'mlss', '05 '),
    ('ml5_mute', ['--mute', '0,1'], 'mlss', '05 '),
    ('ml96_psgonly', [], 'mlss', '96 '),
    ('ml14_blam', ['--mix', 'blam'], 'mlss', '14 '),
]

# Lines from -v output worth keeping in the manifest: deterministic, and they
# say whether the exact mix still verifies and the timing lock still holds.
KEEP = ('verify:', 'high-precision:')


def _resolve(rips, args, set_name, prefix):
    found = sorted(glob.glob(os.path.join(glob.escape(os.path.join(rips, set_name)),
                                          glob.escape(prefix) + '*.minigsf')))
    if not found:
        return None, f'{set_name}/{prefix}*.minigsf'
    out = []
    for a in args:
        if a.startswith('ROM:'):
            rom = os.path.join(rips, 'roms', ROMS[a[4:]])
            if not os.path.exists(rom):
                return None, rom
            a = rom
        out.append(a)
    return out + [found[0]], None


def _run_one(exe, wine, out, name, args):
    wav = os.path.join(out, name + '.wav')
    if os.path.exists(wav):
        os.remove(wav)
    cmd = [exe] + BASE + args + [wav]
    env = dict(os.environ)
    if wine:
        cmd = [os.environ.get('WINE', '/usr/lib/wine/wine64')] + cmd
        env.setdefault('WINEDEBUG', '-all')
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    digest = None
    if os.path.exists(wav):
        with open(wav, 'rb') as f:
            digest = hashlib.md5(f.read()).hexdigest()
    # With -v the detail lines go to stderr; --sample-stats writes to stdout
    lines = [l.strip() for l in r.stderr.splitlines() if any(k in l for k in KEEP)]
    stdout = hashlib.md5(r.stdout.encode()).hexdigest() if r.stdout.strip() else None
    return name, dict(rc=r.returncode, wav=digest, stdout=stdout, lines=lines)


def cmd_run(a):
    exe = os.path.abspath(a.exe)
    out = os.path.abspath(a.outdir)
    os.makedirs(out, exist_ok=True)
    jobs, skipped = [], []
    for name, args, set_name, prefix in CASES:
        full, missing = _resolve(a.rips, args, set_name, prefix)
        if full is None:
            skipped.append((name, missing))
        else:
            jobs.append((name, full))
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        res = dict(ex.map(lambda j: _run_one(exe, a.wine, out, *j), jobs))
    with open(os.path.join(out, 'manifest.json'), 'w') as f:
        json.dump(res, f, indent=1, sort_keys=True)
    bad = sorted(n for n, v in res.items() if v['rc'] or not v['wav'])
    print(f'{len(res)} cases rendered, {len(bad)} failed, {len(skipped)} skipped')
    for n in bad:
        print('  failed:', n, 'exit', res[n]['rc'])
    for n, missing in skipped:
        print('  skipped:', n, '(missing', missing + ')')
    return 1 if bad else 0


def cmd_diff(a):
    def load(d):
        with open(os.path.join(d, 'manifest.json')) as f:
            return json.load(f)
    old, new = load(a.old), load(a.new)
    same, changed, notes = 0, [], []
    for name in sorted(set(old) | set(new)):
        o, n = old.get(name), new.get(name)
        if o is None or n is None:
            changed.append(f'{name}: only in {"new" if o is None else "old"}')
            continue
        diffs = [k for k in ('rc', 'wav', 'stdout') if o.get(k) != n.get(k)]
        if diffs:
            changed.append(f'{name}: {", ".join(diffs)} differ')
        else:
            same += 1
        # Wording changes in the verbose lines are worth a look but aren't a
        # regression by themselves
        if o.get('lines') != n.get('lines'):
            notes.append(f'{name}:\n    old: ' + '\n         '.join(o.get('lines') or ['-'])
                         + '\n    new: ' + '\n         '.join(n.get('lines') or ['-']))
    print(f'{same} identical, {len(changed)} differ')
    for c in changed:
        print(' ', c)
    if notes:
        print(f'\nverbose lines changed in {len(notes)} cases (audio unaffected unless listed above):')
        for c in notes:
            print(' ', c)
    return 1 if changed else 0


def main():
    p = argparse.ArgumentParser(description='gsf2wav regression check against real rips')
    sub = p.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('exe')
    r.add_argument('outdir')
    r.add_argument('--rips', default=os.path.join(REPO, 'rips'))
    r.add_argument('--wine', action='store_true')
    r.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    d = sub.add_parser('diff')
    d.add_argument('old')
    d.add_argument('new')
    a = p.parse_args()
    sys.exit(cmd_run(a) if a.cmd == 'run' else cmd_diff(a))


if __name__ == '__main__':
    main()
