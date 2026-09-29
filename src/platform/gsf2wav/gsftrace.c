/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Reverse-engineering aid for sound drivers gsf2wav doesn't know yet. Runs a
// GSF and reports:
//   - the sound DMA channels' configurations (where the FIFOs are fed from)
//   - with --watch LO-HI: every code address that writes into that range,
//     with how often and which part of the range
//   - with --calls ADDR: how often code at ADDR runs, per frame
//   - with --regs ADDR[t] N: the registers at the first N times code at ADDR
//     (in ROM; t for Thumb) is about to run
//   - with --dump DIR: IWRAM and EWRAM at the end (game data: keep it out of
//     the repository)
//
//   gsftrace [--frames N] [--watch LO-HI]... [--regs ADDR[t] N] [--dump DIR] INPUT.minigsf
#include "psf.h"

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WATCH 8
#define MAX_WRITERS 256

struct Writer {
	uint32_t pc;
	bool thumb;
	uint64_t count;
	uint32_t lo;
	uint32_t hi;
	int width;
};

static struct {
	uint32_t lo[MAX_WATCH];
	uint32_t hi[MAX_WATCH];
	int count;
	struct Writer writers[MAX_WRITERS];
	int nWriters;
	struct ARMMemory orig;
	struct ARMCore* cpu;
} _watch;

static void _note(uint32_t address, int width) {
	int i;
	for (i = 0; i < _watch.count; ++i) {
		if (address + width > _watch.lo[i] && address < _watch.hi[i]) {
			break;
		}
	}
	if (i == _watch.count) {
		return;
	}
	struct ARMCore* cpu = _watch.cpu;
	bool thumb = cpu->executionMode == MODE_THUMB;
	// gprs[PC] runs two instructions ahead of the one executing
	uint32_t pc = cpu->gprs[ARM_PC] - (thumb ? 4 : 8);
	int w;
	for (w = 0; w < _watch.nWriters; ++w) {
		if (_watch.writers[w].pc == pc && _watch.writers[w].width == width) {
			break;
		}
	}
	if (w == _watch.nWriters) {
		if (w == MAX_WRITERS) {
			return;
		}
		++_watch.nWriters;
		_watch.writers[w] = (struct Writer) { .pc = pc, .thumb = thumb, .lo = address, .hi = address, .width = width };
	}
	struct Writer* wr = &_watch.writers[w];
	++wr->count;
	if (address < wr->lo) {
		wr->lo = address;
	}
	if (address > wr->hi) {
		wr->hi = address;
	}
}

static void _store32(struct ARMCore* cpu, uint32_t address, int32_t value, int* cycles) {
	_note(address & ~3, 4);
	_watch.orig.store32(cpu, address, value, cycles);
}

static void _store16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycles) {
	_note(address & ~1, 2);
	_watch.orig.store16(cpu, address, value, cycles);
}

static void _store8(struct ARMCore* cpu, uint32_t address, int8_t value, int* cycles) {
	_note(address, 1);
	_watch.orig.store8(cpu, address, value, cycles);
}

static uint32_t _storeMultiple(struct ARMCore* cpu, uint32_t address, int mask, enum LSMDirection direction, int* cycles) {
	int n = __builtin_popcount(mask & 0xFFFF);
	uint32_t base = address;
	if (direction & LSM_D) {
		base -= n * 4;
		if (!(direction & LSM_B)) {
			base += 4;
		}
	} else if (direction & LSM_B) {
		base += 4;
	}
	int i;
	for (i = 0; i < n; ++i) {
		_note((base & ~3) + i * 4, 4);
	}
	return _watch.orig.storeMultiple(cpu, address, mask, direction, cycles);
}

static int _frame;
static int _regsLeft;

static void _regsHit(struct GBACodeHook* hook, struct GBA* gba) {
	(void) hook;
	if (_regsLeft <= 0) {
		return;
	}
	--_regsLeft;
	const int32_t* r = gba->cpu->gprs;
	printf("frame %d:", _frame);
	int i;
	for (i = 0; i < 15; ++i) {
		printf(" r%d=%08X", i, (uint32_t) r[i]);
	}
	printf("\n");
}

static void _nullLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger;
	(void) category;
	(void) level;
	(void) format;
	(void) args;
}

static struct mLogger _logger = { .log = _nullLog };

static bool _dump(const char* dir, const char* name, const void* data, size_t size) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	fwrite(data, 1, size, f);
	fclose(f);
	return true;
}

int main(int argc, char** argv) {
	int frames = 600;
	const char* dumpDir = NULL;
	uint32_t regsAddress = 0;
	bool regsThumb = false;
	const char* input = NULL;
	int i;
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc && _watch.count < MAX_WATCH) {
			char* end;
			_watch.lo[_watch.count] = strtoul(argv[++i], &end, 16);
			_watch.hi[_watch.count] = *end == '-' ? strtoul(end + 1, NULL, 16) : _watch.lo[_watch.count] + 1;
			++_watch.count;
		} else if (strcmp(argv[i], "--regs") == 0 && i + 2 < argc) {
			char* end;
			regsAddress = strtoul(argv[++i], &end, 16);
			regsThumb = *end == 't';
			_regsLeft = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dumpDir = argv[++i];
		} else if (argv[i][0] != '-' && !input) {
			input = argv[i];
		} else {
			fprintf(stderr, "usage: %s [--frames N] [--watch LO-HI]... [--regs ADDR[t] N] [--dump DIR] INPUT.minigsf\n", argv[0]);
			return 1;
		}
	}
	if (!input) {
		fprintf(stderr, "usage: %s [--frames N] [--watch LO-HI]... [--regs ADDR[t] N] [--dump DIR] INPUT.minigsf\n", argv[0]);
		return 1;
	}
	mLogSetDefaultLogger(&_logger);

	struct GSFImage image;
	struct PSFTags tags;
	char err[512];
	if (!GSFLoad(input, &image, &tags, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	bool multiboot = (image.entry >> 24) == 0x02;
	struct VFile* vf = VFileFromConstMemory(image.data, image.size);
	struct mCore* core = mCoreCreate(mPLATFORM_GBA);
	core->init(core);
	mCoreInitConfig(core, NULL);
	struct mCoreOptions coreOpts = { .skipBios = true, .volume = 0x100 };
	mCoreConfigLoadDefaults(&core->config, &coreOpts);
	core->loadConfig(core, &core->config);
	struct GBA* gba = core->board;
	if (multiboot ? !GBALoadMB(gba, vf) : !GBALoadROM(gba, vf)) {
		fprintf(stderr, "Could not load program image\n");
		return 1;
	}
	core->reset(core);
	if (multiboot) {
		size_t size = image.size < GBA_SIZE_EWRAM ? image.size : GBA_SIZE_EWRAM;
		memcpy(gba->memory.wram, image.data, size);
	}
	gba->cpu->gprs[ARM_PC] = image.entry;
	ARMWritePC(gba->cpu);

	struct GBACodeHook regsHook = { .hit = _regsHit };
	if (regsAddress) {
		GBAInstallCodeHook(gba, &regsHook, regsAddress, regsThumb ? MODE_THUMB : MODE_ARM);
	}
	if (_watch.count) {
		_watch.cpu = gba->cpu;
		_watch.orig = gba->cpu->memory;
		gba->cpu->memory.store32 = _store32;
		gba->cpu->memory.store16 = _store16;
		gba->cpu->memory.store8 = _store8;
		gba->cpu->memory.storeMultiple = _storeMultiple;
	}

	struct DMAConfig {
		uint32_t source, dest;
		uint16_t control;
		int first;
		int seen;
	} configs[32];
	int nConfigs = 0;
	int f;
	for (f = 0; f < frames; ++f) {
		_frame = f;
		core->runFrame(core);
		int d;
		for (d = 1; d <= 2; ++d) {
			struct GBADMA* dma = &gba->memory.dma[d];
			if (!GBADMARegisterIsEnable(dma->reg)) {
				continue;
			}
			// The registered source, not the moving pointer (nextSource)
			uint32_t source = dma->source;
			uint32_t dest = dma->dest;
			int c;
			for (c = 0; c < nConfigs; ++c) {
				if (configs[c].source == source && configs[c].dest == dest && configs[c].control == dma->reg) {
					break;
				}
			}
			if (c == nConfigs && nConfigs < 32) {
				configs[nConfigs++] = (struct DMAConfig) { source, dest, dma->reg, f, 0 };
			}
			if (c < 32) {
				++configs[c].seen;
			}
		}
	}
	printf("Sound DMA configurations (registered source, dest, control; first frame, frames seen):\n");
	for (i = 0; i < nConfigs; ++i) {
		printf("  %08X -> %08X  ctl %04X  from frame %d, %d frames\n", configs[i].source, configs[i].dest,
		       configs[i].control, configs[i].first, configs[i].seen);
	}
	printf("Timers: TM0 reload %04X ctl %04X, TM1 reload %04X ctl %04X; SOUNDCNT_H %04X\n",
	       gba->timers[0].reload, gba->memory.io[GBA_REG_TM0CNT_HI >> 1], gba->timers[1].reload,
	       gba->memory.io[GBA_REG_TM1CNT_HI >> 1], gba->memory.io[GBA_REG_SOUNDCNT_HI >> 1]);
	if (_watch.count) {
		printf("Writers (pc, mode, width, count, per frame, range):\n");
		int w;
		for (w = 0; w < _watch.nWriters; ++w) {
			struct Writer* wr = &_watch.writers[w];
			printf("  %08X %s w%d  %10llu  %8.1f/frame  %08X-%08X\n", wr->pc, wr->thumb ? "thumb" : "arm  ", wr->width,
			       (unsigned long long) wr->count, (double) wr->count / frames, wr->lo, wr->hi + wr->width - 1);
		}
	}
	if (dumpDir) {
		_dump(dumpDir, "iwram.bin", gba->memory.iwram, GBA_SIZE_IWRAM);
		_dump(dumpDir, "ewram.bin", gba->memory.wram, GBA_SIZE_EWRAM);
		_dump(dumpDir, "rom.bin", image.data, image.size);
		printf("Dumped IWRAM, EWRAM and the ROM image to %s\n", dumpDir);
	}
	core->deinit(core);
	return 0;
}
