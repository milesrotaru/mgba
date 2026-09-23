/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_BLMIX_H
#define GSF2WAV_BLMIX_H

#include <stddef.h>
#include <stdint.h>

// Band-limited renderer for event streams timestamped in source clock cycles.
//
// Two kinds of input are supported, and both are evaluated in continuous time
// directly at the output sample instants, so there is no intermediate sample
// grid anywhere in the chain:
//
//  - Steps: a piecewise-constant signal described by its jumps. Rendered as a
//    sum of band-limited steps (the BLEP idea, what blip_buf does), lowpassed
//    at the output rate's Nyquist. Each step is splatted as first differences
//    of the tabulated integral of the kernel, so the running sum reproduces
//    exact samples of the band-limited step. (Summing sampled impulses instead
//    would tilt the response by 1/sinc(f / rate).) Right for signals that really are
//    piecewise-constant, like the PSG channels.
//
//  - Points: a sequence of samples of a band-limited signal at some source
//    rate. Rendered by windowed-sinc interpolation with the cutoff at the lower
//    of the source and output Nyquists. Right for PCM streams whose staircase
//    shape is a DAC artifact rather than part of the data.
//
// The kernel is a Kaiser-windowed sinc, tabulated finely enough that table
// interpolation error sits far below the window's own stopband.
struct BLMixer {
	double clockRate;
	double outRate;
	double cyclesPerSample;
	double outCutoff; // Normalized to output rate
	double minCutoff;

	int zeroCrossings;
	int tableRes;
	double* table;
	double* stepTable;

	size_t capacity;
	size_t mask;
	double* stepL;
	double* stepR;
	double* pointL;
	double* pointR;

	uint64_t base;
	double levelL;
	double levelR;
	int maxHalfWidth;
	double* taps;

	uint64_t lateEvents;
};

void BLMixerInit(struct BLMixer* mixer, double clockRate, double outRate);
void BLMixerDeinit(struct BLMixer* mixer);

// Add a jump of (dl, dr) at the given cycle.
void BLMixerStep(struct BLMixer* mixer, double cycle, double dl, double dr);

// Add a sample of value (l, r) centered at the given cycle, from a stream whose
// sample period is `period` cycles.
void BLMixerPoint(struct BLMixer* mixer, double cycle, double period, double l, double r);

// Emit every output sample that no future event (at or after `now`) can still
// affect. Writes interleaved stereo; returns the number of frames written.
size_t BLMixerRead(struct BLMixer* mixer, double now, double* out, size_t maxFrames);

#endif
