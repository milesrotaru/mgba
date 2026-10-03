/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_HOOKS_H
#define GSF2WAV_HOOKS_H

#include "alphadream.h"
#include "hifi.h"
#include "mp2k.h"

#include <mgba/internal/gba/gba.h>

// Code hooks that catch each sound driver at the point in its frame where its
// state is complete but it hasn't mixed yet. Each frame the hook hands that
// state to the renderer (HiFi) and, for --verify, mixes a copy with the C port
// and compares it with what the game writes.

struct MemoryView {
	struct MP2KMemory d;
	struct ARMCore* cpu;
};

struct MP2KHook {
	struct GBACodeHook d;
	struct MemoryView mem;
	bool verify;
	struct HiFi* hifi;
	struct GBAAudio* audio;
	bool bandwidthDriver;
	bool pending;
	struct MP2KFrame frame;
	int8_t half0[MP2K_MAX_SAMPLES_PER_VBLANK];
	int8_t half1[MP2K_MAX_SAMPLES_PER_VBLANK];
	uint64_t frames;
	uint64_t badFrames;
	uint64_t badSamples;
	uint64_t samples;
	bool printedInfo;
};

struct ADHook {
	struct GBACodeHook d;
	struct MemoryView mem;
	struct ADDriver driver;
	bool verify;
	struct HiFi* hifi;
	struct GBAAudio* audio;
	bool bandwidthDriver;
	bool pending;
	struct ADFrame frame;
	int8_t outA[AD_MAX_SAMPLES_PER_FRAME];
	int8_t outB[AD_MAX_SAMPLES_PER_FRAME];
	uint64_t frames;
	uint64_t badFrames;
	uint64_t badSamples;
	uint64_t samples;
	bool printedInfo;
};

void MemoryViewInit(struct MemoryView* view, struct ARMCore* cpu);

// hifi may be NULL, to only verify. bandwidthDriver is --bandwidth driver.
void MP2KHookInstall(struct MP2KHook* hook, struct GBA* gba, uint32_t address, bool verify, struct HiFi* hifi,
                     bool bandwidthDriver);
void ADHookInstall(struct ADHook* hook, struct GBA* gba, const struct ADDriver* driver, bool verify, struct HiFi* hifi,
                   bool bandwidthDriver);

#endif
