/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_OPTIONS_H
#define GSF2WAV_OPTIONS_H

#include "hifi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DEFAULT_RATE 48000
#define DEFAULT_LENGTH 150.0
#define DEFAULT_FADE 10.0
#define DEFAULT_PSG_GRID 8
#define DEFAULT_RAMP_MS 2.0
#define MAX_LINEAR_SAMPLES 64

enum SampleFormat {
	FORMAT_F32,
	FORMAT_S24,
	FORMAT_S16,
};

// Everything that shapes how one track is rendered. These are the options an
// overrides file can set per track.
struct RenderOptions {
	char* bios;
	char* sampleRom;
	unsigned rate;
	enum SampleFormat format;
	double length; // negative: from the file's tags
	double fade;
	double gainDb;
	bool useVolumeTag;
	bool fifoHold;
	unsigned psgGrid;
	bool verify;
	bool hifi;
	enum HiFiMode hifiMode;
	double rampMs;
	bool mutePsg;
	bool muteFifo;
	uint32_t muteChannels;
	double bandwidth;
	bool bandwidthDriver;
	double sourceCutoff;
	uint32_t soloWav;
	bool sampleStats;
	bool noFillHoles;
	uint32_t linearWavs[MAX_LINEAR_SAMPLES];
	size_t linearWavCount;
};

// What to run, as opposed to how to render each track.
struct RunOptions {
	const char* output; // -o: a folder, or the file for a single input
	const char* overrides;
	bool skipExisting;
	int jobs; // 0: as many as there are cores
	int verbosity; // -1 quiet, 0 normal, 1 verbose
	bool help;
	bool version;
};

void RenderOptionsInit(struct RenderOptions* opts);
void RenderOptionsCopy(struct RenderOptions* dst, const struct RenderOptions* src);
void RenderOptionsDeinit(struct RenderOptions* opts);

// Parses a list of arguments (without the program name). Options are applied
// to render and, if it isn't NULL, run; with run NULL (an overrides file's
// line) only the per-track options are accepted. Anything that isn't an option
// is stored in positionals (pointers into argv), if given. On failure, false
// with a message in err.
bool OptionsParse(int argc, char** argv, struct RenderOptions* render, struct RunOptions* run, char** positionals,
                  int* positionalCount, char* err, size_t errLen);

void OptionsUsage(FILE* out, const char* program);

#endif
