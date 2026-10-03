/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_RENDER_H
#define GSF2WAV_RENDER_H

#include "options.h"

// How a track's DirectSound audio ended up being produced.
enum RenderPath {
	RENDER_PLAIN,    // the game's own mix (no known driver, or --no-hifi)
	RENDER_HIFI,     // the driver's voices re-rendered
	RENDER_PSG_ONLY, // a known driver, but it never played any sampled audio
	RENDER_FALLBACK, // re-rendering was attempted and given up on
};

struct RenderResult {
	bool ok;
	char title[256]; // "Game - Title" from the file's tags, if it has them
	double seconds;  // of audio written, fade included
	double peak;     // linear, after gain and fade
	enum RenderPath path;
	const char* driver; // "MP2K", "AlphaDream", or NULL
	bool noDriver;      // re-rendering was wanted but no known driver was found
	bool verified;
	uint64_t verifyFrames;
	uint64_t verifyBad;
	uint64_t resyncs;
	uint64_t samplesLost;
	uint64_t fadeOutsDropped;
	uint64_t lateEvents;
};

typedef void (*RenderProgress)(void* user, int percent);

// Renders one track to a WAV file. Messages go to the calling thread's
// current log (see msglog.h). progress, if given, is called as the percentage
// done changes.
bool RenderTrack(const struct RenderOptions* opts, const char* input, const char* output, RenderProgress progress,
                 void* user, struct RenderResult* result);

#endif
