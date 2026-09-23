#!/usr/bin/env python3
# Disassembles a GSF's MP2K SoundMain and SoundMainRAM (the ROM copy that the
# driver later copies to IWRAM), for comparing a game's driver revision with
# the port in mp2k.c. Output goes to OUTDIR (keep it out of the repo: it's
# the game's code). Needs clang with the ARM target, ld.lld, llvm-objdump.
#   disasm_mp2k.py GAME.minigsf OUTDIR
# Writes soundmain_thumb.txt plus soundmainram_{thumb,arm}.txt: SoundMainRAM
# switches between Thumb and ARM, so read each part from the matching file
# (the "bx" targets show where it switches).
import os, subprocess, sys
import gsfpy
from mp2k_scan import scan

def disasm(data, base, mode, out, workdir):
    raw = os.path.join(workdir, 'blob.bin')
    open(raw, 'wb').write(data)
    src = os.path.join(workdir, 'blob.s')
    open(src, 'w').write(f'.text\n.{mode}\n.incbin "{raw}"\n')
    obj = os.path.join(workdir, 'blob.o')
    elf = os.path.join(workdir, 'blob.elf')
    subprocess.check_call(['clang', '--target=armv4t-none-eabi', '-c', src, '-o', obj])
    subprocess.check_call(['ld.lld', f'-Ttext=0x{base:08X}', obj, '-o', elf], stderr=subprocess.DEVNULL)
    triple = 'thumbv4t' if mode == 'thumb' else 'armv4t'
    text = subprocess.check_output(['llvm-objdump', '-d', f'--triple={triple}', '--disassemble-all', elf], text=True)
    open(out, 'w').write(text)

def main():
    gsf, outdir = sys.argv[1:3]
    os.makedirs(outdir, exist_ok=True)
    image, _ = gsfpy.load(gsf)
    rom = bytes(image['data'])
    hits = [h for h in scan(rom) if h[1]]
    if not hits:
        sys.exit('No MP2K SoundMain found')
    soundmain, bx, ram = hits[0]
    off = soundmain - 0x08000000
    # SoundMain ends with its literal pool; SoundMainRAM's ROM copy follows it
    tail = bx - 0x08000000 + 2
    pool_end = (tail + 3) & ~3
    pool_end += 4 * 6
    disasm(rom[off:pool_end], soundmain, 'thumb', os.path.join(outdir, 'soundmain_thumb.txt'), outdir)
    ram_rom = pool_end
    for mode in ('thumb', 'arm'):
        disasm(rom[ram_rom:ram_rom + 0x400], 0x08000000 + ram_rom, mode, os.path.join(outdir, f'soundmainram_{mode}.txt'), outdir)
    print(f'SoundMain at {soundmain:08X}, SoundMainRAM copy from ~{0x08000000 + ram_rom:08X} (runs at {ram & ~1:08X}); listings in {outdir}')

if __name__ == '__main__':
    main()
