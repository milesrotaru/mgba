/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "alphadream.h"

#include <string.h>

// The mix routine's prologue and first few instructions, around the literal
// load of the channel array (offset 14, skipped)
static const uint8_t _mixHead[] = {
	0xF0, 0xB5, 0x57, 0x46, 0x4E, 0x46, 0x45, 0x46, 0xE0, 0xB4, 0x0C, 0xB4, 0x00, 0x25,
};
static const uint8_t _mixBody[] = {
	0x08, 0x27, 0x81, 0x46, 0x8A, 0x46, 0x30, 0x78, 0x00, 0x28, 0x2F, 0xD0, 0x80, 0x28, 0x0C, 0xD0,
};
// Where the rest of the driver sits relative to the mix routine
#define VOICE_OFFSET 0xA8        // per-voice routine
#define VOICE_TABLE_LOAD 0x0C    // its "ldr r1, =sampleTable"
#define PITCH_OFFSET (-0xEC)     // pitch routine
#define PITCH_SCALE_LITERAL 0x34
#define PITCH_FREQ_TABLE 0x38
#define PITCH_NOTE_TABLE 0x6C

static uint32_t _le32(const uint8_t* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Target of a Thumb "ldr rX, [pc, #imm]" at offset off
static bool _thumbLiteral(const uint8_t* rom, size_t size, size_t off, uint32_t* value) {
	if (off + 2 > size) {
		return false;
	}
	uint16_t op = rom[off] | (rom[off + 1] << 8);
	if ((op & 0xF800) != 0x4800) {
		return false;
	}
	size_t lit = ((off + 4) & ~(size_t) 3) + (op & 0xFF) * 4;
	if (lit + 4 > size) {
		return false;
	}
	*value = _le32(&rom[lit]);
	return true;
}

bool ADFind(const uint8_t* rom, size_t size, struct ADDriver* driver) {
	size_t off;
	for (off = 0; off + 0x40 <= size; off += 2) {
		if (memcmp(&rom[off], _mixHead, sizeof(_mixHead)) != 0 ||
		    memcmp(&rom[off + 16], _mixBody, sizeof(_mixBody)) != 0) {
			continue;
		}
		struct ADDriver d;
		memset(&d, 0, sizeof(d));
		d.mix = 0x08000000 + off;
		if (!_thumbLiteral(rom, size, off + 14, &d.channels) ||
		    !_thumbLiteral(rom, size, off + VOICE_OFFSET + VOICE_TABLE_LOAD, &d.sampleTable)) {
			continue;
		}
		if (off < (size_t) -PITCH_OFFSET) {
			continue;
		}
		size_t pitch = off + PITCH_OFFSET;
		d.pitchScale = _le32(&rom[pitch + PITCH_SCALE_LITERAL]);
		d.freqTable = 0x08000000 + pitch + PITCH_FREQ_TABLE;
		d.noteTable = 0x08000000 + pitch + PITCH_NOTE_TABLE;
		// The semitone table must run from 2^23 to 2^24
		if (_le32(&rom[pitch + PITCH_FREQ_TABLE]) != 0x800000 ||
		    _le32(&rom[pitch + PITCH_FREQ_TABLE + 48]) != 0x1000000) {
			continue;
		}
		*driver = d;
		return true;
	}
	return false;
}

void ADFrameRead(struct ADFrame* frame, const struct ADDriver* driver, struct MP2KMemory* mem, int32_t count,
                 uint32_t outA, uint32_t outB) {
	memset(frame, 0, sizeof(*frame));
	frame->driver = driver;
	frame->count = count;
	frame->outA = outA;
	frame->outB = outB;
	int c;
	for (c = 0; c < AD_CHANNELS; ++c) {
		uint32_t base = driver->channels + c * 16;
		struct ADChannel* ch = &frame->chans[c];
		ch->state = mem->read8(mem, base);
		ch->level = mem->read8(mem, base + 1);
		ch->sample = mem->read8(mem, base + 2) | (mem->read8(mem, base + 3) << 8);
		ch->pos = mem->read32(mem, base + 4);
		ch->volRight = mem->read8(mem, base + 8);
		ch->volLeft = mem->read8(mem, base + 9);
		ch->fine = mem->read8(mem, base + 10);
		ch->note = mem->read8(mem, base + 11);
		ch->attack = mem->read8(mem, base + 12);
		ch->decay = mem->read8(mem, base + 13);
		ch->sustain = mem->read8(mem, base + 14);
		ch->release = mem->read8(mem, base + 15);
	}
}

bool ADChannelEnvelope(struct ADChannel* ch) {
	int level;
	switch (ch->state) {
	case 0:
		return false;
	case 0x80:
		ch->state = 0x81;
		ch->level = ch->attack;
		return true;
	case 0x81:
		level = ch->level + ch->attack;
		if (level < 0xFF) {
			ch->level = level;
		} else {
			ch->state = 0x82;
			ch->level = 0xFF;
		}
		return true;
	case 0x82:
		// Whether or not the level has decayed past the decay step, it goes
		// straight to the sustain level. (The branch for the second case is
		// zeroed in the rip, as the ripper never saw it run.)
		ch->state = 0x83;
		ch->level = ch->sustain;
		return true;
	case 0x83:
		return true;
	default:
		if (ch->level > ch->release) {
			ch->level -= ch->release;
			return true;
		}
		ch->state = 0;
		return false;
	}
}

uint32_t ADSampleHeader(const struct ADDriver* driver, struct MP2KMemory* mem, const struct ADChannel* ch) {
	uint32_t index = (ch->sample & 0x7FFF) * 4;
	return driver->sampleTable + mem->read32(mem, driver->sampleTable + index);
}

uint32_t ADChannelStep(const struct ADDriver* driver, struct MP2KMemory* mem, const struct ADChannel* ch, uint32_t header) {
	uint32_t base = mem->read32(mem, header + 4);
	uint32_t note = ch->note;
	uint32_t fine = ch->fine;
	if (ch->sample & 0x8000) {
		note = 0x3C;
		fine = 0;
	}
	uint32_t n = mem->read8(mem, driver->noteTable + note);
	uint32_t entry = driver->freqTable + (n & 0xF) * 4;
	uint32_t lo = mem->read32(mem, entry);
	uint32_t hi = mem->read32(mem, entry + 4);
	uint32_t f = lo + (((hi - lo) * fine) >> 8);
	uint32_t shift = n >> 4;
	f = shift >= 32 ? 0 : f >> shift;
	uint32_t p = ((uint64_t) base * f) >> 32;
	return p + (uint32_t) (((uint64_t) p * driver->pitchScale) >> 32);
}

struct ADVoice {
	uint32_t data;
	uint32_t end;
	uint32_t loopStart;
	bool loop;
	uint32_t step;
};

static void _voice(const struct ADDriver* driver, struct MP2KMemory* mem, const struct ADChannel* ch, struct ADVoice* v) {
	uint32_t header = ADSampleHeader(driver, mem, ch);
	v->data = header + 0x10;
	v->end = mem->read32(mem, header + 12);
	v->loopStart = mem->read32(mem, header + 8);
	v->loop = mem->read8(mem, header + 3) & 0x40;
	v->step = ADChannelStep(driver, mem, ch, header);
}

void ADMixExact(struct ADFrame* frame, struct MP2KMemory* mem, int8_t* outA, int8_t* outB) {
	int32_t count = frame->count;
	uint32_t acc[AD_MAX_SAMPLES_PER_FRAME];
	memset(acc, 0, count * sizeof(acc[0]));
	uint32_t bias = 0;
	int c;
	for (c = 0; c < AD_CHANNELS; ++c) {
		struct ADChannel* ch = &frame->chans[c];
		if (!ADChannelEnvelope(ch)) {
			continue;
		}
		uint32_t vol = (((uint32_t) ch->volLeft << 16 | ch->volRight) * ch->level) >> 8;
		vol &= ~0xFF00u;
		bias += vol << 7;

		struct ADVoice v;
		_voice(frame->driver, mem, ch, &v);
		uint32_t pos = ch->pos;
		int32_t remaining = count;
		int32_t i = 0;
		while (true) {
			// Samples before the end, rounded up
			uint32_t left = (v.end << 10) - pos + v.step - 1;
			uint32_t n = remaining;
			if ((uint32_t) (v.step * remaining) >= left && v.step) {
				n = left / v.step;
			}
			remaining -= n;
			uint32_t k;
			for (k = 0; k < n; ++k, ++i) {
				acc[i] += mem->read8(mem, v.data + (pos >> 10)) * vol;
				pos += v.step;
			}
			if ((int32_t) pos < (int32_t) (v.end << 10)) {
				break;
			}
			if (v.loop) {
				pos += (v.loopStart - v.end) << 10;
				if (remaining) {
					continue;
				}
				break;
			}
			// Ended: the rest of the frame gets the bias it was charged for
			ch->state = 0;
			for (; remaining > 0; --remaining, ++i) {
				acc[i] += vol * 0x80;
			}
			break;
		}
		ch->pos = pos;
	}
	int32_t j;
	for (j = 0; j < count; ++j) {
		uint32_t w = acc[j] - bias;
		outA[j] = (int8_t) (w >> 24);
		outB[j] = (int8_t) (w >> 8);
	}
}

void ADMixFloat(struct ADFrame* frame, struct MP2KMemory* mem, uint32_t mutedChannels, uint32_t soloWav, double* outA,
                double* outB) {
	int32_t count = frame->count;
	memset(outA, 0, count * sizeof(*outA));
	memset(outB, 0, count * sizeof(*outB));
	int c;
	for (c = 0; c < AD_CHANNELS; ++c) {
		struct ADChannel* ch = &frame->chans[c];
		if (!ADChannelEnvelope(ch)) {
			continue;
		}
		struct ADVoice v;
		_voice(frame->driver, mem, ch, &v);
		bool keep = !(mutedChannels & (1u << c)) && (!soloWav || ADSampleHeader(frame->driver, mem, ch) == soloWav);
		double gainA = ch->volLeft * ch->level / 65536.0;
		double gainB = ch->volRight * ch->level / 65536.0;
		uint32_t pos = ch->pos;
		int32_t i;
		for (i = 0; i < count; ++i) {
			if (pos >= (v.end << 10)) {
				if (!v.loop) {
					ch->state = 0;
					break;
				}
				while (pos >= (v.end << 10)) {
					pos += (v.loopStart - v.end) << 10;
				}
			}
			if (keep) {
				double s = (int) mem->read8(mem, v.data + (pos >> 10)) - 128;
				outA[i] += s * gainA;
				outB[i] += s * gainB;
			}
			pos += v.step;
		}
		ch->pos = pos;
	}
}
