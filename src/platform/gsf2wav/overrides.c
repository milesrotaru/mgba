/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "overrides.h"

#include "platform.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
static void _fail(char* err, size_t errLen, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(err, errLen, fmt, args);
	va_end(args);
}

// Splits a line into words. Words are separated by spaces; "double" or 'single'
// quotes group a word that has spaces in it. There are no escapes, so Windows
// paths can be written as they are.
static bool _tokenize(char* line, char** tokens, int* count, int max) {
	int n = 0;
	char* p = line;
	while (*p) {
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (!*p) {
			break;
		}
		if (n == max) {
			return false;
		}
		if (*p == '"' || *p == '\'') {
			char quote = *p++;
			tokens[n++] = p;
			while (*p && *p != quote) {
				++p;
			}
			if (!*p) {
				return false;
			}
			*p++ = '\0';
		} else {
			tokens[n++] = p;
			while (*p && *p != ' ' && *p != '\t') {
				++p;
			}
			if (*p) {
				*p++ = '\0';
			}
		}
	}
	*count = n;
	return true;
}

bool OverridesLoad(struct Overrides* overrides, const char* path, char* err, size_t errLen) {
	memset(overrides, 0, sizeof(*overrides));
	FILE* f = PlatFOpen(path, "rb");
	if (!f) {
		_fail(err, errLen, "%s: could not open the overrides file", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* text = malloc(size + 1);
	size_t got = size > 0 ? fread(text, 1, size, f) : 0;
	fclose(f);
	text[got] = '\0';

	char* p = text;
	// A UTF-8 byte order mark, which Notepad adds
	if ((unsigned char) p[0] == 0xEF && (unsigned char) p[1] == 0xBB && (unsigned char) p[2] == 0xBF) {
		p += 3;
	}
	int lineNumber = 0;
	bool ok = true;
	while (*p && ok) {
		char* end = strchr(p, '\n');
		if (end) {
			*end = '\0';
		}
		++lineNumber;
		char* line = p;
		p = end ? end + 1 : p + strlen(p);
		char* hash = strchr(line, '#');
		if (hash) {
			*hash = '\0';
		}
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
			line[--len] = '\0';
		}

		char* tokens[64];
		int count = 0;
		if (!_tokenize(line, tokens, &count, 64)) {
			_fail(err, errLen, "%s:%d: unbalanced quote, or too many words", path, lineNumber);
			ok = false;
			break;
		}
		if (count == 0) {
			continue;
		}
		// Checked now so a typo fails before an hour of rendering, not during it
		struct RenderOptions scratch;
		RenderOptionsInit(&scratch);
		char problem[200];
		bool valid = OptionsParse(count - 1, tokens + 1, &scratch, NULL, NULL, NULL, problem, sizeof(problem));
		RenderOptionsDeinit(&scratch);
		if (!valid) {
			_fail(err, errLen, "%s:%d: %s", path, lineNumber, problem);
			ok = false;
			break;
		}
		overrides->entries = realloc(overrides->entries, (overrides->count + 1) * sizeof(*overrides->entries));
		struct OverrideEntry* e = &overrides->entries[overrides->count++];
		e->key = strdup(tokens[0]);
		e->count = count - 1;
		e->tokens = malloc((count > 1 ? count - 1 : 1) * sizeof(*e->tokens));
		int i;
		for (i = 1; i < count; ++i) {
			e->tokens[i - 1] = strdup(tokens[i]);
		}
		e->line = lineNumber;
	}
	free(text);
	if (!ok) {
		OverridesDeinit(overrides);
	}
	return ok;
}

void OverridesDeinit(struct Overrides* overrides) {
	size_t i;
	int j;
	for (i = 0; i < overrides->count; ++i) {
		free(overrides->entries[i].key);
		for (j = 0; j < overrides->entries[i].count; ++j) {
			free(overrides->entries[i].tokens[j]);
		}
		free(overrides->entries[i].tokens);
	}
	free(overrides->entries);
	memset(overrides, 0, sizeof(*overrides));
}

static bool _applyMatching(const struct Overrides* overrides, const char* key, size_t keyLen, struct RenderOptions* opts) {
	bool any = false;
	size_t i;
	for (i = 0; i < overrides->count; ++i) {
		const struct OverrideEntry* e = &overrides->entries[i];
		if (strlen(e->key) == keyLen && PlatCaseCompare(e->key, key) == 0) {
			char problem[200];
			// Can't fail: it parsed when the file was loaded
			OptionsParse(e->count, e->tokens, opts, NULL, NULL, NULL, problem, sizeof(problem));
			any = true;
		}
	}
	return any;
}

bool OverridesApply(const struct Overrides* overrides, const char* stem, struct RenderOptions* opts) {
	if (_applyMatching(overrides, stem, strlen(stem), opts)) {
		return true;
	}
	size_t word = strcspn(stem, " ");
	char* first = malloc(word + 1);
	memcpy(first, stem, word);
	first[word] = '\0';
	bool any = _applyMatching(overrides, first, word, opts);
	free(first);
	return any;
}
