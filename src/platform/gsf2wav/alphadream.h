/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_ALPHADREAM_H
#define GSF2WAV_ALPHADREAM_H

#include "mp2k.h"

// AlphaDream's sound driver, as used by Mario & Luigi: Superstar Saga.
//
// Once per frame a Thumb routine in ROM (the "mix" function here) steps each
// of 8 channels' envelopes, works out its pitch, and has an ARM routine in
// IWRAM accumulate it into 32-bit words: unsigned 8-bit samples times a
// packed stereo volume (left in the high half, right in the low), nearest
// sample, 10-bit position fraction, no interpolation. A second ARM routine
// then takes bits 8-15 of each half (after removing the unsigned samples'
// bias), which wrap rather than clip, and writes FIFO A (left) and FIFO B
// (right).
//
// Channel, 16 bytes:
//   +0 state: 0 off, 0x80 note on, 0x81 attack, 0x82 decay, 0x83 sustain,
//      anything else release
//   +1 envelope level          +2 sample index (bit 15: fixed pitch)
//   +4 position (22.10)        +8 right volume   +9 left volume
//   +A fine tune               +B note
//   +C attack  +D decay  +E sustain level  +F release
// Sample header: +3 flags (0x40 loop), +4 base rate, +8 loop start, +C end,
// +10 data (unsigned 8-bit).

#define AD_CHANNELS 8
#define AD_MAX_SAMPLES_PER_FRAME 1024

struct ADDriver {
	uint32_t mix;         // the per-frame routine (Thumb)
	uint32_t channels;    // channel array
	uint32_t sampleTable; // sample index -> header offset, relative to the table
	uint32_t freqTable;   // 2^23 * 2^(n/12), n = 0..12
	uint32_t noteTable;   // note -> octave << 4 | semitone
	uint32_t pitchScale;  // the pitch routine's final 0.32 fixed-point factor
};

struct ADChannel {
	uint8_t state;
	uint8_t level;
	uint16_t sample;
	uint32_t pos;
	uint8_t volRight;
	uint8_t volLeft;
	uint8_t fine;
	uint8_t note;
	uint8_t attack;
	uint8_t decay;
	uint8_t sustain;
	uint8_t release;
};

struct ADFrame {
	const struct ADDriver* driver;
	int32_t pcmFreq; // from the FIFO timer, if known
	uint32_t outA;
	uint32_t outB;
	int32_t count;
	struct ADChannel chans[AD_CHANNELS];
};

// Finds the driver in a ROM image. Returns false if it isn't there.
bool ADFind(const uint8_t* rom, size_t size, struct ADDriver* driver);

// Reads the driver state at the mix routine's entry, given its arguments
// (r1: samples per frame, r2/r3: the FIFO A/B buffers it will write).
void ADFrameRead(struct ADFrame* frame, const struct ADDriver* driver, struct MP2KMemory* mem, int32_t count,
                 uint32_t outA, uint32_t outB);

// The envelope step the driver runs before mixing a channel. Updates chan in
// place; returns false if the channel is (or just went) silent this frame.
bool ADChannelEnvelope(struct ADChannel* chan);

uint32_t ADSampleHeader(const struct ADDriver* driver, struct MP2KMemory* mem, const struct ADChannel* chan);

// Position step per mixed sample, in 1/1024 source samples
uint32_t ADChannelStep(const struct ADDriver* driver, struct MP2KMemory* mem, const struct ADChannel* chan, uint32_t header);

// Runs the driver on a copy of its state: the bytes it writes to FIFO A's
// and FIFO B's buffers. Channel state is updated as the driver would.
void ADMixExact(struct ADFrame* frame, struct MP2KMemory* mem, int8_t* outA, int8_t* outB);

// The same algorithm (nearest sample at the mixing rate) without the
// precision loss: unrounded volumes, no truncation to 8 bits, no wrapping.
// Channels in mutedChannels, or not playing soloWav if it's set, are skipped.
void ADMixFloat(struct ADFrame* frame, struct MP2KMemory* mem, uint32_t mutedChannels, uint32_t soloWav, double* outA,
                double* outB);

// For the high-precision renderer: half 0 is FIFO A's buffer, half 1 FIFO B's
struct HiFiDriver;
extern const struct HiFiDriver ADHiFiDriver;

#endif
