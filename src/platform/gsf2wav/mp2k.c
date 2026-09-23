/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "mp2k.h"

#include <string.h>

static uint16_t _rom16(const uint8_t* rom, size_t off) {
	return rom[off] | (rom[off + 1] << 8);
}

static uint32_t _rom32(const uint8_t* rom, size_t off) {
	return rom[off] | (rom[off + 1] << 8) | (rom[off + 2] << 16) | ((uint32_t) rom[off + 3] << 24);
}

uint32_t MP2KFindHook(const uint8_t* rom, size_t size) {
	size_t off;
	for (off = 0; off + 0x140 < size; off += 2) {
		// ldr r0,[pc,#a]; ldr r0,[r0]; ldr r2,[pc,#b]; ldr r3,[r0]; cmp r2,r3
		if ((_rom16(rom, off) >> 8) != 0x48 || _rom16(rom, off + 2) != 0x6800 || (_rom16(rom, off + 4) >> 8) != 0x4A ||
		    _rom16(rom, off + 6) != 0x6803 || _rom16(rom, off + 8) != 0x429A) {
			continue;
		}
		size_t lit0 = ((off + 4) & ~3) + (_rom16(rom, off) & 0xFF) * 4;
		size_t lit2 = ((off + 8) & ~3) + (_rom16(rom, off + 4) & 0xFF) * 4;
		if (lit0 + 4 > size || lit2 + 4 > size || _rom32(rom, lit0) != MP2K_SOUND_INFO_PTR || _rom32(rom, lit2) != MP2K_ID_NUMBER) {
			continue;
		}
		// ldr r6,[pc,#x]; ldr r3,[pc,#y]; bx r3
		size_t t;
		for (t = off; t < off + 0x100; t += 2) {
			if ((_rom16(rom, t) >> 8) == 0x4E && (_rom16(rom, t + 2) >> 8) == 0x4B && _rom16(rom, t + 4) == 0x4718) {
				return 0x08000000 + t + 4;
			}
		}
	}
	return 0;
}

void MP2KFrameRead(struct MP2KFrame* frame, struct MP2KMemory* mem, uint32_t info, uint32_t segment) {
	memset(frame, 0, sizeof(*frame));
	frame->info = info;
	frame->segment = segment;
	frame->pcmDmaCounter = mem->read8(mem, info + MP2K_INFO_PCM_DMA_COUNTER);
	frame->pcmDmaPeriod = mem->read8(mem, info + MP2K_INFO_PCM_DMA_PERIOD);
	frame->reverb = mem->read8(mem, info + MP2K_INFO_REVERB);
	frame->maxChans = mem->read8(mem, info + MP2K_INFO_MAX_CHANS);
	frame->masterVolume = mem->read8(mem, info + MP2K_INFO_MASTER_VOLUME);
	frame->maxLines = mem->read8(mem, info + MP2K_INFO_MAX_LINES);
	frame->samplesPerVBlank = mem->read32(mem, info + MP2K_INFO_PCM_SAMPLES_PER_VBLANK);
	frame->pcmFreq = mem->read32(mem, info + MP2K_INFO_PCM_FREQ);
	frame->divFreq = mem->read32(mem, info + MP2K_INFO_DIV_FREQ);
	if (frame->maxChans > MP2K_MAX_CHANNELS) {
		frame->maxChans = MP2K_MAX_CHANNELS;
	}
	int i;
	for (i = 0; i < frame->maxChans; ++i) {
		uint32_t base = info + MP2K_INFO_CHANS + i * MP2K_CHANNEL_SIZE;
		struct MP2KChannel* ch = &frame->chans[i];
		ch->status = mem->read8(mem, base + 0x00);
		ch->type = mem->read8(mem, base + 0x01);
		ch->rightVolume = mem->read8(mem, base + 0x02);
		ch->leftVolume = mem->read8(mem, base + 0x03);
		ch->attack = mem->read8(mem, base + 0x04);
		ch->decay = mem->read8(mem, base + 0x05);
		ch->sustain = mem->read8(mem, base + 0x06);
		ch->release = mem->read8(mem, base + 0x07);
		ch->envelopeVolume = mem->read8(mem, base + 0x09);
		ch->envelopeVolumeRight = mem->read8(mem, base + 0x0A);
		ch->envelopeVolumeLeft = mem->read8(mem, base + 0x0B);
		ch->echoVolume = mem->read8(mem, base + 0x0C);
		ch->echoLength = mem->read8(mem, base + 0x0D);
		ch->count = mem->read32(mem, base + 0x18);
		ch->fw = mem->read32(mem, base + 0x1C);
		ch->frequency = mem->read32(mem, base + 0x20);
		ch->wav = mem->read32(mem, base + 0x24);
		ch->currentPointer = mem->read32(mem, base + 0x28);
	}
}

bool MP2KChannelEnvelope(const struct MP2KFrame* frame, struct MP2KChannel* ch, struct MP2KMemory* mem) {
	uint32_t st = ch->status;
	if (!(st & MP2K_SF_ON)) {
		return false;
	}
	uint32_t wav = ch->wav;
	uint32_t env;
	if (st & MP2K_SF_START) {
		if (st & MP2K_SF_STOP) {
			ch->status = 0;
			return false;
		}
		st = MP2K_SF_ENV_ATTACK;
		ch->currentPointer = wav + 0x10;
		ch->count = mem->read32(mem, wav + 0xC);
		env = 0;
		ch->fw = 0;
		if (mem->read8(mem, wav + 3) & 0xC0) {
			st |= MP2K_SF_LOOP;
		}
		ch->status = st;
		goto attack;
	}
	env = ch->envelopeVolume;
	if (st & MP2K_SF_IEC) {
		uint8_t length = ch->echoLength;
		ch->echoLength = length - 1;
		if (length > 1) {
			goto store;
		}
		ch->status = 0;
		return false;
	}
	if (st & MP2K_SF_STOP) {
		env = (env * ch->release) >> 8;
		if (env > ch->echoVolume) {
			goto store;
		}
		goto echo;
	}
	if ((st & MP2K_SF_ENV) == MP2K_SF_ENV_DECAY) {
		env = (env * ch->decay) >> 8;
		if (env > ch->sustain) {
			goto store;
		}
		env = ch->sustain;
		if (!env) {
			goto echo;
		}
		ch->status = --st;
		goto store;
	}
	if ((st & MP2K_SF_ENV) != MP2K_SF_ENV_ATTACK) {
		goto store;
	}
attack:
	env += ch->attack;
	if (env >= 0xFF) {
		env = 0xFF;
		ch->status = --st;
	}
	goto store;
echo:
	env = ch->echoVolume;
	if (!env) {
		ch->status = 0;
		return false;
	}
	ch->status = st | MP2K_SF_IEC;
store:
	ch->envelopeVolume = env;
	env = ((frame->masterVolume + 1) * env) >> 4;
	ch->envelopeVolumeRight = (ch->rightVolume * env) >> 8;
	ch->envelopeVolumeLeft = (ch->leftVolume * env) >> 8;
	return true;
}

// The driver accumulates into bytes: each voice adds floor(sample * vol / 256),
// wrapping modulo 256 with no clipping.
static inline void _accumulate(int8_t* lane, int32_t sample, uint32_t volume) {
	*lane = (int8_t) (*lane + (int8_t) ((int32_t) (sample * (int32_t) volume) >> 8));
}

static inline int32_t _s8(struct MP2KMemory* mem, uint32_t address) {
	return (int8_t) mem->read8(mem, address);
}

void MP2KMixExact(struct MP2KFrame* frame, struct MP2KMemory* mem, int8_t* half0, int8_t* half1) {
	int32_t spv = frame->samplesPerVBlank;
	int32_t j;

	if (frame->reverb) {
		// Mono feedback from the two oldest segments in the ring
		uint32_t cur = frame->segment;
		uint32_t next = frame->pcmDmaCounter == 2 ? frame->info + MP2K_INFO_PCM_BUFFER : cur + spv;
		for (j = 0; j < spv; ++j) {
			int32_t v = _s8(mem, cur + j + MP2K_PCM_DMA_BUF_SIZE) + _s8(mem, cur + j) +
			            _s8(mem, next + j + MP2K_PCM_DMA_BUF_SIZE) + _s8(mem, next + j);
			v = (v * frame->reverb) >> 9;
			if (v & 0x80) {
				++v;
			}
			half0[j] = (int8_t) v;
			half1[j] = (int8_t) v;
		}
	} else {
		memset(half0, 0, spv);
		memset(half1, 0, spv);
	}

	int c;
	for (c = 0; c < frame->maxChans; ++c) {
		struct MP2KChannel* ch = &frame->chans[c];
		if (!MP2KChannelEnvelope(frame, ch, mem)) {
			continue;
		}
		uint32_t wav = ch->wav;
		uint32_t loopStart = 0;
		int32_t loopLength = 0;
		if (ch->status & MP2K_SF_LOOP) {
			uint32_t loopOffset = mem->read32(mem, wav + 0x8);
			loopStart = wav + 0x10 + loopOffset;
			loopLength = mem->read32(mem, wav + 0xC) - loopOffset;
		}
		uint32_t volR = ch->envelopeVolumeRight;
		uint32_t volL = ch->envelopeVolumeLeft;
		int32_t count = ch->count;
		uint32_t cur = ch->currentPointer;

		if (ch->type & MP2K_TYPE_FIX) {
			bool stopped = false;
			for (j = 0; j < spv; ++j) {
				int32_t s = _s8(mem, cur++);
				_accumulate(&half0[j], s, volR);
				_accumulate(&half1[j], s, volL);
				if (--count == 0) {
					if (loopLength) {
						cur = loopStart;
						count = loopLength;
					} else {
						ch->status = 0;
						stopped = true;
						break;
					}
				}
			}
			if (!stopped) {
				ch->count = count;
				ch->currentPointer = cur;
			}
			continue;
		}

		uint32_t step = ch->frequency * frame->divFreq;
		uint32_t fw = ch->fw;
		int32_t s0 = _s8(mem, cur);
		++cur;
		int32_t d = _s8(mem, cur) - s0;
		bool stopped = false;
		for (j = 0; j < spv; ++j) {
			int32_t s = s0 + ((int32_t) (fw * (uint32_t) d) >> 23);
			_accumulate(&half0[j], s, volR);
			_accumulate(&half1[j], s, volL);
			fw += step;
			uint32_t advance = fw >> 23;
			if (!advance) {
				continue;
			}
			fw &= ~0x3F800000u;
			count -= advance;
			if (count <= 0) {
				if (!loopLength) {
					ch->status = 0;
					stopped = true;
					break;
				}
				// Wrap the overshoot into the loop
				int32_t offset = -count;
				cur = loopStart;
				while (true) {
					count += loopLength;
					if (count > 0) {
						break;
					}
					offset -= loopLength;
				}
				cur += offset;
				s0 = _s8(mem, cur);
			} else if (advance == 1) {
				s0 += d;
			} else {
				cur += advance - 1;
				s0 = _s8(mem, cur);
			}
			++cur;
			d = _s8(mem, cur) - s0;
		}
		if (!stopped) {
			ch->fw = fw;
			ch->count = count;
			ch->currentPointer = cur - 1;
		}
	}
}
