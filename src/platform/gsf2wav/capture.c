/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "capture.h"

#include <mgba/core/timing.h>
#include <mgba/internal/gba/gba.h>

#include <math.h>
#include <string.h>

void CaptureFifoGains(struct GBAAudio* audio, int fifo, double* left, double* right) {
	bool enableL, enableR, full, forceOff;
	if (fifo == 0) {
		enableL = audio->chALeft;
		enableR = audio->chARight;
		full = audio->volumeChA;
		forceOff = audio->forceDisableChA;
	} else {
		enableL = audio->chBLeft;
		enableR = audio->chBRight;
		full = audio->volumeChB;
		forceOff = audio->forceDisableChB;
	}
	// Matches the core's (sample << 2) >> !volume, without the truncation
	double gain = forceOff ? 0 : (full ? 4 : 2) / DAC_SCALE;
	*left = enableL ? gain : 0;
	*right = enableR ? gain : 0;
}

void CaptureSync(struct Capture* cap, struct GBAAudio* audio, int32_t timestamp) {
	int32_t current = mTimingCurrentTime(cap->timing);
	uint64_t now = mTimingGlobalTime(cap->timing) + (timestamp - current);

	// The PSG runs lazily inside the core, so it can be stepped through any
	// interval in which no register changed. This observer is called before
	// every such change, so walking the grid here sees every level the PSG
	// actually produced, to within one grid step.
	uint64_t t;
	for (t = cap->psgLast + cap->psgGrid; t <= now; t += cap->psgGrid) {
		GBAudioRun(&audio->psg, timestamp - (int32_t) (now - t), 0xF);
		int16_t l = 0;
		int16_t r = 0;
		GBAudioSamplePSG(&audio->psg, &l, &r);
		double scale = cap->mutePsg ? 0 : 1.0 / (1 << (4 - audio->volume)) / DAC_SCALE;
		double sl = l * scale;
		double sr = r * scale;
		if (sl != cap->psgL || sr != cap->psgR) {
			// The edge happened somewhere in the last grid step; its midpoint is
			// the unbiased estimate
			BLMixerStep(cap->mixer, t - cap->psgGrid * 0.5, sl - cap->psgL, sr - cap->psgR);
			cap->psgL = sl;
			cap->psgR = sr;
		}
		cap->psgLast = t;
	}

	if (cap->fifoHold) {
		// Mixer control changes act immediately on held FIFO levels
		int fifo;
		for (fifo = 0; fifo < 2; ++fifo) {
			double gl, gr;
			CaptureFifoGains(audio, fifo, &gl, &gr);
			double l = cap->fifoValue[fifo] * gl;
			double r = cap->fifoValue[fifo] * gr;
			if (l != cap->fifoLevelL[fifo] || r != cap->fifoLevelR[fifo]) {
				BLMixerStep(cap->mixer, now, l - cap->fifoLevelL[fifo], r - cap->fifoLevelR[fifo]);
				cap->fifoLevelL[fifo] = l;
				cap->fifoLevelR[fifo] = r;
			}
		}
	}
}

static void _captureFifo(struct GBAAudioObserver* observer, struct GBAAudio* audio, int fifo, uint64_t when, int8_t sample) {
	struct Capture* cap = (struct Capture*) observer;
	double gl, gr;
	CaptureFifoGains(audio, fifo, &gl, &gr);
	bool replaced = false;
	if (cap->hifi && !cap->hifi->failed) {
		// The driver's voices are rendered from its state instead
		HiFiFifo(cap->hifi, fifo, when, sample);
		replaced = true;
		sample = 0;
	} else if (cap->muteFifo) {
		sample = 0;
	}
	cap->fifoValue[fifo] = sample;

	// Track the stream's sample period. A gap much longer than the last
	// period means the timer stopped and restarted, so keep the old period
	// rather than treating the silence as one very long sample. A shorter one
	// means the rate went up, which is taken as-is; a longer one is only
	// believed once it repeats.
	double period = cap->fifoPeriod[fifo];
	if (cap->fifoCount[fifo]) {
		double dt = (double) (when - cap->fifoLast[fifo]);
		if (dt > 0 && (period == 0 || dt < period * 1.5)) {
			period = dt;
			cap->fifoLongRun[fifo] = 0;
		} else if (dt > 0) {
			if (fabs(dt - cap->fifoLongGap[fifo]) < dt * 0.01) {
				++cap->fifoLongRun[fifo];
			} else {
				cap->fifoLongGap[fifo] = dt;
				cap->fifoLongRun[fifo] = 1;
			}
			if (cap->fifoLongRun[fifo] >= 3) {
				period = dt;
			}
		}
	}
	cap->fifoLast[fifo] = when;
	++cap->fifoCount[fifo];
	if (period <= 0) {
		// First sample of a stream: nothing to measure yet
		period = GBA_ARM7TDMI_FREQUENCY / 16384.0;
	}
	cap->fifoPeriod[fifo] = period;

	if (replaced) {
		if (cap->fifoLevelL[fifo] != 0 || cap->fifoLevelR[fifo] != 0) {
			BLMixerStep(cap->mixer, when, -cap->fifoLevelL[fifo], -cap->fifoLevelR[fifo]);
			cap->fifoLevelL[fifo] = 0;
			cap->fifoLevelR[fifo] = 0;
		}
	} else if (cap->fifoHold) {
		double l = sample * gl;
		double r = sample * gr;
		BLMixerStep(cap->mixer, when, l - cap->fifoLevelL[fifo], r - cap->fifoLevelR[fifo]);
		cap->fifoLevelL[fifo] = l;
		cap->fifoLevelR[fifo] = r;
	} else {
		// The DAC holds each sample for one period, which delays the stream by
		// half a period relative to the PSG. Center the point to keep them
		// aligned.
		BLMixerPoint(cap->mixer, when + period * 0.5, period, sample * gl, sample * gr);
	}
}

static void _observerSync(struct GBAAudioObserver* observer, struct GBAAudio* audio, int32_t timestamp) {
	CaptureSync((struct Capture*) observer, audio, timestamp);
}

void CaptureInit(struct Capture* cap, struct GBAAudio* audio, struct BLMixer* mixer, struct mTiming* timing,
                 const struct RenderOptions* opts, struct HiFi* hifi) {
	memset(cap, 0, sizeof(*cap));
	cap->d.sync = _observerSync;
	cap->d.fifoSample = _captureFifo;
	cap->mixer = mixer;
	cap->timing = timing;
	cap->fifoHold = opts->fifoHold;
	cap->psgGrid = opts->psgGrid;
	cap->mutePsg = opts->mutePsg;
	cap->muteFifo = opts->muteFifo;
	cap->hifi = hifi;
	cap->psgLast = mTimingGlobalTime(timing);
	audio->observer = &cap->d;
}
