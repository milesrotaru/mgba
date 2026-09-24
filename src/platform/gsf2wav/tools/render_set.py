#!/usr/bin/env python3
# Renders every (mini)GSF in a set with gsf2wav, encodes each to Opus with the
# rip's tags, and zips the result. Needs opusenc (opus-tools).
#   render_set.py GSF2WAV SET_DIR OUT_DIR [--bitrate KBPS] [--zip NAME.zip]
#                 [--overrides FILE] [-- gsf2wav args]
# An overrides file lists per-track gsf2wav options: each line is a track
# filename prefix (up to its first space), then options; # starts a comment.
import argparse, concurrent.futures, glob, os, re, shlex, struct, subprocess, sys, zipfile
import gsfpy

def tags_of(path):
    d = open(path, 'rb').read()
    rsv, csz, _ = struct.unpack('<III', d[4:16])
    return gsfpy._tags(d[16 + rsv + csz:])

def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        i = argv.index('--')
        argv, extra = argv[:i], argv[i + 1:]
    p = argparse.ArgumentParser()
    p.add_argument('exe')
    p.add_argument('set_dir')
    p.add_argument('out_dir')
    p.add_argument('--bitrate', type=int, default=96)
    p.add_argument('--zip')
    p.add_argument('--overrides')
    a = p.parse_args(argv)
    overrides = {}
    if a.overrides:
        for line in open(a.overrides):
            line = line.split('#', 1)[0].strip()
            if line:
                key, _, opts = line.partition(' ')
                overrides[key] = shlex.split(opts)
    os.makedirs(a.out_dir, exist_ok=True)
    files = sorted(glob.glob(os.path.join(a.set_dir, '*.minigsf')) + glob.glob(os.path.join(a.set_dir, '*.gsf')))

    def run(f):
        base = os.path.splitext(os.path.basename(f))[0]
        wav = os.path.join(a.out_dir, base + '.wav')
        out = os.path.join(a.out_dir, base + '.opus')
        track_opts = overrides.get(base.split(' ', 1)[0], [])
        r = subprocess.run([a.exe] + extra + track_opts + [f, wav], capture_output=True, text=True)
        if r.returncode:
            return base, False, r.stderr.strip().splitlines()[-1:]
        t = tags_of(f)
        cmd = ['opusenc', '--quiet', '--bitrate', str(a.bitrate)]
        m = re.match(r'(\d+)', base)
        for key, val in (('title', t.get('title')), ('artist', t.get('artist')), ('album', t.get('game')),
                         ('date', t.get('year')), ('tracknumber', m.group(1).lstrip('0') if m else None),
                         ('copyright', t.get('copyright')),
                         ('comment', 'Rendered with gsf2wav (mGBA), MP2K high-precision mixing' +
                          (f'; options: {" ".join(extra)}' if extra else '') +
                          (f'; track options: {" ".join(track_opts)}' if track_opts else ''))):
            if val:
                cmd += ['--comment', f'{key.upper()}={val}']
        r2 = subprocess.run(cmd + [wav, out], capture_output=True, text=True)
        os.remove(wav)
        peak = [l for l in r.stderr.splitlines() if l.startswith('Peak') or 'high-precision' in l]
        if track_opts:
            peak.append('options: ' + ' '.join(track_opts))
        return base, r2.returncode == 0, peak

    ok = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        for n, (base, good, info) in enumerate(ex.map(run, files)):
            ok += good
            print(f'[{n + 1}/{len(files)}] {"ok  " if good else "FAIL"} {base}: {" | ".join(info)}', flush=True)
    print(f'{ok}/{len(files)} tracks encoded')
    if a.zip:
        with zipfile.ZipFile(a.zip, 'w', zipfile.ZIP_STORED) as z:
            for f in sorted(glob.glob(os.path.join(a.out_dir, '*.opus'))):
                z.write(f, os.path.basename(f))
        print(f'{a.zip}: {os.path.getsize(a.zip) / 1e6:.1f} MB')

if __name__ == '__main__':
    main()
