#!/usr/bin/env python3
# Finds the MP2K (m4a) driver's SoundMain in a GSF's ROM image by its literal
# pool (SOUND_INFO_PTR 0x03007FF0 and ID_NUMBER 'Smsh'), then the
# "ldr r6; ldr r3; bx r3" tail that jumps into SoundMainRAM.
import struct, sys
import gsfpy

ID_NUMBER = 0x68736D53
SOUND_INFO_PTR = 0x03007FF0

def scan(rom):
    hits = []
    for off in range(0, len(rom) - 64, 2):
        h = struct.unpack_from('<HHHHH', rom, off)
        # ldr r0,[pc,#a]; ldr r0,[r0]; ldr r2,[pc,#b]; ldr r3,[r0]; cmp r2,r3
        if (h[0] >> 8) != 0x48 or h[1] != 0x6800 or (h[2] >> 8) != 0x4A or h[3] != 0x6803 or h[4] != 0x429A:
            continue
        lit0 = ((off + 4) & ~3) + (h[0] & 0xFF) * 4
        lit2 = ((off + 8) & ~3) + (h[2] & 0xFF) * 4
        if struct.unpack_from('<I', rom, lit0)[0] != SOUND_INFO_PTR:
            continue
        if struct.unpack_from('<I', rom, lit2)[0] != ID_NUMBER:
            continue
        tail = None
        for t in range(off, off + 0x100, 2):
            a, b, c = struct.unpack_from('<HHH', rom, t)
            if (a >> 8) == 0x4E and (b >> 8) == 0x4B and c == 0x4718:
                tail = t + 4
                lit = ((t + 2 + 4) & ~3) + (b & 0xFF) * 4
                ram = struct.unpack_from('<I', rom, lit)[0]
                break
        hits.append((0x08000000 + off, tail and 0x08000000 + tail, tail and ram))
    return hits

if __name__ == '__main__':
    image, tags = gsfpy.load(sys.argv[1])
    for sm, bx, ram in scan(bytes(image['data'])):
        print(f'SoundMain at {sm:08X}; bx r3 at {bx:08X} -> SoundMainRAM at {ram:08X}' if bx else f'SoundMain at {sm:08X}; tail not found')
