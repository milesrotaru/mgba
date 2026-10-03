/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "msglog.h"
#include "options.h"
#include "overrides.h"
#include "platform.h"
#include "render.h"

#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba-util/threading.h>

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef GSF2WAV_VERSION
#define GSF2WAV_VERSION "unknown"
#endif
#define SOURCE_URL "https://github.com/milesrotaru/mgba"
#define MAX_AUTO_JOBS 8

static void _nullLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

static struct mLogger _logger = { .log = _nullLog };

struct Job {
	char* input;
	char* output;
	char* stem;
	bool skip;
	bool ok;
	double wall;
	struct RenderResult result;
	struct MsgLog log;
};

struct Batch {
	struct Job* jobs;
	size_t count;
	const struct RenderOptions* base;
	const struct Overrides* overrides;
	int verbosity;
	bool showProgress;
	bool progressShown;
	size_t toRun;
	size_t next;
	size_t finished;
	Mutex lock;
};

static char* _dup(const char* s, size_t n) {
	char* out = malloc(n + 1);
	memcpy(out, s, n);
	out[n] = '\0';
	return out;
}

static bool _hasExtension(const char* path, const char* const* extensions) {
	const char* ext = PlatExtension(path);
	for (; *extensions; ++extensions) {
		if (PlatCaseCompare(ext, *extensions) == 0) {
			return true;
		}
	}
	return false;
}

static bool _isTrackFile(const char* path) {
	static const char* const extensions[] = { ".minigsf", ".gsf", NULL };
	return _hasExtension(path, extensions);
}

// The libraries that minigsf files refer to; they aren't tracks themselves
static bool _isLibraryFile(const char* path) {
	static const char* const extensions[] = { ".gsflib", ".minigsflib", NULL };
	return _hasExtension(path, extensions);
}

struct NameList {
	char** names;
	size_t count;
};

static void _addName(struct NameList* list, char* name) {
	list->names = realloc(list->names, (list->count + 1) * sizeof(*list->names));
	list->names[list->count++] = name;
}

static bool _collectTrack(const char* name, void* user) {
	if (_isTrackFile(name)) {
		_addName(user, strdup(name));
	}
	return true;
}

static int _compareNames(const void* a, const void* b) {
	return PlatCaseCompare(*(char* const*) a, *(char* const*) b);
}

static struct Job* _addJob(struct Batch* batch, const char* input) {
	batch->jobs = realloc(batch->jobs, (batch->count + 1) * sizeof(*batch->jobs));
	struct Job* job = &batch->jobs[batch->count++];
	memset(job, 0, sizeof(*job));
	job->input = strdup(input);
	const char* base = PlatBaseName(input);
	job->stem = _dup(base, PlatExtension(base) - base);
	MsgLogInit(&job->log);
	return job;
}

// Expands the inputs: files stay, folders become the tracks in them. Problems
// are reported and counted but don't stop the rest.
static size_t _collectInputs(struct Batch* batch, char** inputs, int count) {
	size_t problems = 0;
	int i;
	for (i = 0; i < count; ++i) {
		const char* in = inputs[i];
		if (PlatIsDir(in)) {
			struct NameList list = { 0 };
			PlatListDir(in, _collectTrack, &list);
			if (!list.count) {
				fprintf(stderr, "gsf2wav: no .minigsf or .gsf files in %s\n", in);
				++problems;
				continue;
			}
			qsort(list.names, list.count, sizeof(*list.names), _compareNames);
			size_t n;
			for (n = 0; n < list.count; ++n) {
				char path[4096];
				PlatJoin(path, sizeof(path), in, list.names[n]);
				_addJob(batch, path);
				free(list.names[n]);
			}
			free(list.names);
		} else if (!PlatExists(in)) {
			fprintf(stderr, "gsf2wav: %s: no such file or folder\n", in);
			++problems;
		} else if (_isLibraryFile(in)) {
			fprintf(stderr, "gsf2wav: skipping %s: it's a library that tracks refer to; render a .minigsf instead\n", in);
		} else {
			_addJob(batch, in);
		}
	}
	return problems;
}

static void _formatTime(char* out, size_t size, double seconds) {
	int total = (int) (seconds + 0.5);
	if (total >= 3600) {
		snprintf(out, size, "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
	} else {
		snprintf(out, size, "%d:%02d", total / 60, total % 60);
	}
}

static const char* _pathNote(const struct RenderResult* r) {
	switch (r->path) {
	case RENDER_HIFI:
		return r->driver && strcmp(r->driver, "AlphaDream") == 0 ? "re-rendered, AlphaDream driver" : "re-rendered, MP2K driver";
	case RENDER_PSG_ONLY:
		return "PSG only";
	case RENDER_FALLBACK:
		return "re-render failed, game's own mix";
	case RENDER_PLAIN:
	default:
		return r->noDriver ? "unrecognised sound driver, game's own mix" : "game's own mix";
	}
}

static bool _hasProblems(const struct MsgLog* log) {
	const char* p = log->text;
	const char* end = p ? p + log->length : NULL;
	while (p && p < end) {
		if (*p - '0' == MSG_ERROR || *p - '0' == MSG_WARN) {
			return true;
		}
		const char* nl = memchr(p, '\n', end - p);
		p = nl + 1;
	}
	return false;
}

// Prints a finished job. The caller holds the batch's lock.
static void _report(struct Batch* batch, struct Job* job) {
	bool single = batch->count == 1;
	if (batch->progressShown) {
		fprintf(stderr, "\r    \r");
		batch->progressShown = false;
	}
	const char* name = single ? job->output : PlatBaseName(job->output);
	char head[512];
	if (!job->ok) {
		snprintf(head, sizeof(head), "FAILED %s", PlatBaseName(job->input));
	} else {
		char duration[32];
		_formatTime(duration, sizeof(duration), job->result.seconds);
		double peakDb = 20 * log10(job->result.peak > 1e-12 ? job->result.peak : 1e-12);
		double speed = job->wall > 0.001 ? job->result.seconds / job->wall : 0;
		snprintf(head, sizeof(head), "%s  %s  peak %.1f dBFS  %.1fx realtime  (%s)", name, duration, peakDb, speed,
		         _pathNote(&job->result));
	}
	bool problems = _hasProblems(&job->log);
	bool showHead = batch->verbosity >= 0 || problems || !job->ok;
	if (showHead) {
		if (single) {
			if (job->ok && job->result.title[0] && batch->verbosity >= 0) {
				fprintf(stderr, "%s\n", job->result.title);
			}
			fprintf(stderr, "%s%s\n", job->ok ? "Wrote " : "", head);
		} else {
			fprintf(stderr, "[%*zu/%zu] %s\n", (int) log10((double) batch->toRun) + 1, batch->finished, batch->toRun, head);
		}
	}
	enum MsgLevel max = batch->verbosity < 0 ? MSG_WARN : batch->verbosity > 0 ? MSG_DETAIL : MSG_INFO;
	MsgLogPrint(&job->log, max, "    ");
}

static void _progress(void* user, int percent) {
	struct Batch* batch = user;
	fprintf(stderr, "\r%3d%%", percent);
	batch->progressShown = true;
}

static void _runJob(struct Batch* batch, struct Job* job) {
	struct RenderOptions opts;
	RenderOptionsCopy(&opts, batch->base);
	if (batch->overrides) {
		OverridesApply(batch->overrides, job->stem, &opts);
	}
	// Write to a temporary name and rename it when the render is complete, so a
	// WAV that exists is always a whole one (--skip-existing relies on that),
	// unless the destination is a device such as /dev/null or NUL
	bool direct = PlatIsDevice(job->output);
	char* target = job->output;
	if (!direct) {
		target = malloc(strlen(job->output) + 6);
		sprintf(target, "%s.part", job->output);
	}
	MsgLogSetCurrent(&job->log);
	double start = PlatNowSeconds();
	job->ok = RenderTrack(&opts, job->input, target, batch->showProgress ? _progress : NULL, batch, &job->result);
	if (job->ok && !direct && !PlatReplace(target, job->output)) {
		MsgWrite(MSG_ERROR, "Could not move the finished file into place as %s", job->output);
		job->ok = false;
	}
	if (!job->ok && !direct) {
		PlatRemove(target);
	}
	job->wall = PlatNowSeconds() - start;
	MsgLogSetCurrent(NULL);
	if (!direct) {
		free(target);
	}
	RenderOptionsDeinit(&opts);

	MutexLock(&batch->lock);
	++batch->finished;
	_report(batch, job);
	MutexUnlock(&batch->lock);
}

static THREAD_ENTRY _worker(void* arg) {
	struct Batch* batch = arg;
	while (true) {
		MutexLock(&batch->lock);
		struct Job* job = NULL;
		while (batch->next < batch->count && !job) {
			struct Job* candidate = &batch->jobs[batch->next++];
			if (!candidate->skip) {
				job = candidate;
			}
		}
		MutexUnlock(&batch->lock);
		if (!job) {
			break;
		}
		_runJob(batch, job);
	}
	return 0;
}

static void _printVersion(void) {
	printf("gsf2wav %s, built on mGBA %s\n", GSF2WAV_VERSION, projectVersion);
	printf("Source code: %s\n", SOURCE_URL);
	printf("Licensed under the Mozilla Public License 2.0.\n");
}

static void _printWelcome(FILE* out, const char* program) {
	fprintf(out, "gsf2wav renders Game Boy Advance music rips (.minigsf / .gsf files) to WAV.\n\n");
#ifdef _WIN32
	fprintf(out,
	        "The easy way: drag one or more .minigsf files, or a folder of them, onto\n"
	        "gsf2wav.exe. Each WAV is written next to its file.\n"
	        "\n"
	        "From a terminal, there are many options:\n"
	        "\n");
#endif
	OptionsUsage(out, program);
}

static int _jobCount(const struct RunOptions* run, size_t toRun) {
	int jobs = run->jobs;
	if (jobs <= 0) {
		jobs = PlatCpuCount();
		if (jobs > MAX_AUTO_JOBS) {
			jobs = MAX_AUTO_JOBS;
		}
	}
	if ((size_t) jobs > toRun) {
		jobs = (int) toRun;
	}
	return jobs < 1 ? 1 : jobs;
}

static int _run(int argc, char** argv) {
	struct RenderOptions base;
	RenderOptionsInit(&base);
	struct RunOptions run = { 0 };
	char** positionals = malloc((argc + 1) * sizeof(*positionals));
	int npos = 0;
	char err[512];

	int i;
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "/?") == 0) {
			run.help = true;
		}
	}
	if (!OptionsParse(argc - 1, argv + 1, &base, &run, positionals, &npos, err, sizeof(err))) {
		fprintf(stderr, "gsf2wav: %s\nTry '%s --help' for the options.\n", err, PlatBaseName(argv[0]));
		free(positionals);
		RenderOptionsDeinit(&base);
		return 2;
	}
	int rc = 0;
	struct Overrides overrides = { 0 };
	struct Batch batch = { 0 };
	const char* program = "gsf2wav";
	if (run.help) {
		OptionsUsage(stdout, program);
		goto done;
	}
	if (run.version) {
		_printVersion();
		goto done;
	}
	if (!npos) {
		_printWelcome(stderr, program);
		rc = 2;
		goto done;
	}

	mLogSetDefaultLogger(&_logger);

	// The older form, "gsf2wav [options] INPUT OUTPUT.wav", still works
	const char* legacyOutput = NULL;
	if (npos == 2 && !run.output && PlatExists(positionals[0]) && !PlatIsDir(positionals[0]) && _isTrackFile(positionals[0]) &&
	    !PlatIsDir(positionals[1]) && !_isTrackFile(positionals[1])) {
		legacyOutput = positionals[1];
		npos = 1;
	}

	size_t problems = _collectInputs(&batch, positionals, npos);
	if (!batch.count) {
		rc = 1;
		goto done;
	}

	// Where each WAV goes
	bool outputIsFile = run.output && batch.count == 1 && !PlatEndsWithSeparator(run.output) && !PlatIsDir(run.output) &&
	                    *PlatExtension(run.output);
	if (run.output && !outputIsFile && !PlatMakeDirs(run.output)) {
		fprintf(stderr, "gsf2wav: could not create the folder %s\n", run.output);
		rc = 1;
		goto done;
	}
	size_t j, k;
	for (j = 0; j < batch.count; ++j) {
		struct Job* job = &batch.jobs[j];
		if (legacyOutput) {
			job->output = strdup(legacyOutput);
		} else if (outputIsFile) {
			job->output = strdup(run.output);
		} else {
			char* dir = run.output ? strdup(run.output) : PlatDirName(job->input);
			char name[4096];
			snprintf(name, sizeof(name), "%s.wav", job->stem);
			char path[4096];
			PlatJoin(path, sizeof(path), dir, name);
			free(dir);
			job->output = strdup(path);
		}
		if (outputIsFile || legacyOutput) {
			char* dir = PlatDirName(job->output);
			if (strcmp(dir, ".") != 0) {
				PlatMakeDirs(dir);
			}
			free(dir);
		}
	}
	for (j = 0; j < batch.count; ++j) {
		for (k = j + 1; k < batch.count; ++k) {
			if (PlatCaseCompare(batch.jobs[j].output, batch.jobs[k].output) == 0) {
				fprintf(stderr, "gsf2wav: %s and %s would both be written to %s; render them separately or use -o\n",
				        batch.jobs[j].input, batch.jobs[k].input, batch.jobs[j].output);
				rc = 1;
				goto done;
			}
		}
	}

	if (run.overrides && !OverridesLoad(&overrides, run.overrides, err, sizeof(err))) {
		fprintf(stderr, "gsf2wav: %s\n", err);
		rc = 1;
		goto done;
	}

	size_t skipped = 0;
	for (j = 0; j < batch.count; ++j) {
		if (run.skipExisting && PlatExists(batch.jobs[j].output) && !PlatIsDevice(batch.jobs[j].output)) {
			batch.jobs[j].skip = true;
			++skipped;
		}
	}
	batch.toRun = batch.count - skipped;
	if (skipped && run.verbosity >= 0) {
		fprintf(stderr, "Skipping %zu track%s that already %s a WAV.\n", skipped, skipped == 1 ? "" : "s",
		        skipped == 1 ? "has" : "have");
	}

	batch.base = &base;
	batch.overrides = run.overrides ? &overrides : NULL;
	batch.verbosity = run.verbosity;
	int threads = _jobCount(&run, batch.toRun);
	batch.showProgress = batch.toRun == 1 && run.verbosity >= 0 && PlatStderrIsTty();
	MutexInit(&batch.lock);
	double start = PlatNowSeconds();
	if (batch.toRun) {
		if (threads == 1) {
			_worker(&batch);
		} else {
			Thread* pool = malloc(threads * sizeof(*pool));
			int t;
			for (t = 0; t < threads; ++t) {
				ThreadCreate(&pool[t], _worker, &batch);
			}
			for (t = 0; t < threads; ++t) {
				ThreadJoin(&pool[t]);
			}
			free(pool);
		}
	}
	MutexDeinit(&batch.lock);
	double elapsed = PlatNowSeconds() - start;

	size_t failed = problems;
	size_t loud = 0;
	size_t noDriver = 0;
	for (j = 0; j < batch.count; ++j) {
		const struct Job* job = &batch.jobs[j];
		if (job->skip) {
			continue;
		}
		failed += !job->ok;
		loud += job->ok && job->result.peak > 0.891;
		noDriver += job->ok && job->result.noDriver;
	}
	if (batch.count > 1 && run.verbosity >= 0) {
		char took[32];
		_formatTime(took, sizeof(took), elapsed);
		fprintf(stderr, "\nDone: %zu rendered", batch.toRun - (failed - problems));
		if (skipped) {
			fprintf(stderr, ", %zu skipped", skipped);
		}
		if (failed) {
			fprintf(stderr, ", %zu failed", failed);
		}
		fprintf(stderr, " in %s.\n", took);
		for (j = 0; j < batch.count; ++j) {
			if (!batch.jobs[j].skip && !batch.jobs[j].ok) {
				fprintf(stderr, "  failed: %s\n", batch.jobs[j].input);
			}
		}
	}
	if (run.verbosity >= 0) {
		if (loud) {
			fprintf(stderr, "Note: %zu track%s peak%s within 1 dB of full scale. Lossy encoders (Opus, MP3, AAC) can overshoot\n"
			                "that and clip; lower the level with -g, or per track in an overrides file.\n",
			        loud, loud == 1 ? "" : "s", loud == 1 ? "s" : "");
		}
		if (noDriver) {
			fprintf(stderr, "Note: %zu track%s used a sound driver gsf2wav doesn't recognise, so %s rendered from the\n"
			                "game's own mix; the options that re-render a driver's voices had no effect.\n",
			        noDriver, noDriver == 1 ? "" : "s", noDriver == 1 ? "it was" : "they were");
		}
	}
	rc = failed ? 1 : 0;

done:
	{
		size_t n;
		for (n = 0; n < batch.count; ++n) {
			free(batch.jobs[n].input);
			free(batch.jobs[n].output);
			free(batch.jobs[n].stem);
			MsgLogDeinit(&batch.jobs[n].log);
		}
		free(batch.jobs);
	}
	OverridesDeinit(&overrides);
	RenderOptionsDeinit(&base);
	free(positionals);
	return rc;
}

int main(int argc, char** argv) {
	PlatConsoleInit();
	argv = PlatArgs(argc, argv, &argc);
	int rc = _run(argc, argv);
	PlatArgsFree(argv, argc);
	if (PlatConsoleIsOwned()) {
		// Started from Explorer: the window would vanish before anyone could
		// read what happened
		PlatPause("\nPress Enter to close this window.");
	}
	return rc;
}
