#!/usr/bin/env python3
# Copyright (c) 2026 gsf2wav contributors
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Zips a built gsf2wav.exe with what a Windows user needs next to it: a plain
# README, the license, where the source is, the technical README, and the
# per-track level files. Called by build-windows.sh.
#
#   package_windows.py GSF2WAV.EXE OUT_DIR
#
# Writes OUT_DIR/gsf2wav-VERSION-windows-x64.zip and prints its SHA-256 and the
# exe's. The exe is stripped first if x86_64-w64-mingw32-strip is available.
import hashlib, os, shutil, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
GSF2WAV = os.path.join(ROOT, 'src', 'platform', 'gsf2wav')

def git(*args):
    try:
        return subprocess.check_output(['git', '-C', ROOT] + list(args), text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ''

def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n')

def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()

def main():
    exe, out = sys.argv[1], sys.argv[2]
    version = git('describe', '--always', '--dirty') or 'unknown'
    commit = git('rev-parse', 'HEAD') or 'unknown'
    name = f'gsf2wav-{version}-windows-x64'
    stage = tempfile.mkdtemp()
    top = os.path.join(stage, name)
    os.makedirs(os.path.join(top, 'overrides'))

    packed = os.path.join(top, 'gsf2wav.exe')
    shutil.copy(exe, packed)
    strip = shutil.which('x86_64-w64-mingw32-strip')
    if strip:
        subprocess.check_call([strip, '-s', packed])
    exe_hash = sha256(packed)

    readme = open(os.path.join(GSF2WAV, 'packaging', 'windows', 'README.txt'), encoding='utf-8').read()
    readme = readme.replace('@VERSION@', version).replace('@COMMIT@', commit)
    open(os.path.join(top, 'README.txt'), 'w', encoding='utf-8', newline='').write(crlf(readme))
    shutil.copy(os.path.join(GSF2WAV, 'README.md'), os.path.join(top, 'README-technical.md'))
    shutil.copy(os.path.join(ROOT, 'LICENSE'), os.path.join(top, 'LICENSE.txt'))
    data = os.path.join(GSF2WAV, 'tools', 'data')
    for f in sorted(os.listdir(data)):
        if f.endswith('_overrides.txt'):
            text = open(os.path.join(data, f), encoding='utf-8').read()
            open(os.path.join(top, 'overrides', f), 'w', encoding='utf-8', newline='').write(crlf(text))

    os.makedirs(out, exist_ok=True)
    zpath = os.path.join(out, name + '.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, _, files in os.walk(top):
            for f in sorted(files):
                full = os.path.join(dirpath, f)
                z.write(full, os.path.relpath(full, stage).replace(os.sep, '/'))
    shutil.rmtree(stage)
    print(f'{zpath}  {os.path.getsize(zpath) / 1e6:.1f} MB')
    print(f'zip   sha256 {sha256(zpath)}')
    print(f'exe   sha256 {exe_hash}')

if __name__ == '__main__':
    main()
