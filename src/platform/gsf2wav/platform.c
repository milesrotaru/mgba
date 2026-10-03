/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#define SEP_CHARS "/\\"
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#define SEP_CHARS "/"
#endif

static bool _isSep(char c) {
	return strchr(SEP_CHARS, c) != NULL && c != '\0';
}

const char* PlatBaseName(const char* path) {
	const char* base = path;
	const char* p;
	for (p = path; *p; ++p) {
		if (_isSep(*p)) {
			base = p + 1;
		}
	}
#ifdef _WIN32
	// "C:file"
	if (base == path && path[0] && path[1] == ':') {
		base = path + 2;
	}
#endif
	return base;
}

const char* PlatExtension(const char* path) {
	const char* base = PlatBaseName(path);
	const char* dot = strrchr(base, '.');
	if (!dot || dot == base) {
		return base + strlen(base);
	}
	return dot;
}

char* PlatDirName(const char* path) {
	const char* base = PlatBaseName(path);
	if (base == path) {
		return strdup(".");
	}
	size_t len = base - path;
	// Keep the root's own separator ("/", "C:\"), drop any other trailing one
	if (len > 1 && _isSep(path[len - 1]) && !(len == 3 && path[1] == ':')) {
		--len;
	}
	char* out = malloc(len + 1);
	memcpy(out, path, len);
	out[len] = '\0';
	return out;
}

bool PlatEndsWithSeparator(const char* path) {
	size_t len = strlen(path);
	return len && _isSep(path[len - 1]);
}

void PlatJoin(char* out, size_t size, const char* dir, const char* name) {
	if (!*dir || strcmp(dir, ".") == 0) {
		snprintf(out, size, "%s", name);
	} else if (PlatEndsWithSeparator(dir)) {
		snprintf(out, size, "%s%s", dir, name);
	} else {
#ifdef _WIN32
		snprintf(out, size, "%s\\%s", dir, name);
#else
		snprintf(out, size, "%s/%s", dir, name);
#endif
	}
}

int PlatCaseCompare(const char* a, const char* b) {
	while (*a && *b) {
		int ca = tolower((unsigned char) *a);
		int cb = tolower((unsigned char) *b);
		if (ca != cb) {
			return ca - cb;
		}
		++a;
		++b;
	}
	return tolower((unsigned char) *a) - tolower((unsigned char) *b);
}

#ifdef _WIN32

static wchar_t* _wide(const char* s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t* w = malloc(n * sizeof(*w));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static char* _utf8(const wchar_t* w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 0) {
		return NULL;
	}
	char* s = malloc(n);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return s;
}

static int _compareForSort(const void* a, const void* b) {
	return PlatCaseCompare(*(char* const*) a, *(char* const*) b);
}

struct ArgList {
	char** items;
	int count;
	int capacity;
};

static void _push(struct ArgList* list, char* item) {
	if (list->count == list->capacity) {
		list->capacity = list->capacity ? list->capacity * 2 : 16;
		list->items = realloc(list->items, list->capacity * sizeof(*list->items));
	}
	list->items[list->count++] = item;
}

// A pattern that matches nothing stays as typed, so the caller reports it as a
// missing file by name.
static void _expandWildcard(struct ArgList* list, const char* arg) {
	wchar_t* wpattern = _wide(arg);
	WIN32_FIND_DATAW find;
	HANDLE h = wpattern ? FindFirstFileW(wpattern, &find) : INVALID_HANDLE_VALUE;
	free(wpattern);
	if (h == INVALID_HANDLE_VALUE) {
		_push(list, strdup(arg));
		return;
	}
	size_t dirLen = PlatBaseName(arg) - arg;
	struct ArgList found = { 0 };
	do {
		if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0) {
			continue;
		}
		char* name = _utf8(find.cFileName);
		if (!name) {
			continue;
		}
		char* full = malloc(dirLen + strlen(name) + 1);
		memcpy(full, arg, dirLen);
		strcpy(full + dirLen, name);
		free(name);
		_push(&found, full);
	} while (FindNextFileW(h, &find));
	FindClose(h);
	qsort(found.items, found.count, sizeof(*found.items), _compareForSort);
	int i;
	for (i = 0; i < found.count; ++i) {
		_push(list, found.items[i]);
	}
	free(found.items);
}

char** PlatArgs(int argc, char** argv, int* outArgc) {
	(void) argc;
	(void) argv;
	int wargc = 0;
	wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	struct ArgList list = { 0 };
	int i;
	for (i = 0; wargv && i < wargc; ++i) {
		char* arg = _utf8(wargv[i]);
		if (!arg) {
			continue;
		}
		// Options keep their '?' and '*'; only things that look like paths expand
		if (i > 0 && arg[0] != '-' && strpbrk(arg, "*?")) {
			_expandWildcard(&list, arg);
			free(arg);
		} else {
			_push(&list, arg);
		}
	}
	if (wargv) {
		LocalFree(wargv);
	}
	_push(&list, NULL);
	*outArgc = list.count - 1;
	return list.items;
}

FILE* PlatFOpen(const char* path, const char* mode) {
	wchar_t* wpath = _wide(path);
	wchar_t* wmode = _wide(mode);
	FILE* f = wpath && wmode ? _wfopen(wpath, wmode) : NULL;
	free(wpath);
	free(wmode);
	return f;
}

static DWORD _attributes(const char* path) {
	wchar_t* w = _wide(path);
	DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
	free(w);
	return a;
}

bool PlatExists(const char* path) {
	return _attributes(path) != INVALID_FILE_ATTRIBUTES;
}

bool PlatIsDir(const char* path) {
	DWORD a = _attributes(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool PlatIsDevice(const char* path) {
	// DOS device names count with any extension ("NUL", "nul.wav", "COM1")
	char stem[16];
	const char* base = PlatBaseName(path);
	size_t n = strcspn(base, ".");
	if (n >= sizeof(stem)) {
		return false;
	}
	memcpy(stem, base, n);
	stem[n] = '\0';
	static const char* const names[] = { "CON", "PRN", "AUX", "NUL" };
	size_t i;
	for (i = 0; i < sizeof(names) / sizeof(*names); ++i) {
		if (PlatCaseCompare(stem, names[i]) == 0) {
			return true;
		}
	}
	// COM1..COM9, LPT1..LPT9
	char prefix[4];
	memcpy(prefix, stem, 3);
	prefix[3] = '\0';
	return n == 4 && stem[3] >= '1' && stem[3] <= '9' &&
	       (PlatCaseCompare(prefix, "COM") == 0 || PlatCaseCompare(prefix, "LPT") == 0);
}

bool PlatMakeDirs(const char* path) {
	if (PlatIsDir(path)) {
		return true;
	}
	char* buf = strdup(path);
	size_t len = strlen(buf);
	size_t i;
	// Skip a drive ("C:") or UNC prefix so we don't try to create those
	size_t start = (len > 2 && buf[1] == ':') ? 3 : 1;
	for (i = start; i < len; ++i) {
		if (_isSep(buf[i])) {
			char c = buf[i];
			buf[i] = '\0';
			wchar_t* w = _wide(buf);
			if (w) {
				CreateDirectoryW(w, NULL);
			}
			free(w);
			buf[i] = c;
		}
	}
	wchar_t* w = _wide(buf);
	if (w) {
		CreateDirectoryW(w, NULL);
	}
	free(w);
	free(buf);
	return PlatIsDir(path);
}

bool PlatReplace(const char* from, const char* to) {
	wchar_t* wf = _wide(from);
	wchar_t* wt = _wide(to);
	bool ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
	free(wf);
	free(wt);
	return ok;
}

bool PlatRemove(const char* path) {
	wchar_t* w = _wide(path);
	bool ok = w && DeleteFileW(w);
	free(w);
	return ok;
}

bool PlatListDir(const char* dir, bool (*cb)(const char* name, void* user), void* user) {
	size_t len = strlen(dir);
	char* pattern = malloc(len + 3);
	memcpy(pattern, dir, len);
	if (len && !_isSep(dir[len - 1])) {
		pattern[len++] = '\\';
	}
	pattern[len++] = '*';
	pattern[len] = '\0';
	wchar_t* w = _wide(pattern);
	free(pattern);
	WIN32_FIND_DATAW find;
	HANDLE h = w ? FindFirstFileW(w, &find) : INVALID_HANDLE_VALUE;
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	do {
		if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0) {
			continue;
		}
		char* name = _utf8(find.cFileName);
		if (!name) {
			continue;
		}
		bool more = cb(name, user);
		free(name);
		if (!more) {
			break;
		}
	} while (FindNextFileW(h, &find));
	FindClose(h);
	return true;
}

int PlatCpuCount(void) {
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	return info.dwNumberOfProcessors > 0 ? (int) info.dwNumberOfProcessors : 1;
}

bool PlatStderrIsTty(void) {
	return _isatty(_fileno(stderr));
}

void PlatConsoleInit(void) {
	// So file names and titles in Japanese come out right in a console that
	// has a font for them
	SetConsoleOutputCP(CP_UTF8);
}

bool PlatConsoleIsOwned(void) {
	DWORD pids[2];
	return GetConsoleProcessList(pids, 2) == 1;
}

#else

char** PlatArgs(int argc, char** argv, int* outArgc) {
	char** out = malloc((argc + 1) * sizeof(*out));
	int i;
	for (i = 0; i < argc; ++i) {
		out[i] = strdup(argv[i]);
	}
	out[argc] = NULL;
	*outArgc = argc;
	return out;
}

FILE* PlatFOpen(const char* path, const char* mode) {
	return fopen(path, mode);
}

bool PlatExists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

bool PlatIsDir(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool PlatIsDevice(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode);
}

bool PlatMakeDirs(const char* path) {
	if (PlatIsDir(path)) {
		return true;
	}
	char* buf = strdup(path);
	size_t len = strlen(buf);
	size_t i;
	for (i = 1; i < len; ++i) {
		if (buf[i] == '/') {
			buf[i] = '\0';
			mkdir(buf, 0777);
			buf[i] = '/';
		}
	}
	mkdir(buf, 0777);
	free(buf);
	return PlatIsDir(path);
}

bool PlatReplace(const char* from, const char* to) {
	return rename(from, to) == 0;
}

bool PlatRemove(const char* path) {
	return unlink(path) == 0;
}

bool PlatListDir(const char* dir, bool (*cb)(const char* name, void* user), void* user) {
	DIR* d = opendir(dir);
	if (!d) {
		return false;
	}
	struct dirent* ent;
	while ((ent = readdir(d))) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}
		if (!cb(ent->d_name, user)) {
			break;
		}
	}
	closedir(d);
	return true;
}

int PlatCpuCount(void) {
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int) n : 1;
}

bool PlatStderrIsTty(void) {
	return isatty(STDERR_FILENO);
}

void PlatConsoleInit(void) {}

bool PlatConsoleIsOwned(void) {
	return false;
}

#endif

void PlatArgsFree(char** argv, int argc) {
	int i;
	for (i = 0; i < argc; ++i) {
		free(argv[i]);
	}
	free(argv);
}

void PlatPause(const char* prompt) {
	fprintf(stderr, "%s", prompt);
	fflush(stderr);
	int c;
	while ((c = getchar()) != '\n' && c != EOF) {
	}
}
