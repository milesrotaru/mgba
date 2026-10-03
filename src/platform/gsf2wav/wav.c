/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "wav.h"

#include "platform.h"

#include <math.h>
#include <string.h>

static void _put16(uint8_t* b, uint16_t v) {
	b[0] = v;
	b[1] = v >> 8;
}

static void _put32(uint8_t* b, uint32_t v) {
	b[0] = v;
	b[1] = v >> 8;
	b[2] = v >> 16;
	b[3] = v >> 24;
}

static unsigned _bytesPerSample(enum SampleFormat format) {
	switch (format) {
	case FORMAT_F32:
		return 4;
	case FORMAT_S24:
		return 3;
	case FORMAT_S16:
	default:
		return 2;
	}
}

static bool _writeHeader(struct WavWriter* w) {
	uint8_t h[58];
	unsigned bps = _bytesPerSample(w->format);
	bool isFloat = w->format == FORMAT_F32;
	uint32_t fmtSize = isFloat ? 18 : 16;
	uint64_t dataSize = w->frames * 2 * bps;
	if (dataSize > 0xFFFFFF00ULL) {
		dataSize = 0xFFFFFF00ULL;
	}
	size_t headerSize = 12 + 8 + fmtSize + (isFloat ? 12 : 0) + 8;
	memcpy(h, "RIFF", 4);
	_put32(&h[4], (uint32_t) (headerSize - 8 + dataSize));
	memcpy(&h[8], "WAVE", 4);
	memcpy(&h[12], "fmt ", 4);
	_put32(&h[16], fmtSize);
	_put16(&h[20], isFloat ? 3 : 1);
	_put16(&h[22], 2);
	_put32(&h[24], w->rate);
	_put32(&h[28], w->rate * 2 * bps);
	_put16(&h[32], 2 * bps);
	_put16(&h[34], bps * 8);
	size_t o = 36;
	if (isFloat) {
		_put16(&h[o], 0);
		o += 2;
		memcpy(&h[o], "fact", 4);
		_put32(&h[o + 4], 4);
		_put32(&h[o + 8], (uint32_t) w->frames);
		o += 12;
	}
	memcpy(&h[o], "data", 4);
	_put32(&h[o + 4], (uint32_t) dataSize);
	o += 8;
	return fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, o, w->f) == o;
}

bool WavOpen(struct WavWriter* w, const char* path, enum SampleFormat format, unsigned rate) {
	memset(w, 0, sizeof(*w));
	w->format = format;
	w->rate = rate;
	w->rng = 0x2545F491;
	w->f = PlatFOpen(path, "wb");
	if (!w->f) {
		return false;
	}
	if (!_writeHeader(w)) {
		fclose(w->f);
		w->f = NULL;
		return false;
	}
	return true;
}

static double _tpdf(struct WavWriter* w) {
	// Two uniform variables in [0, 1) summed gives triangular noise of +/-1 LSB
	w->rng = w->rng * 1664525 + 1013904223;
	double a = (w->rng >> 8) / 16777216.0;
	w->rng = w->rng * 1664525 + 1013904223;
	double b = (w->rng >> 8) / 16777216.0;
	return a - b;
}

bool WavWrite(struct WavWriter* w, const double* samples, size_t frames) {
	uint8_t buf[4096 * 2 * 4];
	size_t done = 0;
	while (done < frames) {
		size_t chunk = frames - done;
		if (chunk > 4096) {
			chunk = 4096;
		}
		size_t i;
		size_t o = 0;
		for (i = 0; i < chunk * 2; ++i) {
			double s = samples[done * 2 + i];
			switch (w->format) {
			case FORMAT_F32: {
				float f = (float) s;
				uint32_t bits;
				memcpy(&bits, &f, 4);
				_put32(&buf[o], bits);
				o += 4;
				break;
			}
			case FORMAT_S24: {
				double v = floor(s * 8388608.0 + _tpdf(w) + 0.5);
				int32_t q = v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : (int32_t) v;
				buf[o] = q;
				buf[o + 1] = q >> 8;
				buf[o + 2] = q >> 16;
				o += 3;
				break;
			}
			case FORMAT_S16: {
				double v = floor(s * 32768.0 + _tpdf(w) + 0.5);
				int32_t q = v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t) v;
				_put16(&buf[o], (uint16_t) q);
				o += 2;
				break;
			}
			}
		}
		if (fwrite(buf, 1, o, w->f) != o) {
			return false;
		}
		w->frames += chunk;
		done += chunk;
	}
	return true;
}

bool WavClose(struct WavWriter* w, bool finalize) {
	if (!w->f) {
		return false;
	}
	bool ok = true;
	if (finalize) {
		ok = _writeHeader(w);
	}
	if (fclose(w->f) != 0) {
		ok = false;
	}
	w->f = NULL;
	return ok;
}
