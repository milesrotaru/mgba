/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "mp2k_hifi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Voice resampling kernel: 32 zero crossings, ~100 dB stopband
#define VOICE_ZERO_CROSSINGS 32
#define VOICE_KAISER_BETA 10.0
#define VOICE_TABLE_RES 4096
// Cutoffs as fractions of the source and output sample rates, placed so the
// transition band finishes before each Nyquist
#define VOICE_SOURCE_CUTOFF 0.47
#define VOICE_OUT_CUTOFF 0.46
// Upper bound on source samples per output sample honored by the kernel.
// Faster than this, the voice is inaudible anyway.
#define VOICE_MAX_RATE 64.0

// A new voice's sinc kernel reaches back before its first sample; render this
// much before the note starts so that pre-ringing isn't cut off, and hold
// back output finalization by the same amount
#define VOICE_PREROLL_SECONDS 0.006

#define RING_SIZE (1 << 18)
#define LINEAR_HISTORY_FRAMES 16

#define FIFO_HISTORY 16384
#define LOCK_PATTERN 32
#define LOCK_GIVE_UP 900

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

void MP2KHiFiInit(struct MP2KHiFi* hifi, struct BLMixer* out, struct MP2KMemory* mem, const uint8_t* rom, size_t romSize,
                  double clockRate, double rampSeconds) {
	memset(hifi, 0, sizeof(*hifi));
	hifi->out = out;
	hifi->mem = mem;
	hifi->rom = rom;
	hifi->romSize = romSize;
	hifi->clockRate = clockRate;
	hifi->ramp = rampSeconds * clockRate;
	hifi->zeroCrossings = VOICE_ZERO_CROSSINGS;
	hifi->tableRes = VOICE_TABLE_RES;
	size_t entries = (size_t) VOICE_ZERO_CROSSINGS * VOICE_TABLE_RES + 2;
	hifi->table = malloc(entries * sizeof(double));
	double i0beta = _besselI0(VOICE_KAISER_BETA);
	size_t i;
	for (i = 0; i < entries; ++i) {
		double u = (double) i / VOICE_TABLE_RES;
		double sinc = u == 0 ? 1 : sin(M_PI * u) / (M_PI * u);
		double r = u / VOICE_ZERO_CROSSINGS;
		double window = r >= 1 ? 0 : _besselI0(VOICE_KAISER_BETA * sqrt(1 - r * r)) / i0beta;
		hifi->table[i] = sinc * window;
	}
	size_t maxTaps = (size_t) ceil(VOICE_ZERO_CROSSINGS * VOICE_MAX_RATE / VOICE_OUT_CUTOFF) + 4;
	hifi->taps = malloc(maxTaps * sizeof(double));
	hifi->fifoCapacity = FIFO_HISTORY;
	int f;
	for (f = 0; f < 2; ++f) {
		hifi->fifoHistory[f] = malloc(FIFO_HISTORY);
		hifi->fifoTimes[f] = malloc(FIFO_HISTORY * sizeof(uint64_t));
	}
	hifi->pending = malloc(MP2K_HIFI_MAX_PENDING * sizeof(*hifi->pending));
	hifi->pendingExact = malloc(MP2K_HIFI_MAX_PENDING * sizeof(*hifi->pendingExact));
	hifi->horizon = INFINITY;
	hifi->ringMask = RING_SIZE - 1;
	hifi->half[0] = calloc(RING_SIZE, sizeof(double));
	hifi->half[1] = calloc(RING_SIZE, sizeof(double));
	hifi->mono = calloc(RING_SIZE, sizeof(double));
	hifi->reverbGain = calloc(RING_SIZE, sizeof(double));
	hifi->flushed = -1;
	hifi->linearHistory = calloc(LINEAR_HISTORY_FRAMES * MP2K_MAX_SAMPLES_PER_VBLANK, sizeof(double));
}

void MP2KHiFiDeinit(struct MP2KHiFi* hifi) {
	free(hifi->table);
	free(hifi->taps);
	int f;
	for (f = 0; f < 2; ++f) {
		free(hifi->fifoHistory[f]);
		free(hifi->fifoTimes[f]);
	}
	free(hifi->pending);
	free(hifi->pendingExact);
	free(hifi->half[0]);
	free(hifi->half[1]);
	free(hifi->mono);
	free(hifi->reverbGain);
	free(hifi->linearHistory);
}

static inline double _kernel(const struct MP2KHiFi* hifi, double u) {
	u = fabs(u) * VOICE_TABLE_RES;
	size_t i = (size_t) u;
	if (i >= (size_t) VOICE_ZERO_CROSSINGS * VOICE_TABLE_RES) {
		return 0;
	}
	double frac = u - i;
	return hifi->table[i] + (hifi->table[i + 1] - hifi->table[i]) * frac;
}

// Sample k of the signal the voice plays: the data once, then the loop forever
static inline int _sourceSample(const struct MP2KHiFi* hifi, const struct MP2KHiFiVoice* v, int64_t k) {
	if (k < 0) {
		return 0;
	}
	if (k >= v->size) {
		if (!v->loopLength) {
			return 0;
		}
		k = v->loopStart + (k - v->size) % v->loopLength;
	}
	if (v->data) {
		return v->data[k];
	}
	return (int8_t) hifi->mem->read8(hifi->mem, v->wav + 0x10 + (uint32_t) k);
}

static bool _voiceStart(struct MP2KHiFi* hifi, struct MP2KHiFiVoice* v, const struct MP2KChannel* ch) {
	struct MP2KMemory* mem = hifi->mem;
	uint32_t wav = ch->wav;
	memset(v, 0, sizeof(*v));
	v->active = true;
	v->wav = wav;
	v->size = mem->read32(mem, wav + 0xC);
	if (ch->status & MP2K_SF_LOOP) {
		int64_t loopOffset = mem->read32(mem, wav + 0x8);
		v->loopStart = loopOffset;
		v->loopLength = v->size - loopOffset;
		if (v->loopLength <= 0 || loopOffset < 0) {
			v->loopLength = 0;
		}
	}
	v->fixed = ch->type & MP2K_TYPE_FIX;
	if ((wav >> 24) == 0x08 || (wav >> 24) == 0x09) {
		size_t off = (wav & 0x01FFFFFF) + 0x10;
		if (off + (size_t) v->size + 1 <= hifi->romSize) {
			v->data = (const int8_t*) &hifi->rom[off];
		}
	}
	return v->size > 0;
}

static double _channelPosition(const struct MP2KChannel* ch) {
	double pos = (double) (ch->currentPointer - (ch->wav + 0x10));
	if (!(ch->type & MP2K_TYPE_FIX)) {
		pos += ch->fw / 8388608.0;
	}
	return pos;
}

static double _wrap(const struct MP2KHiFiVoice* v, double u) {
	if (u < v->size || !v->loopLength) {
		return u;
	}
	return v->loopStart + fmod(u - v->size, (double) v->loopLength);
}

static void _ghost(struct MP2KHiFi* hifi, const struct MP2KHiFiVoice* v, double stopTime) {
	int i;
	for (i = 0; i < MP2K_HIFI_MAX_GHOSTS; ++i) {
		if (!hifi->ghosts[i].active) {
			hifi->ghosts[i] = *v;
			hifi->ghosts[i].stopTime = stopTime;
			return;
		}
	}
	++hifi->droppedGhosts;
}

struct FrameTiming {
	double start;  // time of mixer sample 0's center
	double period; // cycles per mixer sample
	double cyclesPerOut;
	int64_t first;
	int64_t end;
};

// Renders one voice over the frame. mode 0: gain ramps from prevGain to gain
// starting at the frame start; mode 1 (ghost): ramps from gain to 0 starting
// at stopTime. Positions are u + tau * step, tau in mixer samples.
static void _renderVoice(struct MP2KHiFi* hifi, struct MP2KHiFiVoice* v, const struct FrameTiming* ft, bool ghost) {
	if (hifi->mutedChannels & (1u << v->channel)) {
		if (ghost) {
			v->active = false;
		}
		return;
	}
	double rate = v->step * ft->cyclesPerOut / ft->period; // source samples per output sample
	if (rate > VOICE_MAX_RATE) {
		rate = VOICE_MAX_RATE;
	}
	double cutoff = VOICE_SOURCE_CUTOFF;
	if (rate > 0 && VOICE_OUT_CUTOFF / rate < cutoff) {
		cutoff = VOICE_OUT_CUTOFF / rate;
	}
	if (hifi->bandwidth > 0 && v->step > 0) {
		// Cutoff in source samples: Hz over the rate the source is played at
		double sourceRate = v->step * hifi->clockRate / ft->period;
		double cap = hifi->bandwidth / sourceRate;
		if (cap < cutoff) {
			cutoff = cap;
		}
	}
	double scale = 2 * cutoff;
	double halfWidth = VOICE_ZERO_CROSSINGS / scale;
	int64_t i;
	for (i = ft->first; i < ft->end; ++i) {
		double t = i * ft->cyclesPerOut;
		double g0, g1;
		if (ghost) {
			double x = (t - v->stopTime) / hifi->ramp;
			if (x >= 1) {
				v->active = false;
				return;
			}
			double k = x <= 0 ? 1 : 1 - x;
			g0 = v->gain[0] * k;
			g1 = v->gain[1] * k;
		} else {
			double x = hifi->ramp > 0 ? (t - (ft->start - ft->period * 0.5)) / hifi->ramp : 1;
			if (x >= 1) {
				g0 = v->gain[0];
				g1 = v->gain[1];
			} else {
				if (x < 0) {
					x = 0;
				}
				g0 = v->prevGain[0] + (v->gain[0] - v->prevGain[0]) * x;
				g1 = v->prevGain[1] + (v->gain[1] - v->prevGain[1]) * x;
			}
		}
		if (g0 == 0 && g1 == 0) {
			continue;
		}
		double tau = (t - ft->start) / ft->period;
		double u = v->u + tau * v->step;
		int64_t k0 = (int64_t) ceil(u - halfWidth);
		int64_t k1 = (int64_t) floor(u + halfWidth);
		double acc = 0;
		int64_t k;
		if (v->data && k0 >= 0 && k1 < v->size) {
			const int8_t* d = v->data;
			for (k = k0; k <= k1; ++k) {
				acc += d[k] * _kernel(hifi, scale * (u - k));
			}
		} else {
			for (k = k0; k <= k1; ++k) {
				int s = _sourceSample(hifi, v, k);
				if (s) {
					acc += s * _kernel(hifi, scale * (u - k));
				}
			}
		}
		acc *= scale;
		if (i < hifi->flushed || i >= hifi->flushed + (int64_t) hifi->ringMask) {
			++hifi->lostSamples;
			continue;
		}
		hifi->half[0][i & hifi->ringMask] += acc * g0;
		hifi->half[1][i & hifi->ringMask] += acc * g1;
	}
}

// The mono history at a fractional index, by windowed-sinc interpolation at
// the output Nyquist (the history is already bandlimited below it)
static double _monoAt(const struct MP2KHiFi* hifi, double x) {
	int64_t k0 = (int64_t) ceil(x - VOICE_ZERO_CROSSINGS);
	int64_t k1 = (int64_t) floor(x + VOICE_ZERO_CROSSINGS);
	double acc = 0;
	int64_t k;
	for (k = k0; k <= k1; ++k) {
		acc += hifi->mono[k & hifi->ringMask] * _kernel(hifi, x - k);
	}
	return acc;
}

// Finalizes the half buffers up to (not including) index `end`: adds the
// driver's reverb, routes the halves to the FIFOs' outputs and hands them on
static void _flush(struct MP2KHiFi* hifi, int64_t end) {
	int64_t i;
	for (i = hifi->flushed; i < end; ++i) {
		size_t idx = i & hifi->ringMask;
		double w = 0;
		double g = hifi->reverbGain[idx];
		if (g) {
			w = g * (_monoAt(hifi, i - hifi->reverbDelay[0]) + _monoAt(hifi, i - hifi->reverbDelay[1]));
		}
		double y0 = hifi->half[0][idx] + w;
		double y1 = hifi->half[1][idx] + w;
		hifi->mono[idx] = y0 + y1;
		double l = y0 * hifi->route[0][0] + y1 * hifi->route[1][0];
		double r = y0 * hifi->route[0][1] + y1 * hifi->route[1][1];
		if ((l || r) && !BLMixerAdd(hifi->out, i, l, r)) {
			--hifi->out->lateEvents;
			++hifi->lostSamples;
		}
		hifi->half[0][idx] = 0;
		hifi->half[1][idx] = 0;
		hifi->reverbGain[idx] = 0;
	}
	hifi->flushed = end;
}

static void _renderFrameLinear(struct MP2KHiFi* hifi, uint64_t n, const struct MP2KFrame* frameIn, const double route[2][2]) {
	struct MP2KFrame frame = *frameIn;
	int32_t spv = frame.samplesPerVBlank;
	int c;
	for (c = 0; c < MP2K_MAX_CHANNELS; ++c) {
		if (hifi->mutedChannels & (1u << c)) {
			frame.chans[c].status = 0;
		}
	}
	double half0[MP2K_MAX_SAMPLES_PER_VBLANK];
	double half1[MP2K_MAX_SAMPLES_PER_VBLANK];
	MP2KMixFloat(&frame, hifi->mem, half0, half1);
	double* mono = &hifi->linearHistory[(n % LINEAR_HISTORY_FRAMES) * MP2K_MAX_SAMPLES_PER_VBLANK];
	int period = frame.pcmDmaPeriod;
	bool reverb = frame.reverb && period > 1 && period < LINEAR_HISTORY_FRAMES && n >= (uint64_t) period;
	const double* old0 = reverb ? &hifi->linearHistory[((n - period) % LINEAR_HISTORY_FRAMES) * MP2K_MAX_SAMPLES_PER_VBLANK] : NULL;
	const double* old1 = reverb ? &hifi->linearHistory[((n - period + 1) % LINEAR_HISTORY_FRAMES) * MP2K_MAX_SAMPLES_PER_VBLANK] : NULL;
	double g = frame.reverb / 512.0;
	double latch = hifi->latchPeriod;
	double start = hifi->t0 + (double) n * spv * latch + latch * 0.5;
	int32_t j;
	for (j = 0; j < spv; ++j) {
		if (reverb) {
			double w = g * (old0[j] + old1[j]);
			half0[j] += w;
			half1[j] += w;
		}
		mono[j] = half0[j] + half1[j];
		double l = half0[j] * route[0][0] + half1[j] * route[1][0];
		double r = half0[j] * route[0][1] + half1[j] * route[1][1];
		BLMixerPoint(hifi->out, start + j * latch, latch, l, r);
	}
	hifi->horizon = start + spv * latch - latch * 0.5;
}

static void _renderFrame(struct MP2KHiFi* hifi, uint64_t n, const struct MP2KFrame* frameIn, const double fifoGain[2][2]) {
	struct MP2KFrame frame = *frameIn;
	int32_t spv = frame.samplesPerVBlank;
	struct FrameTiming ft;
	ft.period = hifi->latchPeriod;
	ft.cyclesPerOut = hifi->out->cyclesPerSample;
	ft.start = hifi->t0 + (double) n * spv * ft.period + ft.period * 0.5;
	double end = ft.start + spv * ft.period;
	ft.first = (int64_t) ceil((ft.start - ft.period * 0.5) / ft.cyclesPerOut);
	ft.end = (int64_t) ceil((end - ft.period * 0.5) / ft.cyclesPerOut);

	double route[2][2] = { { 0, 0 }, { 0, 0 } };
	int f;
	for (f = 0; f < 2; ++f) {
		int h = hifi->halfForFifo[f];
		if (h >= 0) {
			route[h][0] += fifoGain[f][0];
			route[h][1] += fifoGain[f][1];
		}
	}
	if (hifi->mode == MP2K_HIFI_LINEAR) {
		_renderFrameLinear(hifi, n, frameIn, route);
		return;
	}
	memcpy(hifi->route, route, sizeof(route));
	if (hifi->flushed < 0) {
		hifi->flushed = (int64_t) floor((ft.start - ft.period * 0.5 - VOICE_PREROLL_SECONDS * hifi->clockRate) / ft.cyclesPerOut);
	}
	double framePeriodOut = spv * ft.period / ft.cyclesPerOut;
	hifi->reverbDelay[0] = frame.pcmDmaPeriod * framePeriodOut;
	hifi->reverbDelay[1] = (frame.pcmDmaPeriod - 1) * framePeriodOut;
	if (frame.reverb && frame.pcmDmaPeriod > 1) {
		int64_t i;
		for (i = ft.first < hifi->flushed ? hifi->flushed : ft.first; i < ft.end; ++i) {
			hifi->reverbGain[i & hifi->ringMask] = frame.reverb / 512.0;
		}
	}

	int c;
	for (c = 0; c < MP2K_MAX_CHANNELS; ++c) {
		struct MP2KHiFiVoice* v = &hifi->voices[c];
		if (c >= frame.maxChans) {
			if (v->active) {
				_ghost(hifi, v, ft.start - ft.period * 0.5);
				v->active = false;
			}
			continue;
		}
		struct MP2KChannel* ch = &frame.chans[c];
		bool started = ch->status & MP2K_SF_START;
		bool on = MP2KChannelEnvelope(&frame, ch, hifi->mem);
		if (!on) {
			if (v->active) {
				_ghost(hifi, v, ft.start - ft.period * 0.5);
				v->active = false;
			}
			continue;
		}
		double e = (frame.masterVolume + 1) * ch->envelopeVolume / 16.0;
		double gain[2] = { ch->rightVolume * e / 65536.0, ch->leftVolume * e / 65536.0 };
		double pos = _channelPosition(ch);
		if (started || !v->active || v->wav != ch->wav) {
			if (v->active) {
				_ghost(hifi, v, ft.start - ft.period * 0.5);
			}
			if (!_voiceStart(hifi, v, ch)) {
				v->active = false;
				continue;
			}
			v->channel = c;
			v->u = pos;
			v->prevGain[0] = gain[0];
			v->prevGain[1] = gain[1];
			v->gain[0] = gain[0];
			v->gain[1] = gain[1];
			v->step = v->fixed ? 1.0 : (uint32_t) (ch->frequency * frame.divFreq) / 8388608.0;
			struct FrameTiming pre = ft;
			pre.end = ft.first;
			pre.first = (int64_t) ceil((ft.start - ft.period * 0.5 - VOICE_PREROLL_SECONDS * hifi->clockRate) / ft.cyclesPerOut);
			_renderVoice(hifi, v, &pre, false);
		} else {
			if (fabs(_wrap(v, v->u) - pos) > 1e-3) {
				++hifi->resyncs;
				v->u = pos;
			}
			v->prevGain[0] = v->gain[0];
			v->prevGain[1] = v->gain[1];
		}
		v->gain[0] = gain[0];
		v->gain[1] = gain[1];
		v->step = v->fixed ? 1.0 : (uint32_t) (ch->frequency * frame.divFreq) / 8388608.0;
		_renderVoice(hifi, v, &ft, false);
		v->u += spv * v->step;
	}

	int g;
	for (g = 0; g < MP2K_HIFI_MAX_GHOSTS; ++g) {
		struct MP2KHiFiVoice* v = &hifi->ghosts[g];
		if (!v->active) {
			continue;
		}
		_renderVoice(hifi, v, &ft, true);
		v->u += spv * v->step;
	}

	int64_t ready = (int64_t) floor((end - ft.period * 0.5 - VOICE_PREROLL_SECONDS * hifi->clockRate) / ft.cyclesPerOut);
	if (ready > hifi->flushed) {
		_flush(hifi, ready);
	}
	hifi->horizon = hifi->flushed * ft.cyclesPerOut;
}

static bool _audible(const int8_t* pattern) {
	int i;
	int distinct = 0;
	for (i = 1; i < LOCK_PATTERN; ++i) {
		distinct += pattern[i] != pattern[i - 1];
	}
	return distinct >= LOCK_PATTERN / 2;
}

static int64_t _findPattern(const struct MP2KHiFi* hifi, int fifo, const int8_t* pattern) {
	const int8_t* h = hifi->fifoHistory[fifo];
	size_t n = hifi->fifoCount[fifo];
	size_t i;
	for (i = 0; i + LOCK_PATTERN <= n; ++i) {
		if (h[i] == pattern[0] && memcmp(&h[i], pattern, LOCK_PATTERN) == 0) {
			return i;
		}
	}
	return -1;
}

static void _tryLock(struct MP2KHiFi* hifi) {
	size_t p;
	for (p = 0; p + 1 < hifi->pendingCount; ++p) {
		int half;
		for (half = 0; half < 2; ++half) {
			if (!_audible(hifi->pendingExact[p][half]) || !_audible(hifi->pendingExact[p + 1][half])) {
				continue;
			}
			int f;
			for (f = 0; f < 2; ++f) {
				int64_t at = _findPattern(hifi, f, hifi->pendingExact[p][half]);
				if (at < 0) {
					continue;
				}
				// Confirm with the next frame, one frame's worth of samples later
				int64_t next = at + hifi->samplesPerVBlank;
				if (next + LOCK_PATTERN > (int64_t) hifi->fifoCount[f] ||
				    memcmp(&hifi->fifoHistory[f][next], hifi->pendingExact[p + 1][half], LOCK_PATTERN) != 0) {
					continue;
				}
				const uint64_t* times = hifi->fifoTimes[f];
				hifi->latchPeriod = (double) (times[next] - times[at]) / hifi->samplesPerVBlank;
				hifi->t0 = times[at] - (double) (hifi->frameIndex + p) * hifi->samplesPerVBlank * hifi->latchPeriod;
				hifi->halfForFifo[f] = half;
				// The other FIFO normally carries the other half
				int other = 1 - f;
				hifi->halfForFifo[other] = -1;
				if (hifi->fifoCount[other]) {
					int64_t o = _findPattern(hifi, other, hifi->pendingExact[p][1 - half]);
					if (o >= 0) {
						hifi->halfForFifo[other] = 1 - half;
					} else if (_findPattern(hifi, other, hifi->pendingExact[p][half]) >= 0) {
						hifi->halfForFifo[other] = half;
					}
				}
				hifi->locked = true;
				return;
			}
		}
	}
}

void MP2KHiFiFifo(struct MP2KHiFi* hifi, int fifo, uint64_t when, int8_t sample) {
	if (hifi->locked || hifi->failed) {
		return;
	}
	if (hifi->fifoCount[fifo] == hifi->fifoCapacity) {
		size_t keep = hifi->fifoCapacity / 2;
		memmove(hifi->fifoHistory[fifo], &hifi->fifoHistory[fifo][hifi->fifoCount[fifo] - keep], keep);
		memmove(hifi->fifoTimes[fifo], &hifi->fifoTimes[fifo][hifi->fifoCount[fifo] - keep], keep * sizeof(uint64_t));
		hifi->fifoCount[fifo] = keep;
	}
	hifi->fifoHistory[fifo][hifi->fifoCount[fifo]] = sample;
	hifi->fifoTimes[fifo][hifi->fifoCount[fifo]] = when;
	++hifi->fifoCount[fifo];
}

void MP2KHiFiFrame(struct MP2KHiFi* hifi, uint64_t hookTime, const struct MP2KFrame* frame, const double fifoGain[2][2]) {
	if (hifi->failed || frame->samplesPerVBlank <= 0 || frame->samplesPerVBlank > MP2K_MAX_SAMPLES_PER_VBLANK) {
		return;
	}
	if (hifi->locked) {
		if (frame->samplesPerVBlank != hifi->samplesPerVBlank) {
			// The driver was reconfigured; timing no longer holds
			fprintf(stderr, "MP2K: mixing rate changed mid-song; high-precision mixing stopped\n");
			hifi->failed = true;
			hifi->horizon = INFINITY;
			return;
		}
		_renderFrame(hifi, hifi->frameIndex++, frame, fifoGain);
		return;
	}

	// Not locked yet: keep the frame, and its exact mix to find it in the FIFO
	if (hifi->pendingCount && frame->samplesPerVBlank != hifi->samplesPerVBlank) {
		hifi->frameIndex += hifi->pendingCount;
		hifi->pendingCount = 0;
	}
	hifi->samplesPerVBlank = frame->samplesPerVBlank;
	if (!hifi->pendingCount) {
		// Leading frames with no voice at all render nothing; just count them
		bool active = false;
		int c;
		for (c = 0; c < frame->maxChans; ++c) {
			active = active || (frame->chans[c].status & MP2K_SF_ON);
		}
		if (!active) {
			++hifi->frameIndex;
			hifi->horizon = INFINITY;
			return;
		}
	}
	if (hifi->pendingCount == MP2K_HIFI_MAX_PENDING) {
		hifi->failed = true;
		hifi->horizon = INFINITY;
		return;
	}
	struct MP2KHiFiPending* pend = &hifi->pending[hifi->pendingCount];
	pend->hookTime = hookTime;
	pend->frame = *frame;
	memcpy(pend->fifoGain, fifoGain, sizeof(pend->fifoGain));
	struct MP2KFrame copy = *frame;
	int8_t half0[MP2K_MAX_SAMPLES_PER_VBLANK];
	int8_t half1[MP2K_MAX_SAMPLES_PER_VBLANK];
	MP2KMixExact(&copy, hifi->mem, half0, half1);
	memcpy(hifi->pendingExact[hifi->pendingCount][0], half0, LOCK_PATTERN);
	memcpy(hifi->pendingExact[hifi->pendingCount][1], half1, LOCK_PATTERN);
	++hifi->pendingCount;
	// A frame plays after its hook, so nothing from the first pending frame on
	// may be finalized until it's rendered
	hifi->horizon = (double) hifi->pending[0].hookTime;

	_tryLock(hifi);
	if (hifi->locked) {
		size_t p;
		for (p = 0; p < hifi->pendingCount; ++p) {
			_renderFrame(hifi, hifi->frameIndex++, &hifi->pending[p].frame, hifi->pending[p].fifoGain);
		}
		hifi->pendingCount = 0;
	} else if (hifi->pendingCount > LOCK_GIVE_UP) {
		fprintf(stderr, "MP2K: couldn't find the driver's output in the FIFO stream; high-precision mixing disabled\n");
		hifi->failed = true;
		hifi->horizon = INFINITY;
	}
}

double MP2KHiFiHorizon(const struct MP2KHiFi* hifi) {
	return hifi->horizon;
}
