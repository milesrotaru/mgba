#!/usr/bin/env python3
# Downloads and extracts a GSF set into rips/ at the repo root (gitignored:
# rips are ROM-derived and can't be published with the source).
#
#   fetch_set.py URL [name]
#
# Needs py7zr for .7z sets (pip install py7zr); .zip works out of the box.
import os, sys, urllib.parse, urllib.request, zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))

def main():
    url = sys.argv[1]
    base = urllib.parse.unquote(os.path.basename(urllib.parse.urlparse(url).path))
    name = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(base)[0]
    dest = os.path.join(ROOT, 'rips', name)
    os.makedirs(dest, exist_ok=True)
    archive = os.path.join(ROOT, 'rips', base)
    if not os.path.exists(archive):
        print(f'Downloading {url}')
        urllib.request.urlretrieve(url, archive)
    if base.lower().endswith('.7z'):
        import py7zr
        with py7zr.SevenZipFile(archive) as z:
            z.extractall(dest)
    else:
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    print(dest)

if __name__ == '__main__':
    main()
