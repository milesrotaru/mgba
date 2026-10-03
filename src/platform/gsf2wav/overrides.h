/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_OVERRIDES_H
#define GSF2WAV_OVERRIDES_H

#include "options.h"

// Per-track options from a text file. Each line is a track name followed by
// options, and a # starts a comment:
//
//   006 --mp2k-linear-samples 082ED1BC
//   "99 Unknown Song 3" -g -0.5
//
// A name is either a track's whole file name without its extension (quoted if
// it has spaces), or just the first word of it, which for ripped sets is
// usually the track number. The whole name wins over the first word. A
// track's options are applied after the ones from the command line, so they
// can override them.

struct OverrideEntry {
	char* key;
	char** tokens;
	int count;
	int line;
};

struct Overrides {
	struct OverrideEntry* entries;
	size_t count;
};

// Reads and checks every line; on failure false, with the file, line number
// and problem in err.
bool OverridesLoad(struct Overrides* overrides, const char* path, char* err, size_t errLen);
void OverridesDeinit(struct Overrides* overrides);

// Applies the options for a track (named by its file name without extension)
// to opts. Returns whether there were any.
bool OverridesApply(const struct Overrides* overrides, const char* stem, struct RenderOptions* opts);

#endif
