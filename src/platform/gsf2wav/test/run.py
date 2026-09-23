#!/usr/bin/env python3
# Builds synthetic GSFs, renders them with gsf2wav and checks the spectra.
# Needs clang (with the ARM target), ld.lld, llvm-objcopy and numpy.
#
#   run.py path/to/gsf2wav [workdir]
import math, os, struct, subprocess, sys, zlib
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

    for args, src, out in (([], 'psg.gsf', 'psg.wav'), ([], 'fifo.gsf', 'fifo.wav'),
                           (['--fifo-hold'], 'fifo.gsf', 'hold.wav'), ([], 'song.minigsf', 'mini.wav')):
        subprocess.check_call([exe] + args + [src, out], stderr=subprocess.DEVNULL)

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
    sys.exit(1 if failures else 0)

if __name__ == '__main__':
    main()
