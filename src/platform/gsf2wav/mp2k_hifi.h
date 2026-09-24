/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_MP2K_HIFI_H
#define GSF2WAV_MP2K_HIFI_H

#include "blmix.h"
#include "mp2k.h"

// High-precision re-render of the MP2K driver's PCM voices, and of its reverb
// (a mono feedback echo of the segments mixed one DMA period and one period
// less a frame ago).
//
// The driver's own state (read from the game every frame, after the
// sequencer) decides which notes play, where each voice is in its sample, its
// pitch and its envelope. Instead of the driver's mix (linear interpolation to
// the mixing rate, per-voice truncation to 8 bits, wrapping sums), each voice
// is resampled straight from its source PCM to the output rate with a
// windowed sinc, bandlimited to both its own Nyquist and the output's, and
// summed in double precision.
//
// Timing comes from the hardware: the FIFO latch clock is uniform, and the
// driver's frames are laid end to end on it, so once one frame has been found
// in the FIFO stream every later frame lands exactly where the hardware played
// it.

#define MP2K_HIFI_MAX_GHOSTS 24
#define MP2K_HIFI_MAX_PENDING 4096

struct MP2KHiFiVoice {
	bool active;
	uint32_t wav;
	const int8_t* data;
	int64_t size;
	int64_t loopStart;
	int64_t loopLength;
	bool fixed;
	// Render like the driver: linear interpolation at its mixing rate
	bool linear;
	int channel;

	// Unwrapped position in source samples at the start of the current frame,
	// and the step per mixer sample
	double u;
	double step;

	// Gain into each segment half, in driver sample units (1.0 = one int8 LSB
	// per unit of source sample), now and at the end of the previous frame
	double gain[2];
	double prevGain[2];
	bool rampIn;

	// For ghosts: the time at which the ramp to silence started
	double stopTime;
};

struct MP2KHiFiPending {
	uint64_t hookTime;
	struct MP2KFrame frame;
	double fifoGain[2][2];
};

// Per-sample usage statistics (--sample-stats)
#define MP2K_HIFI_MAX_SAMPLE_STATS 2048

struct MP2KSampleStats {
	uint32_t wav;
	uint32_t notes;
	double seconds;
	double maxRate;
	double rateSeconds; // integral of playback rate over time, for the mean
	double maxGain;
};

enum MP2KHiFiMode {
	// Each voice sinc-resampled from its source straight to the output rate
	MP2K_HIFI_SINC,
	// The driver's own resampling (linear, at its mixing rate), without its
	// 8-bit truncation, then sinc-reconstructed like the FIFO stream
	MP2K_HIFI_LINEAR,
};

struct MP2KHiFi {
	enum MP2KHiFiMode mode;
	uint32_t mutedChannels;
	// If nonzero, only voices playing this sample (its header address) sound
	uint32_t soloWav;
	// Samples (header addresses) to render like the driver in sinc mode
	uint32_t linearWavs[64];
	size_t linearWavCount;
	// Upper limit on each voice's bandwidth in Hz (0: the output's Nyquist)
	double bandwidth;
	// Each voice's cutoff as a fraction of the rate its source is played at
	double sourceCutoff;
	struct BLMixer* out;
	struct MP2KMemory* mem;
	const uint8_t* rom;
	size_t romSize;
	double clockRate;
	double ramp;

	// Voice resampling kernel
	int zeroCrossings;
	int tableRes;
	double* table;
	double* taps;

	struct MP2KHiFiVoice voices[MP2K_MAX_CHANNELS];
	struct MP2KHiFiVoice ghosts[MP2K_HIFI_MAX_GHOSTS];

	// Lock onto the FIFO clock
	bool locked;
	bool failed;
	double latchPeriod;
	double t0;
	int halfForFifo[2];
	uint64_t frameIndex;
	int32_t samplesPerVBlank;

	int8_t* fifoHistory[2];
	uint64_t* fifoTimes[2];
	size_t fifoCount[2];
	size_t fifoCapacity;

	struct MP2KHiFiPending* pending;
	size_t pendingCount;
	int8_t (*pendingExact)[2][32];

	// Sinc mode: voices render into per-half buffers at the output rate, which
	// are flushed in order through the driver's reverb and the FIFO routing
	size_t ringMask;
	double* half[2];
	double* mono;
	double* reverbGain;
	int64_t flushed;
	double route[2][2];
	double reverbDelay[2];

	// Linear mode: the last few frames of the mix, for the reverb
	double* linearHistory;
	int32_t linearHistoryFrames;

	double horizon;
	uint64_t lostSamples;
	uint64_t resyncs;
	uint64_t droppedGhosts;

	bool collectStats;
	struct MP2KSampleStats* stats;
	size_t statsCount;
};

void MP2KHiFiInit(struct MP2KHiFi* hifi, struct BLMixer* out, struct MP2KMemory* mem, const uint8_t* rom, size_t romSize,
                  double clockRate, double rampSeconds);
void MP2KHiFiDeinit(struct MP2KHiFi* hifi);

// Called at the hook, with the FIFO routing gains in effect (per FIFO, left
// and right, in output units per int8 LSB).
void MP2KHiFiFrame(struct MP2KHiFi* hifi, uint64_t hookTime, const struct MP2KFrame* frame, const double fifoGain[2][2]);

void MP2KHiFiFifo(struct MP2KHiFi* hifi, int fifo, uint64_t when, int8_t sample);

// Output is complete up to this cycle.
double MP2KHiFiHorizon(const struct MP2KHiFi* hifi);

#endif
