#!/usr/bin/env python3
# Builds synthetic GSFs, renders them with gsf2wav and checks the spectra, then
# exercises the command line (batches, naming, overrides, errors) on them.
# Needs clang (with the ARM target), ld.lld, llvm-objcopy and numpy.
#
#   run.py path/to/gsf2wav [workdir]
#
# To test the Windows build under Wine: GSF2WAV_RUNNER=wine64 run.py gsf2wav.exe
import math, os, shutil, struct, subprocess, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CLOCK = 16777216
FIFO_PERIOD = 1254  # 13379 Hz, a common MP2K mixing rate

def rom(mode, table=b''):
    subprocess.check_call(['clang', '--target=armv4t-none-eabi', '-marm', '-O2', '-ffreestanding', '-nostdlib',
                           f'-DMODE={mode}', f'-DPERIOD={FIFO_PERIOD}', '-c', os.path.join(HERE, 'rom.c'), '-o', 'rom.o'])
    subprocess.check_call(['ld.lld', '-T', os.path.join(HERE, 'link.ld'), '-e', '_start', 'rom.o', '-o', 'rom.elf'])
    subprocess.check_call(['llvm-objcopy', '-O', 'binary', '-j', '.text', 'rom.elf', 'rom.bin'])
    code = open('rom.bin', 'rb').read()
    assert len(code) < 0x1000
    return code + b'\0' * (0x1000 - len(code)) + table

def gsf(path, payload, tags, offset=0x08000000):
    prog = struct.pack('<III', 0x08000000, offset, len(payload)) + payload
    comp = zlib.compress(prog, 9)
    data = b'PSF\x22' + struct.pack('<III', 0, len(comp), zlib.crc32(comp)) + comp
    data += b'[TAG]' + ''.join(f'{k}={v}\n' for k, v in tags.items()).encode()
    open(path, 'wb').write(data)

def load(path):
    d = open(path, 'rb').read()
    i = d.index(b'data')
    n = struct.unpack('<I', d[i + 4:i + 8])[0]
    return np.frombuffer(d[i + 8:i + 8 + n], dtype='<f4').reshape(-1, 2).astype(np.float64)

def spectrum(x, fs):
    x = x[fs:fs * 3, 0]
    X = np.abs(np.fft.rfft(x * np.kaiser(len(x), 20)))
    X /= X.max()
    return np.fft.rfftfreq(len(x), 1 / fs), 20 * np.log10(X + 1e-20)

def main():
    exe = os.path.abspath(sys.argv[1])
    work = sys.argv[2] if len(sys.argv) > 2 else 'gsf2wav-test'
    os.makedirs(work, exist_ok=True)
    os.chdir(work)
    fs = 48000
    rate = CLOCK / FIFO_PERIOD
    failures = 0

    def check(name, value, limit):
        nonlocal failures
        ok = value <= limit
        failures += not ok
        print(f'{"ok  " if ok else "FAIL"} {name}: {value:.1f} dB (limit {limit} dB)')

    gsf('psg.gsf', rom(0), {'length': '0:04', 'fade': '0'})
    table = bytes(int(round(100 * math.sin(2 * math.pi * 1000 * i / rate))) & 0xFF for i in range(int(rate * 6)))
    gsf('fifo.gsf', rom(1, table), {'length': '0:04', 'fade': '0'})
    # _lib chain, with a tag whose case doesn't match the file name
    image = rom(0)
    gsf('Test.GSFLIB', image, {})
    gsf('song.minigsf', image[:16], {'_lib': 'test.gsflib', 'length': '0:04', 'fade': '0'})

    runner = os.environ.get('GSF2WAV_RUNNER', '').split()
    for args, src, out in (([], 'psg.gsf', 'psg.wav'), ([], 'fifo.gsf', 'fifo.wav'),
                           (['--fifo-hold'], 'fifo.gsf', 'hold.wav'), ([], 'song.minigsf', 'mini.wav')):
        subprocess.check_call(runner + [exe] + args + [src, out], stderr=subprocess.DEVNULL)

    f, S = spectrum(load('psg.wav'), fs)
    harmonic = np.zeros_like(f, bool)
    for k in range(1, 100, 2):
        harmonic |= np.abs(f - 512 * k) < 6
    check('PSG square: worst non-harmonic', S[~harmonic & (f > 20)].max(), -120)
    # Passband flatness: a 50% square's odd harmonics fall off exactly as 1/k
    x = load('psg.wav')[fs:fs * 3, 0]
    t = np.arange(len(x)) / fs
    def amp(freq):
        m = np.stack([np.sin(2 * np.pi * freq * t), np.cos(2 * np.pi * freq * t)], 1)
        c, *_ = np.linalg.lstsq(m, x, rcond=None)
        return np.hypot(*c)
    a1 = amp(512)
    worst = max(abs(20 * math.log10(amp(512 * k) / a1 * k)) for k in range(3, 40, 2))
    check('PSG square: harmonic level error up to 20 kHz', worst, 0.01)

    f, S = spectrum(load('fifo.wav'), fs)
    check('FIFO sinc: image at fs-f0', S[np.abs(f - (rate - 1000)) < 4].max(), -120)
    f, S = spectrum(load('hold.wav'), fs)
    image = S[np.abs(f - (rate - 1000)) < 4].max()
    # A zero-order hold leaves the first image at f0 / (fs - f0) relative to the fundamental
    expected = 20 * math.log10(1000 / (rate - 1000))
    check('FIFO hold: image vs. sinc(x) rolloff', abs(image - expected), 0.5)

    diff = np.abs(load('mini.wav') - load('psg.wav')).max()
    print(f'{"ok  " if diff == 0 else "FAIL"} minigsf + _lib renders identically to the plain GSF')
    failures += diff != 0

    def expect(name, condition, detail=''):
        nonlocal failures
        failures += not condition
        print(f'{"ok  " if condition else "FAIL"} {name}{": " + str(detail) if detail and not condition else ""}')
    cli_tests(exe, runner, expect)
    sys.exit(1 if failures else 0)

def cli_tests(exe, runner, expect):
    def g(*args):
        return subprocess.run(runner + [exe] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace')

    def peak(path):
        return np.abs(load(path)).max()

    def same(a, b):
        return open(a, 'rb').read() == open(b, 'rb').read()

    shutil.rmtree('cli', ignore_errors=True)
    os.makedirs('cli/set')
    shutil.copy('psg.gsf', 'cli/set/01 First.gsf')
    shutil.copy('fifo.gsf', 'cli/set/02 Second.gsf')
    shutil.copy('song.minigsf', 'cli/set/03 Third.minigsf')
    shutil.copy('Test.GSFLIB', 'cli/set/Test.GSFLIB')
    quick = ['-l', '1', '-f', '0']

    # A folder: every track, named after its file, next to it; libraries aren't tracks
    r = g(*quick, 'cli/set')
    names = sorted(f for f in os.listdir('cli/set') if f.endswith('.wav'))
    expect('folder: one WAV per track, named after the file', r.returncode == 0 and names == ['01 First.wav', '02 Second.wav', '03 Third.wav'], (r.returncode, names, r.stderr))
    expect('folder: no .part files left behind', not any(f.endswith('.part') for f in os.listdir('cli/set')))
    g(*quick, 'cli/set/02 Second.gsf', 'cli/single.wav')
    expect('folder: a track renders the same as on its own', same('cli/set/02 Second.wav', 'cli/single.wav'))

    # Output locations
    r = g(*quick, '-o', 'cli/out/nested/', 'cli/set')
    expect('-o folder/: created, holds every track', r.returncode == 0 and len(os.listdir('cli/out/nested')) == 3, r.stderr)
    r = g(*quick, '-o', 'cli/named.wav', 'cli/set/01 First.gsf')
    expect('-o FILE.wav with one input', r.returncode == 0 and os.path.exists('cli/named.wav'), r.stderr)
    r = g(*quick, '-o', 'cli/x.wav', 'cli/set/01 First.gsf', 'cli/set/02 Second.gsf')
    expect('-o FILE.wav with two inputs is a folder, not a file', r.returncode == 0 and os.path.isdir('cli/x.wav'), r.stderr)
    os.makedirs('cli/clash_a')
    os.makedirs('cli/clash_b')
    shutil.copy('psg.gsf', 'cli/clash_a/one.gsf')
    shutil.copy('psg.gsf', 'cli/clash_b/one.gsf')
    r = g(*quick, 'cli/clash_a/one.gsf', 'cli/clash_b/one.gsf', '-o', 'cli/clash')
    expect('two inputs with the same name into one folder are refused', r.returncode == 1 and 'both be written' in r.stderr, r.stderr)

    # --skip-existing
    before = os.stat('cli/set/01 First.wav').st_mtime_ns
    r = g(*quick, '--skip-existing', 'cli/set')
    expect('--skip-existing leaves finished WAVs alone', r.returncode == 0 and os.stat('cli/set/01 First.wav').st_mtime_ns == before and 'Skipping 3' in r.stderr, r.stderr)

    # --overrides: a 6 dB cut for one track, matched by its number, only that one
    open('cli/ov.txt', 'w').write('# levels\n02 -g -6\n')
    r = g(*quick, '--overrides', 'cli/ov.txt', '-o', 'cli/ov', 'cli/set')
    ratio = peak('cli/ov/02 Second.wav') / peak('cli/set/02 Second.wav')
    expect('--overrides: the named track is turned down by 6 dB', r.returncode == 0 and abs(20 * math.log10(ratio) + 6) < 0.01, 20 * math.log10(ratio))
    expect('--overrides: other tracks are untouched', same('cli/ov/01 First.wav', 'cli/set/01 First.wav'))
    open('cli/ov2.txt', 'w').write('"02 Second" -g -6\n02 -g -12\n')
    g(*quick, '--overrides', 'cli/ov2.txt', '-o', 'cli/ov2', 'cli/set/02 Second.gsf')
    ratio = peak('cli/ov2/02 Second.wav') / peak('cli/set/02 Second.wav')
    expect('--overrides: a full name beats the first word', abs(20 * math.log10(ratio) + 6) < 0.01, 20 * math.log10(ratio))

    # Threads must not change a byte
    g(*quick, '-j', '1', '-o', 'cli/j1', 'cli/set')
    g(*quick, '-j', '3', '-o', 'cli/j3', 'cli/set')
    expect('-j 3 renders identically to -j 1', all(same(f'cli/j1/{n}', f'cli/j3/{n}') for n in os.listdir('cli/j1')) and len(os.listdir('cli/j3')) == 3)

    # Quiet means quiet; a version and help always work
    r = g('-q', *quick, '-o', 'cli/q', 'cli/set/01 First.gsf')
    expect('-q: nothing printed on success', r.returncode == 0 and not r.stdout.strip() and not r.stderr.strip(), r.stderr)
    r = g('--version')
    expect('--version', r.returncode == 0 and r.stdout.startswith('gsf2wav'), r.stdout)
    r = g('--help')
    expect('--help', r.returncode == 0 and 'usage:' in r.stdout and '--overrides' in r.stdout)

    # Things that should fail, with a useful message and the right exit code
    r = g('--fifo-hld', 'cli/set')
    expect('a mistyped option suggests the right one', r.returncode == 2 and 'did you mean --fifo-hold' in r.stderr, r.stderr)
    r = g('-r', '100', 'cli/set')
    expect('a bad value names the option and the problem', r.returncode == 2 and '-r' in r.stderr and '8000' in r.stderr, r.stderr)
    r = g('cli/nothing.gsf')
    expect('a missing input is reported', r.returncode == 1 and 'no such file' in r.stderr, r.stderr)
    open('cli/bad.txt', 'w').write('01 -g -1\n02 --mix chirp\n')
    r = g('--overrides', 'cli/bad.txt', 'cli/set')
    expect('a bad overrides line is found up front, with its line number', r.returncode == 1 and 'bad.txt:2' in r.stderr, r.stderr)
    open('cli/corrupt.gsf', 'wb').write(b'PSF\x22' + bytes(range(64)))
    r = g('-o', 'cli/corrupt_out', 'cli/corrupt.gsf')
    leftovers = os.listdir('cli/corrupt_out') if os.path.isdir('cli/corrupt_out') else []
    expect('a track that fails leaves no partial file', r.returncode == 1 and 'FAILED' in r.stderr and not leftovers, (r.stderr, leftovers))
    r = g('cli/set/Test.GSFLIB')
    expect('a library file given as input is skipped with an explanation', r.returncode == 1 and 'library' in r.stderr, r.stderr)

    # Names that aren't ASCII (this is where Windows' ANSI C library goes wrong)
    os.makedirs('cli/マザー3', exist_ok=True)
    shutil.copy('Test.GSFLIB', 'cli/マザー3/Test.GSFLIB')
    shutil.copy('song.minigsf', 'cli/マザー3/曲 004 ようこそ.minigsf')
    r = g(*quick, '-o', 'cli/出力', 'cli/マザー3')
    expect('non-ASCII folder and file names, found libs, written to a non-ASCII folder',
           r.returncode == 0 and os.path.exists('cli/出力/曲 004 ようこそ.wav'), (r.returncode, r.stderr))

if __name__ == '__main__':
    main()
