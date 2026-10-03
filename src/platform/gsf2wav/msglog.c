/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "msglog.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local struct MsgLog* _current;

void MsgLogInit(struct MsgLog* log) {
	memset(log, 0, sizeof(*log));
}

void MsgLogDeinit(struct MsgLog* log) {
	free(log->text);
	memset(log, 0, sizeof(*log));
}

void MsgLogSetCurrent(struct MsgLog* log) {
	_current = log;
}

// Each entry is stored as one level digit, the text, and a newline
static void _append(struct MsgLog* log, enum MsgLevel level, const char* text, size_t len) {
	size_t need = log->length + len + 3;
	if (need > log->capacity) {
		size_t capacity = log->capacity ? log->capacity : 256;
		while (capacity < need) {
			capacity *= 2;
		}
		log->text = realloc(log->text, capacity);
		log->capacity = capacity;
	}
	log->text[log->length++] = '0' + level;
	memcpy(&log->text[log->length], text, len);
	log->length += len;
	log->text[log->length++] = '\n';
	log->text[log->length] = '\0';
}

void MsgWrite(enum MsgLevel level, const char* fmt, ...) {
	char stack[512];
	char* buf = stack;
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(stack, sizeof(stack), fmt, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	if ((size_t) n >= sizeof(stack)) {
		buf = malloc(n + 1);
		va_start(args, fmt);
		vsnprintf(buf, n + 1, fmt, args);
		va_end(args);
	}
	// One entry per line
	char* line = buf;
	while (line) {
		char* end = strchr(line, '\n');
		size_t len = end ? (size_t) (end - line) : strlen(line);
		if (end || len) {
			if (_current) {
				_append(_current, level, line, len);
			} else if (level == MSG_OUT) {
				printf("%.*s\n", (int) len, line);
			} else if (level <= MSG_WARN) {
				fprintf(stderr, "%.*s\n", (int) len, line);
			}
		}
		line = end ? end + 1 : NULL;
	}
	if (buf != stack) {
		free(buf);
	}
}

void MsgLogPrint(const struct MsgLog* log, enum MsgLevel maxLevel, const char* prefix) {
	const char* p = log->text;
	const char* end = log->text ? log->text + log->length : NULL;
	while (p && p < end) {
		enum MsgLevel level = *p - '0';
		const char* line = p + 1;
		const char* nl = memchr(line, '\n', end - line);
		size_t len = nl - line;
		if (level == MSG_OUT) {
			printf("%.*s\n", (int) len, line);
		} else if (level <= maxLevel) {
			fprintf(stderr, "%s%.*s\n", prefix, (int) len, line);
		}
		p = nl + 1;
	}
}
