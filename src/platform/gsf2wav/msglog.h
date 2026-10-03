/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_MSGLOG_H
#define GSF2WAV_MSGLOG_H

#include <stdio.h>

// Messages from a render go into a log of its own instead of straight to the
// terminal, so that renders running on several threads don't interleave their
// output, and so the caller decides how much of it to show.

enum MsgLevel {
	MSG_OUT = 0,    // program output (to stdout), always shown
	MSG_ERROR = 1,  // the render failed
	MSG_WARN = 2,   // the render finished but something is off
	MSG_INFO = 3,   // the usual per-track information
	MSG_DETAIL = 4, // driver and timing details, for -v
};

struct MsgLog {
	char* text;
	size_t length;
	size_t capacity;
};

void MsgLogInit(struct MsgLog* log);
void MsgLogDeinit(struct MsgLog* log);

// Where this thread's MsgWrite calls go. With none set (NULL) they're printed
// at once: errors and warnings to stderr, MSG_OUT to stdout, the rest dropped.
void MsgLogSetCurrent(struct MsgLog* log);

void MsgWrite(enum MsgLevel level, const char* fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

// Prints the entries up to maxLevel, each stderr line after prefix. Entries
// stay in the log.
void MsgLogPrint(const struct MsgLog* log, enum MsgLevel maxLevel, const char* prefix);

#endif
