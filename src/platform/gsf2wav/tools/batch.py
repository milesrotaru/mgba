#!/usr/bin/env python3
# Runs gsf2wav over many files in parallel and prints selected stderr lines.
#   batch.py GSF2WAV DIR [--every N] [--grep TEXT] [-- extra gsf2wav args]
import argparse, concurrent.futures, glob, os, subprocess, sys

def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        i = argv.index('--')
        argv, extra = argv[:i], argv[i + 1:]
    p = argparse.ArgumentParser()
    p.add_argument('exe')
    p.add_argument('dir')
    p.add_argument('--every', type=int, default=1)
    p.add_argument('--grep', default='')
    p.add_argument('--out', default=None, help='directory for WAVs (default: discard)')
    a = p.parse_args(argv)
    files = sorted(glob.glob(os.path.join(a.dir, '*.minigsf')))[::a.every]
    def run(f):
        out = os.path.join(a.out, os.path.basename(f)[:-8] + '.wav') if a.out else '/dev/null'
        r = subprocess.run([a.exe] + extra + [f, out], capture_output=True, text=True)
        lines = [l for l in r.stderr.splitlines() if a.grep in l]
        return os.path.basename(f), r.returncode, lines
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        for name, rc, lines in ex.map(run, files):
            print(f'{name}: ' + ' | '.join(lines) + (f' [exit {rc}]' if rc else ''))

if __name__ == '__main__':
    main()
