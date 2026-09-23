#!/usr/bin/env python3
# Runs gsf2wav --sample-stats over a whole set (tag lengths) and writes one
# aggregated CSV of which MP2K samples play, for how long, how high, how loud.
#   sample_stats.py GSF2WAV DIR out.csv
import collections, concurrent.futures, csv, glob, os, subprocess, sys

def main():
    exe, dir, out = sys.argv[1:4]
    files = sorted(glob.glob(os.path.join(dir, '*.minigsf')))
    agg = collections.defaultdict(lambda: {'notes': 0, 'seconds': 0.0, 'rate_s': 0.0, 'maxrate': 0.0, 'maxgain': 0.0, 'tracks': set()})
    def run(f):
        r = subprocess.run([exe, '--sample-stats', '-r', '8000', f, '/dev/null'], capture_output=True, text=True)
        return os.path.basename(f), r.stdout
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        for n, (name, stdout) in enumerate(ex.map(run, files)):
            for line in stdout.splitlines():
                p = line.split()
                if not p or p[0] != 'sample':
                    continue
                a = agg[p[1]]
                secs = float(p[5])
                a['notes'] += int(p[3])
                a['seconds'] += secs
                a['rate_s'] += float(p[7]) * secs
                a['maxrate'] = max(a['maxrate'], float(p[9]))
                a['maxgain'] = max(a['maxgain'], float(p[11]))
                a['tracks'].add(name[:3])
            print(f'\r{n + 1}/{len(files)}', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    with open(out, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['wav', 'notes', 'seconds', 'meanrate', 'maxrate', 'maxgain', 'tracks'])
        for wav, a in sorted(agg.items()):
            w.writerow([wav, a['notes'], f"{a['seconds']:.3f}", f"{a['rate_s'] / a['seconds'] if a['seconds'] else 0:.1f}",
                        f"{a['maxrate']:.1f}", f"{a['maxgain']:.4f}", ' '.join(sorted(a['tracks']))])

if __name__ == '__main__':
    main()
