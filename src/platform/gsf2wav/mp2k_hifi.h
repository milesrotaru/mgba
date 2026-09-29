/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_MP2K_HIFI_H
#define GSF2WAV_MP2K_HIFI_H

#include "blmix.h"
#include "mp2k.h"

// High-precision re-render of a sound driver's PCM voices. It was written for
// MP2K (hence the names) and takes other drivers through struct HiFiDriver
// below; alphadream.c is the second one.
//
// For MP2K: the driver's voices, and its reverb
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
#define HIFI_MAX_VOICES MP2K_MAX_CHANNELS
#define HIFI_MAX_SAMPLES_PER_FRAME MP2K_MAX_SAMPLES_PER_VBLANK

// The renderer isn't tied to MP2K: a driver backend turns its own state, read
// at its hook, into this description of each voice as the driver is about to
// mix it (after its envelope step), and supplies its exact output (for the
// timing lock) and its own algorithm in floating point (for linear mode).
struct HiFiVoiceIn {
	bool on;      // mixed this frame
	bool started; // note (re)started this frame
	uint32_t wav; // the sample's identity (its header address)
	uint32_t data; // address of its first sample
	int64_t size;
	int64_t loopStart;
	int64_t loopLength; // 0: no loop
	bool fixed;         // MP2K fixed-rate sample
	bool driverHold;    // the driver doesn't interpolate this voice
	bool unsignedData;  // 8-bit samples centered on 0x80
	double pos;         // source samples at the frame's start
	double step;        // source samples per mixed sample
	double gain[2];     // into each output half, in int8 LSBs per source unit
};

struct HiFiFrameInfo {
	int32_t samplesPerVBlank;
	int32_t pcmFreq; // 0 if the driver doesn't know it
	uint8_t reverb;
	uint8_t pcmDmaPeriod;
};

struct HiFiDriver {
	const char* name;
	size_t frameSize;
	void (*info)(const void* frame, struct HiFiFrameInfo* info);
	bool (*anyActive)(const void* frame);
	// Fills out[0..HIFI_MAX_VOICES)
	void (*voices)(const void* frame, struct MP2KMemory* mem, struct HiFiVoiceIn* out);
	void (*mixExact)(const void* frame, struct MP2KMemory* mem, int8_t* half0, int8_t* half1);
	void (*mixFloat)(const void* frame, struct MP2KMemory* mem, uint32_t mutedChannels, uint32_t soloWav, double* half0,
	                 double* half1);
};

extern const struct HiFiDriver MP2KHiFiDriver;

struct MP2KHiFiVoice {
	bool active;
	uint32_t wav;
	uint32_t dataAddress;
	const int8_t* data; // in the ROM image, if it's there
	int64_t size;
	int64_t loopStart;
	int64_t loopLength;
	bool fixed;
	bool driverHold;
	bool unsignedData;
	// Render like the driver, at its mixing rate
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
	// Each voice linearly interpolated from its source straight at the output
	// rate, like the linear option in many sequenced-audio players: no
	// bandlimiting, so pitched-up samples alias and images leak above the
	// source band, but nothing is lost to the driver's mixing rate
	MP2K_HIFI_LERP,
	// Linear interpolation, then lowpassed at the output rate: the triangle
	// kernel convolved with the output's sinc. Keeps linear's images above
	// each source's band at low rates, and stops aliasing at high ones
	MP2K_HIFI_BLAM,
};

struct MP2KHiFi {
	const struct HiFiDriver* driver;
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
	// Blam: Q(y), the second integral of the kernel from 0 (even), and its
	// derivative, the first integral (odd), for y in [0, zeroCrossings]
	double* blamQ;
	double* blamS;

	struct MP2KHiFiVoice voices[HIFI_MAX_VOICES];
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
	uint8_t* pendingFrames; // the driver's frames, driver->frameSize each
	size_t pendingCount;
	// Per pending frame and half: the most distinctive window of the exact mix
	struct MP2KLockWindow {
		int8_t data[64];
		int16_t offset;
		bool usable;
	} (*pendingExact)[2];

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

void MP2KHiFiInit(struct MP2KHiFi* hifi, const struct HiFiDriver* driver, struct BLMixer* out, struct MP2KMemory* mem,
                  const uint8_t* rom, size_t romSize, double clockRate, double rampSeconds);
void MP2KHiFiDeinit(struct MP2KHiFi* hifi);

// Called at the hook, with the FIFO routing gains in effect (per FIFO, left
// and right, in output units per int8 LSB).
// frame is the driver's own frame type.
void MP2KHiFiFrame(struct MP2KHiFi* hifi, uint64_t hookTime, const void* frame, const double fifoGain[2][2]);

void MP2KHiFiFifo(struct MP2KHiFi* hifi, int fifo, uint64_t when, int8_t sample);

// Output is complete up to this cycle.
double MP2KHiFiHorizon(const struct MP2KHiFi* hifi);

#endif
