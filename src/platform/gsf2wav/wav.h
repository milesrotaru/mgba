/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_WAV_H
#define GSF2WAV_WAV_H

#include "options.h"

#include <stdio.h>

// A streaming stereo WAV writer: 32-bit float, or 24 / 16-bit PCM with TPDF
// dither. The header's sizes are filled in when the file is closed.
struct WavWriter {
	FILE* f;
	enum SampleFormat format;
	unsigned rate;
	uint64_t frames;
	uint32_t rng;
};

bool WavOpen(struct WavWriter* w, const char* path, enum SampleFormat format, unsigned rate);
// Interleaved left, right, in the range -1..1 for full scale.
bool WavWrite(struct WavWriter* w, const double* samples, size_t frames);
// Writes the final header if finalize is set, and closes the file. False if
// anything failed along the way, including the close itself (a full disk).
bool WavClose(struct WavWriter* w, bool finalize);

#endif
