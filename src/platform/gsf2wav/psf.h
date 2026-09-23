/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_PSF_H
#define GSF2WAV_PSF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GSF_VERSION 0x22

struct PSFTag {
	char* key;
	char* value;
};

struct PSFTags {
	struct PSFTag* tags;
	size_t count;
};

struct GSFImage {
	uint8_t* data;
	size_t size;
	uint32_t entry;
	bool haveEntry;
};

// Loads a (mini)GSF, following its _lib/_lib2.._libN chain in psflib order:
// _lib (recursively), then the file's own program, then _lib2, _lib3, ...
// Only the top-level file's tags are returned.
bool GSFLoad(const char* path, struct GSFImage* image, struct PSFTags* tags, char* err, size_t errLen);

void GSFImageDeinit(struct GSFImage* image);
void PSFTagsDeinit(struct PSFTags* tags);
const char* PSFTagGet(const struct PSFTags* tags, const char* key);

// Parses "[[h:]m:]s[.fff]" (',' also accepted as decimal separator).
// Returns a negative value on failure.
double PSFParseTime(const char* str);

#endif
