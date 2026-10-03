/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define R16(a) (*(volatile unsigned short*)(a))
#define R32(a) (*(volatile unsigned int*)(a))
void _start(void) __attribute__((section(".text.start"), naked));
void run(void);
void _start(void) { __asm__("ldr sp, =0x03007F00\n b run"); }
void run(void) {
	R16(0x04000084) = 0x80;
#if MODE == 0
	R16(0x04000080) = 0xFF77;
	R16(0x04000082) = 0x0002;
	R16(0x04000060) = 0;
	R16(0x04000062) = 0xF080;
	R16(0x04000064) = 0x8000 | 1792;
#else
	R16(0x04000082) = (1 << 2) | (1 << 8) | (1 << 9) | (1 << 11);
	R32(0x040000BC) = 0x08001000;
	R32(0x040000C0) = 0x040000A0;
	R16(0x040000C6) = 0xB600;
	R16(0x04000100) = 65536 - PERIOD;
	R16(0x04000102) = 0x0080;
#endif
	for (;;) ;
}
