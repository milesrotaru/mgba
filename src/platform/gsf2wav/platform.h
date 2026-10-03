/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GSF2WAV_PLATFORM_H
#define GSF2WAV_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// The few places gsf2wav touches the operating system. Every path and string
// that goes in or out is UTF-8, on Windows too (where the C library's own
// functions would use the ANSI code page and mangle anything outside it, like
// Japanese file names).

// The command line as UTF-8. On Windows this also expands wildcards, which
// cmd.exe leaves to the program. Free the result with PlatArgsFree.
char** PlatArgs(int argc, char** argv, int* outArgc);
void PlatArgsFree(char** argv, int argc);

FILE* PlatFOpen(const char* path, const char* mode);
bool PlatExists(const char* path);
bool PlatIsDir(const char* path);
// A device, pipe or similar rather than a plain file (/dev/null, NUL): writes
// to it must not go through a temporary file and a rename.
bool PlatIsDevice(const char* path);
// Creates the directory and any missing parents.
bool PlatMakeDirs(const char* path);
// Renames from over to, replacing it if it exists.
bool PlatReplace(const char* from, const char* to);
bool PlatRemove(const char* path);

// Calls cb with the name (not the path) of every entry in a directory until it
// returns false. False if the directory can't be read.
bool PlatListDir(const char* dir, bool (*cb)(const char* name, void* user), void* user);

// Paths: both separators count on Windows.
const char* PlatBaseName(const char* path);
const char* PlatExtension(const char* path); // ".ext", or "" if none
// The directory part of path, "." if it has none. Free with free().
char* PlatDirName(const char* path);
void PlatJoin(char* out, size_t size, const char* dir, const char* name);
bool PlatEndsWithSeparator(const char* path);
// ASCII case-insensitive comparison, enough for file names and extensions.
int PlatCaseCompare(const char* a, const char* b);

int PlatCpuCount(void);
// Seconds from some fixed moment; only differences mean anything.
double PlatNowSeconds(void);
bool PlatStderrIsTty(void);

// Windows console niceties; no-ops elsewhere.
void PlatConsoleInit(void);
// True when the program got a console window of its own, which is what
// happens when it's started from Explorer rather than from a shell: the
// window closes the moment the program ends.
bool PlatConsoleIsOwned(void);
void PlatPause(const char* prompt);

#endif
