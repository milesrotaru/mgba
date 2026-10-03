/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "hooks.h"

#include "capture.h"
#include "msglog.h"

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/memory.h>

#include <math.h>
#include <string.h>

static uint8_t _view8(struct MP2KMemory* mem, uint32_t address) {
	return GBAView8(((struct MemoryView*) mem)->cpu, address);
}

static uint32_t _view32(struct MP2KMemory* mem, uint32_t address) {
	return GBAView32(((struct MemoryView*) mem)->cpu, address);
}

void MemoryViewInit(struct MemoryView* view, struct ARMCore* cpu) {
	view->d.read8 = _view8;
	view->d.read32 = _view32;
	view->cpu = cpu;
}

static void _mp2kVerifyCheck(struct MP2KHook* v) {
	if (!v->pending) {
		return;
	}
	v->pending = false;
	int32_t j;
	uint64_t bad = 0;
	for (j = 0; j < v->frame.samplesPerVBlank; ++j) {
		int8_t a = GBAView8(v->mem.cpu, v->frame.segment + j);
		int8_t b = GBAView8(v->mem.cpu, v->frame.segment + j + MP2K_PCM_DMA_BUF_SIZE);
		bad += (a != v->half0[j]) + (b != v->half1[j]);
	}
	++v->frames;
	v->samples += v->frame.samplesPerVBlank * 2;
	if (bad) {
		if (v->badFrames < 5) {
			MsgWrite(MSG_WARN, "MP2K verify: frame %llu differs in %llu of %d samples", (unsigned long long) v->frames, (unsigned long long) bad, v->frame.samplesPerVBlank * 2);
		}
		++v->badFrames;
		v->badSamples += bad;
	}
}

static void _adVerifyCheck(struct ADHook* v) {
	if (!v->pending) {
		return;
	}
	v->pending = false;
	int32_t j;
	uint64_t bad = 0;
	for (j = 0; j < v->frame.count; ++j) {
		bad += ((int8_t) GBAView8(v->mem.cpu, v->frame.outA + j) != v->outA[j]) +
		       ((int8_t) GBAView8(v->mem.cpu, v->frame.outB + j) != v->outB[j]);
	}
	++v->frames;
	v->samples += v->frame.count * 2;
	if (bad) {
		if (v->badFrames < 5) {
			MsgWrite(MSG_WARN, "AlphaDream verify: frame %llu differs in %llu of %d samples", (unsigned long long) v->frames,
			        (unsigned long long) bad, v->frame.count * 2);
		}
		++v->badFrames;
		v->badSamples += bad;
	}
}

static void _adHit(struct GBACodeHook* hook, struct GBA* gba) {
	struct ADHook* v = (struct ADHook*) hook;
	_adVerifyCheck(v);
	struct ARMCore* cpu = gba->cpu;
	int32_t count = cpu->gprs[1];
	if (count <= 0 || count > AD_MAX_SAMPLES_PER_FRAME || (count & 3)) {
		return;
	}
	ADFrameRead(&v->frame, &v->driver, &v->mem.d, count, cpu->gprs[2], cpu->gprs[3]);
	// The driver doesn't keep its own rate; FIFO A's timer has it
	uint16_t reload = gba->timers[v->audio->chATimer].reload;
	v->frame.pcmFreq = reload ? (int32_t) lround(GBA_ARM7TDMI_FREQUENCY / (double) (0x10000 - reload)) : 0;
	if (!v->printedInfo) {
		v->printedInfo = true;
		MsgWrite(MSG_DETAIL, "AlphaDream: channels %08X, samples %08X, %d Hz, %d samples/frame", v->driver.channels,
		        v->driver.sampleTable, v->frame.pcmFreq, count);
	}
	if (v->hifi) {
		if (v->bandwidthDriver && v->frame.pcmFreq > 0) {
			v->hifi->bandwidth = v->frame.pcmFreq * 0.5;
		}
		double gains[2][2];
		CaptureFifoGains(v->audio, 0, &gains[0][0], &gains[0][1]);
		CaptureFifoGains(v->audio, 1, &gains[1][0], &gains[1][1]);
		HiFiFrame(v->hifi, mTimingGlobalTime(&gba->timing), &v->frame, gains);
	}
	if (v->verify) {
		struct ADFrame copy = v->frame;
		ADMixExact(&copy, &v->mem.d, v->outA, v->outB);
		v->pending = true;
	}
}

static void _mp2kHit(struct GBACodeHook* hook, struct GBA* gba) {
	struct MP2KHook* v = (struct MP2KHook*) hook;
	_mp2kVerifyCheck(v);
	struct ARMCore* cpu = gba->cpu;
	MP2KFrameRead(&v->frame, &v->mem.d, cpu->gprs[0], cpu->gprs[5]);
	if (!v->printedInfo) {
		v->printedInfo = true;
		MsgWrite(MSG_DETAIL, "MP2K: SoundInfo %08X, %d Hz, %d samples/frame, %d channels, reverb %d, master volume %d, maxLines %d, DMA period %d",
		        v->frame.info, v->frame.pcmFreq, v->frame.samplesPerVBlank, v->frame.maxChans, v->frame.reverb,
		        v->frame.masterVolume, v->frame.maxLines, v->frame.pcmDmaPeriod);
	}
	if (v->frame.samplesPerVBlank <= 0 || v->frame.samplesPerVBlank > MP2K_MAX_SAMPLES_PER_VBLANK) {
		return;
	}
	if (v->hifi) {
		if (v->bandwidthDriver && v->frame.pcmFreq > 0) {
			v->hifi->bandwidth = v->frame.pcmFreq * 0.5;
		}
		double gains[2][2];
		CaptureFifoGains(v->audio, 0, &gains[0][0], &gains[0][1]);
		CaptureFifoGains(v->audio, 1, &gains[1][0], &gains[1][1]);
		HiFiFrame(v->hifi, mTimingGlobalTime(&gba->timing), &v->frame, gains);
	}
	if (v->verify) {
		struct MP2KFrame copy = v->frame;
		MP2KMixExact(&copy, &v->mem.d, v->half0, v->half1);
		v->pending = true;
	}
}

void MP2KHookInstall(struct MP2KHook* hook, struct GBA* gba, uint32_t address, bool verify, struct HiFi* hifi,
                     bool bandwidthDriver) {
	memset(hook, 0, sizeof(*hook));
	MemoryViewInit(&hook->mem, gba->cpu);
	hook->verify = verify;
	hook->audio = &gba->audio;
	hook->hifi = hifi;
	hook->bandwidthDriver = bandwidthDriver;
	hook->d.hit = _mp2kHit;
	GBAInstallCodeHook(gba, &hook->d, address, MODE_THUMB);
}

void ADHookInstall(struct ADHook* hook, struct GBA* gba, const struct ADDriver* driver, bool verify, struct HiFi* hifi,
                   bool bandwidthDriver) {
	memset(hook, 0, sizeof(*hook));
	hook->driver = *driver;
	MemoryViewInit(&hook->mem, gba->cpu);
	hook->verify = verify;
	hook->audio = &gba->audio;
	hook->hifi = hifi;
	hook->bandwidthDriver = bandwidthDriver;
	hook->d.hit = _adHit;
	GBAInstallCodeHook(gba, &hook->d, hook->driver.mix, MODE_THUMB);
}
