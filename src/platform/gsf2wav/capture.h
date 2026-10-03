/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_CAPTURE_H
#define GSF2WAV_CAPTURE_H

#include "blmix.h"
#include "hifi.h"
#include "options.h"

#include <mgba/internal/gba/audio.h>

// Full scale of the GBA's 10-bit DAC around its bias point. A single
// DirectSound channel at 100% volume spans exactly this.
#define DAC_SCALE 512.0

// Receives the raw DAC inputs from the core, bypassing the core's own
// resolution-limited sampling, bias clamp and output resampler: the PSG's
// levels as they change, and each DirectSound sample with the exact CPU cycle
// it latched at. When a driver is being re-rendered, its FIFO samples go to
// the renderer (to find its timing) instead of into the output.
struct Capture {
	struct GBAAudioObserver d;
	struct BLMixer* mixer;
	struct mTiming* timing;
	bool fifoHold;
	unsigned psgGrid;
	bool mutePsg;
	bool muteFifo;

	uint64_t psgLast;
	double psgL;
	double psgR;

	int8_t fifoValue[2];
	double fifoLevelL[2];
	double fifoLevelR[2];
	uint64_t fifoLast[2];
	double fifoPeriod[2];
	uint64_t fifoCount[2];
	double fifoLongGap[2];
	int fifoLongRun[2];

	struct HiFi* hifi;
};

void CaptureInit(struct Capture* cap, struct GBAAudio* audio, struct BLMixer* mixer, struct mTiming* timing,
                 const struct RenderOptions* opts, struct HiFi* hifi);
// Brings the PSG up to date, so everything it has produced so far is in the
// mixer. The core calls this itself before every register change.
void CaptureSync(struct Capture* cap, struct GBAAudio* audio, int32_t timestamp);

// The gains a FIFO's samples have on the left and right outputs, in output
// units per int8 LSB, as the mixer registers currently have them.
void CaptureFifoGains(struct GBAAudio* audio, int fifo, double* left, double* right);

#endif
