/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "blmix.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// 48 zero crossings per side with beta 12.3 gives roughly 120 dB stopband and
// a transition band of about 16% of the cutoff frequency.
#define ZERO_CROSSINGS 48
#define KAISER_BETA 12.3
#define TABLE_RES 8192

// The output lowpass is centered below Nyquist by enough that the transition
// band is finished before it.
#define OUT_CUTOFF 0.46
// Point streams are lowpassed a little below their own Nyquist for the same
// reason: images of the source spectrum start right above it.
#define SOURCE_CUTOFF 0.47
// Lowest source-stream cutoff honored, in Hz. Bounds the kernel length.
#define MIN_SOURCE_CUTOFF 900.0

static double _besselI0(double x) {
	double sum = 1;
	double term = 1;
	double k;
	for (k = 1; k < 200; ++k) {
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
		if (term < sum * 1e-17) {
			break;
		}
	}
	return sum;
}

void BLMixerInit(struct BLMixer* mixer, double clockRate, double outRate) {
	memset(mixer, 0, sizeof(*mixer));
	mixer->clockRate = clockRate;
	mixer->outRate = outRate;
	mixer->cyclesPerSample = clockRate / outRate;
	mixer->outCutoff = OUT_CUTOFF;
	mixer->minCutoff = MIN_SOURCE_CUTOFF / outRate;
	if (mixer->minCutoff > mixer->outCutoff) {
		mixer->minCutoff = mixer->outCutoff;
	}
	mixer->zeroCrossings = ZERO_CROSSINGS;
	mixer->tableRes = TABLE_RES;

	// Table of sinc(u) * kaiser(u / Z) for u in [0, Z], plus a guard entry
	size_t entries = (size_t) ZERO_CROSSINGS * TABLE_RES + 2;
	mixer->table = malloc(entries * sizeof(double));
	double i0beta = _besselI0(KAISER_BETA);
	size_t i;
	for (i = 0; i < entries; ++i) {
		double u = (double) i / TABLE_RES;
		double sinc = u == 0 ? 1 : sin(M_PI * u) / (M_PI * u);
		double r = u / ZERO_CROSSINGS;
		double window = r >= 1 ? 0 : _besselI0(KAISER_BETA * sqrt(1 - r * r)) / i0beta;
		mixer->table[i] = sinc * window;
	}
	// Running integral of the kernel from 0, by the trapezoid rule, scaled so
	// the full integral over [-Z, Z] is exactly 1. The step response at u is
	// then 0.5 + stepTable[u] for u >= 0 and 0.5 - stepTable[-u] otherwise.
	mixer->stepTable = malloc(entries * sizeof(double));
	mixer->stepTable[0] = 0;
	for (i = 1; i < entries; ++i) {
		mixer->stepTable[i] = mixer->stepTable[i - 1] + (mixer->table[i - 1] + mixer->table[i]) * 0.5 / TABLE_RES;
	}
	double half = mixer->stepTable[(size_t) ZERO_CROSSINGS * TABLE_RES];
	for (i = 0; i < entries; ++i) {
		mixer->stepTable[i] *= 0.5 / half;
	}

	mixer->maxHalfWidth = (int) ceil(ZERO_CROSSINGS / (2 * mixer->minCutoff)) + 2;
	size_t capacity = 1;
	while (capacity < (size_t) mixer->maxHalfWidth * 8 + 0x10000) {
		capacity <<= 1;
	}
	mixer->capacity = capacity;
	mixer->mask = capacity - 1;
	mixer->stepL = calloc(capacity, sizeof(double));
	mixer->stepR = calloc(capacity, sizeof(double));
	mixer->pointL = calloc(capacity, sizeof(double));
	mixer->pointR = calloc(capacity, sizeof(double));
	mixer->taps = malloc((mixer->maxHalfWidth * 2 + 4) * sizeof(double));
}

void BLMixerDeinit(struct BLMixer* mixer) {
	free(mixer->table);
	free(mixer->stepTable);
	free(mixer->stepL);
	free(mixer->stepR);
	free(mixer->pointL);
	free(mixer->pointR);
	free(mixer->taps);
	memset(mixer, 0, sizeof(*mixer));
}

static inline double _kernel(const struct BLMixer* mixer, double u) {
	u = fabs(u) * mixer->tableRes;
	size_t i = (size_t) u;
	if (i >= (size_t) mixer->zeroCrossings * mixer->tableRes) {
		return 0;
	}
	double frac = u - i;
	return mixer->table[i] + (mixer->table[i + 1] - mixer->table[i]) * frac;
}

static inline double _step(const struct BLMixer* mixer, double u) {
	double a = fabs(u) * mixer->tableRes;
	double v;
	size_t i = (size_t) a;
	if (i >= (size_t) mixer->zeroCrossings * mixer->tableRes) {
		v = 0.5;
	} else {
		double frac = a - i;
		v = mixer->stepTable[i] + (mixer->stepTable[i + 1] - mixer->stepTable[i]) * frac;
	}
	return u < 0 ? 0.5 - v : 0.5 + v;
}

// Computes the taps of a lowpass kernel with the given normalized cutoff,
// centered at output position x. Returns the first sample index covered.
static int64_t _taps(struct BLMixer* mixer, double x, double cutoff, int* count) {
	double* taps = mixer->taps;
	double scale = 2 * cutoff;
	double halfWidth = mixer->zeroCrossings / scale;
	int64_t first = (int64_t) ceil(x - halfWidth);
	int64_t last = (int64_t) floor(x + halfWidth);
	if (first < (int64_t) mixer->base) {
		// Kernels reaching back before time zero are just clipped
		if (mixer->base > 0) {
			++mixer->lateEvents;
		}
		first = mixer->base;
	}
	if (last >= (int64_t) (mixer->base + mixer->capacity)) {
		last = mixer->base + mixer->capacity - 1;
	}
	int n = 0;
	int64_t i;
	for (i = first; i <= last; ++i) {
		taps[n++] = scale * _kernel(mixer, scale * (i - x));
	}
	*count = n;
	return first;
}

void BLMixerStep(struct BLMixer* mixer, double cycle, double dl, double dr) {
	if (dl == 0 && dr == 0) {
		return;
	}
	double x = cycle / mixer->cyclesPerSample;
	double scale = 2 * mixer->outCutoff;
	double halfWidth = mixer->zeroCrossings / scale;
	int64_t first = (int64_t) ceil(x - halfWidth);
	int64_t last = (int64_t) floor(x + halfWidth) + 1;
	if (last >= (int64_t) (mixer->base + mixer->capacity)) {
		last = mixer->base + mixer->capacity - 1;
	}
	// Sample n gets step(n) - step(n - 1). Anything before the oldest
	// unfinalized sample is folded into that sample, so the step still
	// integrates to its full height.
	int64_t i = first;
	if (i < (int64_t) mixer->base) {
		if (mixer->base > 0) {
			++mixer->lateEvents;
		}
		i = mixer->base;
	}
	double prev = i > first ? _step(mixer, scale * (i - 1 - x)) : 0;
	for (; i <= last; ++i) {
		double cur = i == last ? 1 : _step(mixer, scale * (i - x));
		size_t idx = i & mixer->mask;
		mixer->stepL[idx] += dl * (cur - prev);
		mixer->stepR[idx] += dr * (cur - prev);
		prev = cur;
	}
}

void BLMixerPoint(struct BLMixer* mixer, double cycle, double period, double l, double r) {
	if (l == 0 && r == 0) {
		return;
	}
	double periodOut = period / mixer->cyclesPerSample;
	double cutoff = SOURCE_CUTOFF / periodOut;
	if (cutoff > mixer->outCutoff) {
		cutoff = mixer->outCutoff;
	}
	if (cutoff < mixer->minCutoff) {
		cutoff = mixer->minCutoff;
	}
	const double* taps = mixer->taps;
	int count;
	double x = cycle / mixer->cyclesPerSample;
	int64_t first = _taps(mixer, x, cutoff, &count);
	int i;
	for (i = 0; i < count; ++i) {
		size_t idx = (first + i) & mixer->mask;
		double t = taps[i] * periodOut;
		mixer->pointL[idx] += l * t;
		mixer->pointR[idx] += r * t;
	}
}

size_t BLMixerRead(struct BLMixer* mixer, double now, double* out, size_t maxFrames) {
	double x = now / mixer->cyclesPerSample;
	int64_t limit = (int64_t) floor(x) - mixer->maxHalfWidth;
	size_t n = 0;
	while ((int64_t) mixer->base < limit && n < maxFrames) {
		size_t idx = mixer->base & mixer->mask;
		mixer->levelL += mixer->stepL[idx];
		mixer->levelR += mixer->stepR[idx];
		out[n * 2] = mixer->levelL + mixer->pointL[idx];
		out[n * 2 + 1] = mixer->levelR + mixer->pointR[idx];
		mixer->stepL[idx] = 0;
		mixer->stepR[idx] = 0;
		mixer->pointL[idx] = 0;
		mixer->pointR[idx] = 0;
		++mixer->base;
		++n;
	}
	return n;
}

bool BLMixerAdd(struct BLMixer* mixer, int64_t index, double l, double r) {
	if (index < (int64_t) mixer->base || index >= (int64_t) (mixer->base + mixer->capacity)) {
		++mixer->lateEvents;
		return false;
	}
	size_t idx = index & mixer->mask;
	mixer->pointL[idx] += l;
	mixer->pointR[idx] += r;
	return true;
}
