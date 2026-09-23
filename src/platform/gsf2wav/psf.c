/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "psf.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define PSF_MAX_DEPTH 10
#define GSF_REGION_MASK 0x01FFFFFF
#define GSF_ROM_MAX 0x02000000
#define GSF_EWRAM_MAX 0x00040000

static void _error(char* err, size_t errLen, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(err, errLen, fmt, args);
	va_end(args);
}

static uint32_t _le32(const uint8_t* b) {
	return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24);
}

static char* _strndup(const char* s, size_t n) {
	char* out = malloc(n + 1);
	memcpy(out, s, n);
	out[n] = '\0';
	return out;
}

static void _tagSet(struct PSFTags* tags, const char* key, size_t keyLen, const char* value, size_t valueLen) {
	size_t i;
	for (i = 0; i < tags->count; ++i) {
		if (strlen(tags->tags[i].key) == keyLen && strncasecmp(tags->tags[i].key, key, keyLen) == 0) {
			// Repeated keys are joined with newlines, per the PSF spec
			size_t oldLen = strlen(tags->tags[i].value);
			tags->tags[i].value = realloc(tags->tags[i].value, oldLen + valueLen + 2);
			tags->tags[i].value[oldLen] = '\n';
			memcpy(&tags->tags[i].value[oldLen + 1], value, valueLen);
			tags->tags[i].value[oldLen + valueLen + 1] = '\0';
			return;
		}
	}
	tags->tags = realloc(tags->tags, sizeof(*tags->tags) * (tags->count + 1));
	struct PSFTag* tag = &tags->tags[tags->count++];
	tag->key = _strndup(key, keyLen);
	size_t k;
	for (k = 0; k < keyLen; ++k) {
		tag->key[k] = tolower((unsigned char) tag->key[k]);
	}
	tag->value = _strndup(value, valueLen);
}

static void _parseTags(const uint8_t* data, size_t size, struct PSFTags* tags) {
	if (size < 5 || memcmp(data, "[TAG]", 5) != 0) {
		return;
	}
	const char* p = (const char*) data + 5;
	const char* end = (const char*) data + size;
	while (p < end) {
		const char* eol = memchr(p, '\n', end - p);
		if (!eol) {
			eol = end;
		}
		const char* eq = memchr(p, '=', eol - p);
		if (eq) {
			const char* ks = p;
			const char* ke = eq;
			const char* vs = eq + 1;
			const char* ve = eol;
			while (ks < ke && (unsigned char) *ks <= ' ') {
				++ks;
			}
			while (ke > ks && (unsigned char) ke[-1] <= ' ') {
				--ke;
			}
			while (vs < ve && (unsigned char) *vs <= ' ') {
				++vs;
			}
			while (ve > vs && (unsigned char) ve[-1] <= ' ') {
				--ve;
			}
			if (ke > ks) {
				_tagSet(tags, ks, ke - ks, vs, ve - vs);
			}
		}
		p = eol + 1;
	}
}

void PSFTagsDeinit(struct PSFTags* tags) {
	size_t i;
	for (i = 0; i < tags->count; ++i) {
		free(tags->tags[i].key);
		free(tags->tags[i].value);
	}
	free(tags->tags);
	tags->tags = NULL;
	tags->count = 0;
}

const char* PSFTagGet(const struct PSFTags* tags, const char* key) {
	size_t i;
	for (i = 0; i < tags->count; ++i) {
		if (strcasecmp(tags->tags[i].key, key) == 0) {
			return tags->tags[i].value;
		}
	}
	return NULL;
}

void GSFImageDeinit(struct GSFImage* image) {
	free(image->data);
	memset(image, 0, sizeof(*image));
}

double PSFParseTime(const char* str) {
	if (!str) {
		return -1;
	}
	double total = 0;
	double part = 0;
	double frac = 0;
	double scale = 0;
	bool any = false;
	for (; *str; ++str) {
		char c = *str;
		if (c >= '0' && c <= '9') {
			any = true;
			if (scale > 0) {
				frac += (c - '0') * scale;
				scale /= 10;
			} else {
				part = part * 10 + (c - '0');
			}
		} else if (c == ':') {
			if (scale > 0) {
				return -1;
			}
			total = (total + part) * 60;
			part = 0;
		} else if (c == '.' || c == ',') {
			if (scale > 0) {
				return -1;
			}
			scale = 0.1;
		} else if (c == ' ' || c == '\t') {
			continue;
		} else {
			break;
		}
	}
	if (!any) {
		return -1;
	}
	return total + part + frac;
}

static char* _dirname(const char* path) {
	const char* slash = strrchr(path, '/');
	if (!slash) {
		return strdup(".");
	}
	return _strndup(path, slash - path);
}

// Rips are usually made on case-insensitive filesystems, so a _lib tag may not
// match the actual filename's case.
static FILE* _openLib(const char* dir, const char* name, char** resolved) {
	size_t len = strlen(dir) + strlen(name) + 2;
	char* path = malloc(len);
	snprintf(path, len, "%s/%s", dir, name);
	FILE* f = fopen(path, "rb");
	if (f) {
		*resolved = path;
		return f;
	}
	free(path);
	DIR* d = opendir(dir);
	if (!d) {
		return NULL;
	}
	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (strcasecmp(ent->d_name, name) == 0) {
			len = strlen(dir) + strlen(ent->d_name) + 2;
			path = malloc(len);
			snprintf(path, len, "%s/%s", dir, ent->d_name);
			f = fopen(path, "rb");
			if (f) {
				*resolved = path;
				break;
			}
			free(path);
		}
	}
	closedir(d);
	return f;
}

static bool _readFile(FILE* f, uint8_t** data, size_t* size) {
	if (fseek(f, 0, SEEK_END) != 0) {
		return false;
	}
	long len = ftell(f);
	if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
		return false;
	}
	*data = malloc(len ? len : 1);
	if (fread(*data, 1, len, f) != (size_t) len) {
		free(*data);
		return false;
	}
	*size = len;
	return true;
}

static bool _uploadSection(struct GSFImage* image, const uint8_t* data, size_t size, const char* path, char* err, size_t errLen) {
	if (size < 12) {
		_error(err, errLen, "%s: program section too small", path);
		return false;
	}
	uint32_t entry = _le32(&data[0]);
	uint32_t offset = _le32(&data[4]);
	uint32_t romSize = _le32(&data[8]);
	if (romSize > size - 12) {
		_error(err, errLen, "%s: program section claims %u bytes but only has %zu", path, romSize, size - 12);
		return false;
	}
	if (!image->haveEntry) {
		image->entry = entry;
		image->haveEntry = true;
	}
	size_t max = (image->entry >> 24) == 0x02 ? GSF_EWRAM_MAX : GSF_ROM_MAX;
	size_t start = offset & GSF_REGION_MASK;
	if (start + romSize > max) {
		_error(err, errLen, "%s: section at 0x%08X + 0x%X overflows its memory region", path, offset, romSize);
		return false;
	}
	size_t needed = start + romSize;
	if (needed > image->size) {
		// Round up to a power of two so the core's ROM mirroring behaves
		size_t rounded = 1;
		while (rounded < needed) {
			rounded <<= 1;
		}
		image->data = realloc(image->data, rounded);
		memset(&image->data[image->size], 0, rounded - image->size);
		image->size = rounded;
	}
	memcpy(&image->data[start], &data[12], romSize);
	return true;
}

static bool _load(const char* path, FILE* f, struct GSFImage* image, struct PSFTags* outTags, int depth, char* err, size_t errLen) {
	if (depth > PSF_MAX_DEPTH) {
		_error(err, errLen, "%s: _lib chain too deep", path);
		return false;
	}
	uint8_t* file;
	size_t fileSize;
	if (!_readFile(f, &file, &fileSize)) {
		_error(err, errLen, "%s: could not read file", path);
		return false;
	}
	bool ok = false;
	struct PSFTags tags = {0};
	char* dir = _dirname(path);
	uint8_t* program = NULL;

	if (fileSize < 16 || memcmp(file, "PSF", 3) != 0) {
		_error(err, errLen, "%s: not a PSF file", path);
		goto done;
	}
	if (file[3] != GSF_VERSION) {
		_error(err, errLen, "%s: PSF version 0x%02X is not GSF (0x22)", path, file[3]);
		goto done;
	}
	uint32_t reservedSize = _le32(&file[4]);
	uint32_t compressedSize = _le32(&file[8]);
	uint32_t crc = _le32(&file[12]);
	if ((uint64_t) 16 + reservedSize + compressedSize > fileSize) {
		_error(err, errLen, "%s: truncated file", path);
		goto done;
	}
	const uint8_t* compressed = &file[16 + reservedSize];
	size_t tagOffset = 16 + reservedSize + compressedSize;
	_parseTags(&file[tagOffset], fileSize - tagOffset, &tags);

	const char* lib = PSFTagGet(&tags, "_lib");
	if (lib) {
		char* libPath;
		FILE* lf = _openLib(dir, lib, &libPath);
		if (!lf) {
			_error(err, errLen, "%s: could not open _lib \"%s\"", path, lib);
			goto done;
		}
		bool libOk = _load(libPath, lf, image, NULL, depth + 1, err, errLen);
		fclose(lf);
		free(libPath);
		if (!libOk) {
			goto done;
		}
	}

	if (compressedSize) {
		if (crc32(crc32(0, Z_NULL, 0), compressed, compressedSize) != crc) {
			_error(err, errLen, "%s: CRC mismatch in program section", path);
			goto done;
		}
		uLongf programSize = compressedSize * 4 + 0x10000;
		int zerr;
		while (true) {
			program = realloc(program, programSize);
			uLongf got = programSize;
			zerr = uncompress(program, &got, compressed, compressedSize);
			if (zerr == Z_BUF_ERROR && programSize < 0x4000000) {
				programSize *= 2;
				continue;
			}
			programSize = got;
			break;
		}
		if (zerr != Z_OK) {
			_error(err, errLen, "%s: could not decompress program section (zlib error %d)", path, zerr);
			goto done;
		}
		if (!_uploadSection(image, program, programSize, path, err, errLen)) {
			goto done;
		}
	}

	unsigned n;
	for (n = 2;; ++n) {
		char key[16];
		snprintf(key, sizeof(key), "_lib%u", n);
		lib = PSFTagGet(&tags, key);
		if (!lib) {
			break;
		}
		char* libPath;
		FILE* lf = _openLib(dir, lib, &libPath);
		if (!lf) {
			_error(err, errLen, "%s: could not open %s \"%s\"", path, key, lib);
			goto done;
		}
		bool libOk = _load(libPath, lf, image, NULL, depth + 1, err, errLen);
		fclose(lf);
		free(libPath);
		if (!libOk) {
			goto done;
		}
	}
	ok = true;

done:
	if (ok && outTags) {
		*outTags = tags;
	} else {
		PSFTagsDeinit(&tags);
	}
	free(program);
	free(dir);
	free(file);
	return ok;
}

bool GSFLoad(const char* path, struct GSFImage* image, struct PSFTags* tags, char* err, size_t errLen) {
	memset(image, 0, sizeof(*image));
	memset(tags, 0, sizeof(*tags));
	FILE* f = fopen(path, "rb");
	if (!f) {
		_error(err, errLen, "%s: could not open file", path);
		return false;
	}
	bool ok = _load(path, f, image, tags, 0, err, errLen);
	fclose(f);
	if (ok && !image->size) {
		_error(err, errLen, "%s: no program data in file or its libs", path);
		ok = false;
	}
	if (!ok) {
		GSFImageDeinit(image);
	}
	return ok;
}
