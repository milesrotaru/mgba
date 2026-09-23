/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_MP2K_H
#define GSF2WAV_MP2K_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Nintendo's MP2K ("m4a", "Sappy") sound driver: structure layouts and a
// literal C port of its PCM mixer, SoundMainRAM. Offsets are from the SDK 3.0
// revision of the driver (as used by Mother 3); see pret's m4a_1.s for a
// commented, later revision.

#define MP2K_ID_NUMBER 0x68736D53
#define MP2K_SOUND_INFO_PTR 0x03007FF0
#define MP2K_MAX_CHANNELS 12
#define MP2K_PCM_DMA_BUF_SIZE 0x630
#define MP2K_MAX_SAMPLES_PER_VBLANK 0x630

enum {
	MP2K_SF_START = 0x80,
	MP2K_SF_STOP = 0x40,
	MP2K_SF_LOOP = 0x10,
	MP2K_SF_IEC = 0x04,
	MP2K_SF_ENV = 0x03,
	MP2K_SF_ENV_ATTACK = 0x03,
	MP2K_SF_ENV_DECAY = 0x02,
	MP2K_SF_ENV_SUSTAIN = 0x01,
	MP2K_SF_ON = 0xC7,
	MP2K_TYPE_FIX = 0x08,
};

enum {
	MP2K_INFO_PCM_DMA_COUNTER = 0x04,
	MP2K_INFO_REVERB = 0x05,
	MP2K_INFO_MAX_CHANS = 0x06,
	MP2K_INFO_MASTER_VOLUME = 0x07,
	MP2K_INFO_FREQ = 0x08,
	MP2K_INFO_PCM_DMA_PERIOD = 0x0B,
	MP2K_INFO_MAX_LINES = 0x0C,
	MP2K_INFO_PCM_SAMPLES_PER_VBLANK = 0x10,
	MP2K_INFO_PCM_FREQ = 0x14,
	MP2K_INFO_DIV_FREQ = 0x18,
	MP2K_INFO_CHANS = 0x50,
	MP2K_INFO_PCM_BUFFER = 0x350,
	MP2K_CHANNEL_SIZE = 0x40,
};

struct MP2KChannel {
	uint8_t status;
	uint8_t type;
	uint8_t rightVolume;
	uint8_t leftVolume;
	uint8_t attack;
	uint8_t decay;
	uint8_t sustain;
	uint8_t release;
	uint8_t envelopeVolume;
	uint8_t envelopeVolumeRight;
	uint8_t envelopeVolumeLeft;
	uint8_t echoVolume;
	uint8_t echoLength;
	int32_t count;
	uint32_t fw;
	uint32_t frequency;
	uint32_t wav;
	uint32_t currentPointer;
};

// Driver state at the jump from SoundMain into SoundMainRAM: the sequencer
// has run for this frame, mixing hasn't.
struct MP2KFrame {
	uint32_t info;
	uint32_t segment;
	uint8_t pcmDmaCounter;
	uint8_t pcmDmaPeriod;
	uint8_t reverb;
	uint8_t maxChans;
	uint8_t masterVolume;
	uint8_t maxLines;
	int32_t samplesPerVBlank;
	int32_t pcmFreq;
	uint32_t divFreq;
	struct MP2KChannel chans[MP2K_MAX_CHANNELS];
};

struct MP2KMemory {
	uint8_t (*read8)(struct MP2KMemory*, uint32_t address);
	uint32_t (*read32)(struct MP2KMemory*, uint32_t address);
};

void MP2KFrameRead(struct MP2KFrame* frame, struct MP2KMemory* mem, uint32_t info, uint32_t segment);

// What SoundMainRAM does for one channel's envelope, before mixing it.
// Updates chan in place. Returns false if the channel is (or just went) off.
bool MP2KChannelEnvelope(const struct MP2KFrame* frame, struct MP2KChannel* chan, struct MP2KMemory* mem);

// Runs SoundMainRAM on a copy of the driver state, producing the two halves
// of the segment exactly as the driver writes them (half 0 is at the segment
// address, half 1 PCM_DMA_BUF_SIZE bytes later). Channel state in frame is
// updated as the driver would update it.
void MP2KMixExact(struct MP2KFrame* frame, struct MP2KMemory* mem, int8_t* half0, int8_t* half1);

// Finds SoundMain in a ROM image and returns the address of the "bx r3" that
// enters SoundMainRAM, or 0.
uint32_t MP2KFindHook(const uint8_t* rom, size_t size);

#endif
