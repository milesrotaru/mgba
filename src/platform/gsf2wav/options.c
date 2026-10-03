/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "options.h"

#include "mp2k.h"
#include "platform.h"
#include "psf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum OptionId {
	OPT_OUTPUT,
	OPT_RATE,
	OPT_BITS,
	OPT_LENGTH,
	OPT_FADE,
	OPT_GAIN,
	OPT_NO_VOLUME_TAG,
	OPT_NO_HIFI,
	OPT_MIX,
	OPT_BANDWIDTH,
	OPT_SOURCE_CUTOFF,
	OPT_LINEAR_SAMPLES,
	OPT_RAMP,
	OPT_FIFO_HOLD,
	OPT_PSG_GRID,
	OPT_BIOS,
	OPT_SAMPLE_ROM,
	OPT_NO_FILL_HOLES,
	OPT_OVERRIDES,
	OPT_SKIP_EXISTING,
	OPT_JOBS,
	OPT_QUIET,
	OPT_VERBOSE,
	OPT_MUTE,
	OPT_SOLO_SAMPLE,
	OPT_SAMPLE_STATS,
	OPT_VERIFY,
	OPT_VERSION,
	OPT_HELP,
};

enum OptionGroup {
	GROUP_OUTPUT,
	GROUP_SOUND,
	GROUP_SAMPLES,
	GROUP_BATCH,
	GROUP_DIAGNOSTICS,
	GROUP_OTHER,
	GROUP_COUNT,
};

static const char* const GROUP_NAMES[GROUP_COUNT] = {
	"Output",
	"Sound",
	"Samples (rips are missing bytes of the game's sample data)",
	"Batches",
	"Diagnostics",
	"Other",
};

struct OptionSpec {
	enum OptionId id;
	enum OptionGroup group;
	char shortName;
	const char* longName;
	const char* alias; // an older name for the same option, not listed in --help
	const char* argName; // NULL: takes no argument
	bool perTrack; // allowed in an overrides file
	const char* help; // '\n' continues on an aligned line
};

static const struct OptionSpec OPTIONS[] = {
	{ OPT_OUTPUT, GROUP_OUTPUT, 'o', "output", NULL, "PATH", false,
	  "folder for the WAVs (made if missing), or the file name for a single input" },
	{ OPT_RATE, GROUP_OUTPUT, 'r', "rate", NULL, "HZ", true, "sample rate (default 48000)" },
	{ OPT_BITS, GROUP_OUTPUT, 'b', "bits", NULL, "FMT", true, "32f (float, the default), 24 or 16; integer formats get TPDF dither" },
	{ OPT_LENGTH, GROUP_OUTPUT, 'l', "length", NULL, "TIME", true, "play time before the fade (default: the file's length tag, else 150 s)" },
	{ OPT_FADE, GROUP_OUTPUT, 'f', "fade", NULL, "TIME", true, "fade-out time (default: the file's fade tag, else 10 s)" },
	{ OPT_GAIN, GROUP_OUTPUT, 'g', "gain", NULL, "DB", true, "extra gain in dB; negative turns it down" },
	{ OPT_NO_VOLUME_TAG, GROUP_OUTPUT, 0, "no-volume-tag", NULL, NULL, true, "ignore the file's volume tag" },

	{ OPT_NO_HIFI, GROUP_SOUND, 0, "no-hifi", NULL, NULL, true,
	  "play the game's own mix instead of re-rendering its voices" },
	{ OPT_MIX, GROUP_SOUND, 0, "mix", "mp2k-mix", "MODE", true,
	  "how each voice is resampled when re-rendering:\n"
	  "sinc (default): from its source straight to the output rate\n"
	  "linear: the driver's own method, at its mixing rate, without its 8-bit loss\n"
	  "lerp: linear interpolation at the output rate, not bandlimited\n"
	  "blam: linear interpolation, bandlimited to the output rate" },
	{ OPT_BANDWIDTH, GROUP_SOUND, 0, "bandwidth", "mp2k-bandwidth", "HZ", true,
	  "cap each voice's bandwidth; \"driver\" means the driver's own Nyquist" },
	{ OPT_SOURCE_CUTOFF, GROUP_SOUND, 0, "source-cutoff", "mp2k-source-cutoff", "F", true,
	  "each voice's cutoff as a fraction of its own playback rate (default 0.47)" },
	{ OPT_LINEAR_SAMPLES, GROUP_SOUND, 0, "linear-samples", "mp2k-linear-samples", "LIST", true,
	  "render these samples (hex header addresses, comma-separated) the way the\n"
	  "driver does, whatever --mix says" },
	{ OPT_RAMP, GROUP_SOUND, 0, "ramp", NULL, "MS", true, "volume change and note cut smoothing (default 2; 0 = the driver's steps)" },
	{ OPT_FIFO_HOLD, GROUP_SOUND, 0, "fifo-hold", NULL, NULL, true,
	  "play DirectSound as the hardware's sample-and-hold instead of\nsinc-interpolating it" },
	{ OPT_PSG_GRID, GROUP_SOUND, 0, "psg-grid", NULL, "CYCLES", true, "PSG sampling grid in CPU cycles (default 8, already exact)" },
	{ OPT_BIOS, GROUP_SOUND, 0, "bios", NULL, "FILE", true, "use a real GBA BIOS instead of mGBA's built-in one" },

	{ OPT_SAMPLE_ROM, GROUP_SAMPLES, 0, "sample-rom", NULL, "FILE", true,
	  "the full game ROM the rip came from: sample data is read from it, which\n"
	  "recovers the bytes the ripper never saw (the game still runs from the rip)" },
	{ OPT_NO_FILL_HOLES, GROUP_SAMPLES, 0, "no-fill-holes", NULL, NULL, true,
	  "read samples exactly as the rip has them; normally zero bytes (never read\n"
	  "while ripping) are filled in from their neighbours" },

	{ OPT_OVERRIDES, GROUP_BATCH, 0, "overrides", NULL, "FILE", false,
	  "per-track options: one line per track, \"name options...\" (see the README)" },
	{ OPT_SKIP_EXISTING, GROUP_BATCH, 0, "skip-existing", NULL, NULL, false, "don't render tracks whose WAV already exists" },
	{ OPT_JOBS, GROUP_BATCH, 'j', "jobs", NULL, "N", false, "render N tracks at once (default: one per core, at most 8)" },
	{ OPT_QUIET, GROUP_BATCH, 'q', "quiet", NULL, NULL, false, "only warnings and errors" },
	{ OPT_VERBOSE, GROUP_BATCH, 'v', "verbose", NULL, NULL, false, "also print driver, timing and sample details" },

	{ OPT_MUTE, GROUP_DIAGNOSTICS, 0, "mute", NULL, "LIST", true, "silence psg, pcm, or driver channels 0-11, e.g. psg,0,3" },
	{ OPT_SOLO_SAMPLE, GROUP_DIAGNOSTICS, 0, "solo-sample", NULL, "ADDR", true,
	  "only voices playing the sample at hex header address ADDR (mutes the PSG)" },
	{ OPT_SAMPLE_STATS, GROUP_DIAGNOSTICS, 0, "sample-stats", NULL, NULL, true,
	  "print which samples played: notes, seconds, mean and max playback rate, max gain" },
	{ OPT_VERIFY, GROUP_DIAGNOSTICS, 0, "verify", "mp2k-verify", NULL, true,
	  "check the driver port against the game's own mixer, every frame" },

	{ OPT_VERSION, GROUP_OTHER, 0, "version", NULL, NULL, false, "print the version and where the source is" },
	{ OPT_HELP, GROUP_OTHER, 'h', "help", NULL, NULL, false, "this text" },
};

#define OPTION_COUNT (sizeof(OPTIONS) / sizeof(*OPTIONS))

void RenderOptionsInit(struct RenderOptions* opts) {
	memset(opts, 0, sizeof(*opts));
	opts->rate = DEFAULT_RATE;
	opts->format = FORMAT_F32;
	opts->length = -1;
	opts->fade = -1;
	opts->useVolumeTag = true;
	opts->psgGrid = DEFAULT_PSG_GRID;
	opts->hifi = true;
	opts->rampMs = DEFAULT_RAMP_MS;
}

void RenderOptionsCopy(struct RenderOptions* dst, const struct RenderOptions* src) {
	*dst = *src;
	dst->bios = src->bios ? strdup(src->bios) : NULL;
	dst->sampleRom = src->sampleRom ? strdup(src->sampleRom) : NULL;
}

void RenderOptionsDeinit(struct RenderOptions* opts) {
	free(opts->bios);
	free(opts->sampleRom);
	opts->bios = NULL;
	opts->sampleRom = NULL;
}

#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
static void _fail(char* err, size_t errLen, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(err, errLen, fmt, args);
	va_end(args);
}

static bool _number(const char* s, double* out) {
	if (!*s) {
		return false;
	}
	char* end;
	errno = 0;
	double v = strtod(s, &end);
	if (*end || errno || v != v) {
		return false;
	}
	*out = v;
	return true;
}

// Comma-separated tokens; each is handed to fn. Doesn't modify its input.
static bool _forEachToken(const char* list, bool (*fn)(const char* token, void* user), void* user) {
	char* copy = strdup(list);
	char* p = copy;
	bool ok = true;
	while (ok) {
		char* comma = strchr(p, ',');
		if (comma) {
			*comma = '\0';
		}
		ok = fn(p, user);
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
	free(copy);
	return ok;
}

static bool _muteToken(const char* tok, void* user) {
	struct RenderOptions* r = user;
	char* end;
	long ch = strtol(tok, &end, 10);
	if (strcmp(tok, "psg") == 0) {
		r->mutePsg = true;
	} else if (strcmp(tok, "fifo") == 0 || strcmp(tok, "pcm") == 0) {
		r->muteFifo = true;
		r->muteChannels = 0xFFFFFFFF;
	} else if (*tok && !*end && ch >= 0 && ch < MP2K_MAX_CHANNELS) {
		r->muteChannels |= 1u << ch;
	} else {
		return false;
	}
	return true;
}

static bool _linearToken(const char* tok, void* user) {
	struct RenderOptions* r = user;
	char* end;
	unsigned long addr = strtoul(tok, &end, 16);
	if (!*tok || *end || r->linearWavCount == MAX_LINEAR_SAMPLES) {
		return false;
	}
	r->linearWavs[r->linearWavCount++] = addr;
	return true;
}

static void _replace(char** field, const char* value) {
	free(*field);
	*field = strdup(value);
}

static bool _apply(const struct OptionSpec* spec, const char* name, const char* value, struct RenderOptions* r,
                   struct RunOptions* run, char* err, size_t errLen) {
	double v;
	switch (spec->id) {
	case OPT_OUTPUT:
		run->output = value;
		break;
	case OPT_RATE:
		if (!_number(value, &v) || v < 8000 || v > 768000) {
			_fail(err, errLen, "%s: expected a sample rate from 8000 to 768000, got '%s'", name, value);
			return false;
		}
		r->rate = (unsigned) v;
		break;
	case OPT_BITS:
		if (strcmp(value, "32f") == 0 || strcmp(value, "32") == 0) {
			r->format = FORMAT_F32;
		} else if (strcmp(value, "24") == 0) {
			r->format = FORMAT_S24;
		} else if (strcmp(value, "16") == 0) {
			r->format = FORMAT_S16;
		} else {
			_fail(err, errLen, "%s: expected 32f, 24 or 16, got '%s'", name, value);
			return false;
		}
		break;
	case OPT_LENGTH:
		r->length = PSFParseTime(value);
		if (r->length < 0) {
			_fail(err, errLen, "%s: expected a time like 90 or 1:30.5, got '%s'", name, value);
			return false;
		}
		break;
	case OPT_FADE:
		r->fade = PSFParseTime(value);
		if (r->fade < 0) {
			_fail(err, errLen, "%s: expected a time like 10 or 0:10, got '%s'", name, value);
			return false;
		}
		break;
	case OPT_GAIN:
		if (!_number(value, &v)) {
			_fail(err, errLen, "%s: expected a number of dB, got '%s'", name, value);
			return false;
		}
		r->gainDb = v;
		break;
	case OPT_NO_VOLUME_TAG:
		r->useVolumeTag = false;
		break;
	case OPT_NO_HIFI:
		r->hifi = false;
		break;
	case OPT_MIX:
		if (strcmp(value, "sinc") == 0) {
			r->hifiMode = HIFI_MIX_SINC;
		} else if (strcmp(value, "linear") == 0) {
			r->hifiMode = HIFI_MIX_LINEAR;
		} else if (strcmp(value, "lerp") == 0) {
			r->hifiMode = HIFI_MIX_LERP;
		} else if (strcmp(value, "blam") == 0) {
			r->hifiMode = HIFI_MIX_BLAM;
		} else {
			_fail(err, errLen, "%s: expected sinc, linear, lerp or blam, got '%s'", name, value);
			return false;
		}
		break;
	case OPT_BANDWIDTH:
		if (strcmp(value, "driver") == 0) {
			r->bandwidthDriver = true;
			r->bandwidth = 0;
		} else if (_number(value, &v) && v >= 0) {
			r->bandwidth = v;
			r->bandwidthDriver = false;
		} else {
			_fail(err, errLen, "%s: expected a frequency in Hz or \"driver\", got '%s'", name, value);
			return false;
		}
		break;
	case OPT_SOURCE_CUTOFF:
		if (!_number(value, &v) || v <= 0.05 || v > 0.5) {
			_fail(err, errLen, "%s: expected a fraction above 0.05 and up to 0.5, got '%s'", name, value);
			return false;
		}
		r->sourceCutoff = v;
		break;
	case OPT_LINEAR_SAMPLES:
		if (!_forEachToken(value, _linearToken, r)) {
			_fail(err, errLen, "%s: expected hex sample addresses separated by commas (at most %d), got '%s'", name,
			      MAX_LINEAR_SAMPLES, value);
			return false;
		}
		break;
	case OPT_RAMP:
		if (!_number(value, &v) || v < 0 || v > 100) {
			_fail(err, errLen, "%s: expected milliseconds from 0 to 100, got '%s'", name, value);
			return false;
		}
		r->rampMs = v;
		break;
	case OPT_FIFO_HOLD:
		r->fifoHold = true;
		break;
	case OPT_PSG_GRID:
		if (!_number(value, &v) || v < 1 || v > 1024) {
			_fail(err, errLen, "%s: expected a number of cycles from 1 to 1024, got '%s'", name, value);
			return false;
		}
		r->psgGrid = (unsigned) v;
		break;
	case OPT_BIOS:
		_replace(&r->bios, value);
		break;
	case OPT_SAMPLE_ROM:
		_replace(&r->sampleRom, value);
		break;
	case OPT_NO_FILL_HOLES:
		r->noFillHoles = true;
		break;
	case OPT_OVERRIDES:
		run->overrides = value;
		break;
	case OPT_SKIP_EXISTING:
		run->skipExisting = true;
		break;
	case OPT_JOBS:
		if (strcmp(value, "auto") == 0) {
			run->jobs = 0;
		} else if (_number(value, &v) && v >= 1 && v <= 64) {
			run->jobs = (int) v;
		} else {
			_fail(err, errLen, "%s: expected a number from 1 to 64, or auto, got '%s'", name, value);
			return false;
		}
		break;
	case OPT_QUIET:
		run->verbosity = -1;
		break;
	case OPT_VERBOSE:
		run->verbosity = 1;
		break;
	case OPT_MUTE:
		if (!_forEachToken(value, _muteToken, r)) {
			_fail(err, errLen, "%s: expected psg, pcm or channel numbers from 0 to %d separated by commas, got '%s'", name,
			      MP2K_MAX_CHANNELS - 1, value);
			return false;
		}
		break;
	case OPT_SOLO_SAMPLE: {
		char* end;
		unsigned long addr = strtoul(value, &end, 16);
		if (!*value || *end) {
			_fail(err, errLen, "%s: expected a hex address, got '%s'", name, value);
			return false;
		}
		r->soloWav = addr;
		r->mutePsg = true;
		break;
	}
	case OPT_SAMPLE_STATS:
		r->sampleStats = true;
		break;
	case OPT_VERIFY:
		r->verify = true;
		break;
	case OPT_VERSION:
		run->version = true;
		break;
	case OPT_HELP:
		run->help = true;
		break;
	}
	return true;
}

static int _distance(const char* a, const char* b) {
	size_t la = strlen(a);
	size_t lb = strlen(b);
	int* row = malloc((lb + 1) * sizeof(*row));
	size_t i, j;
	for (j = 0; j <= lb; ++j) {
		row[j] = (int) j;
	}
	for (i = 1; i <= la; ++i) {
		int diag = row[0];
		row[0] = (int) i;
		for (j = 1; j <= lb; ++j) {
			int up = row[j];
			int cost = a[i - 1] == b[j - 1] ? 0 : 1;
			int best = diag + cost;
			if (row[j] + 1 < best) {
				best = row[j] + 1;
			}
			if (row[j - 1] + 1 < best) {
				best = row[j - 1] + 1;
			}
			row[j] = best;
			diag = up;
		}
	}
	int d = row[lb];
	free(row);
	return d;
}

static void _unknown(const char* arg, char* err, size_t errLen) {
	const char* name = arg + (arg[1] == '-' ? 2 : 1);
	char typed[64];
	snprintf(typed, sizeof(typed), "%s", name);
	char* eq = strchr(typed, '=');
	if (eq) {
		*eq = '\0';
	}
	const char* best = NULL;
	int bestDistance = 1 << 20;
	size_t i;
	for (i = 0; i < OPTION_COUNT; ++i) {
		const char* names[2] = { OPTIONS[i].longName, OPTIONS[i].alias };
		int n;
		for (n = 0; n < 2; ++n) {
			if (names[n]) {
				int d = _distance(typed, names[n]);
				if (d < bestDistance) {
					bestDistance = d;
					best = OPTIONS[i].longName;
				}
			}
		}
	}
	if (best && bestDistance <= 2 && bestDistance < (int) strlen(typed)) {
		_fail(err, errLen, "unknown option '%s' (did you mean --%s?)", arg, best);
	} else {
		_fail(err, errLen, "unknown option '%s'", arg);
	}
}

static const struct OptionSpec* _findLong(const char* name, size_t len) {
	size_t i;
	for (i = 0; i < OPTION_COUNT; ++i) {
		if ((strlen(OPTIONS[i].longName) == len && strncmp(OPTIONS[i].longName, name, len) == 0) ||
		    (OPTIONS[i].alias && strlen(OPTIONS[i].alias) == len && strncmp(OPTIONS[i].alias, name, len) == 0)) {
			return &OPTIONS[i];
		}
	}
	return NULL;
}

static const struct OptionSpec* _findShort(char c) {
	size_t i;
	for (i = 0; i < OPTION_COUNT; ++i) {
		if (OPTIONS[i].shortName == c) {
			return &OPTIONS[i];
		}
	}
	return NULL;
}

bool OptionsParse(int argc, char** argv, struct RenderOptions* render, struct RunOptions* run, char** positionals,
                  int* positionalCount, char* err, size_t errLen) {
	int npos = 0;
	bool onlyPositionals = false;
	int i;
	for (i = 0; i < argc; ++i) {
		const char* arg = argv[i];
		if (onlyPositionals || arg[0] != '-' || !arg[1]) {
			if (!positionals) {
				_fail(err, errLen, "unexpected '%s' (only options belong here)", arg);
				return false;
			}
			positionals[npos++] = argv[i];
			continue;
		}
		if (strcmp(arg, "--") == 0) {
			onlyPositionals = true;
			continue;
		}
		const struct OptionSpec* spec;
		const char* value = NULL;
		char shown[80];
		if (arg[1] == '-') {
			const char* eq = strchr(arg, '=');
			size_t len = eq ? (size_t) (eq - arg) - 2 : strlen(arg) - 2;
			spec = _findLong(arg + 2, len);
			if (!spec) {
				_unknown(arg, err, errLen);
				return false;
			}
			snprintf(shown, sizeof(shown), "--%.*s", (int) len, arg + 2);
			if (eq) {
				if (!spec->argName) {
					_fail(err, errLen, "%s doesn't take a value", shown);
					return false;
				}
				value = eq + 1;
			}
		} else {
			spec = _findShort(arg[1]);
			if (!spec) {
				_unknown(arg, err, errLen);
				return false;
			}
			snprintf(shown, sizeof(shown), "-%c", arg[1]);
			if (spec->argName && arg[2]) {
				value = arg + 2;
			} else if (arg[2]) {
				_fail(err, errLen, "unknown option '%s' (short options can't be combined)", arg);
				return false;
			}
		}
		if (!run && !spec->perTrack) {
			_fail(err, errLen, "%s can't be set per track", shown);
			return false;
		}
		if (spec->argName && !value) {
			if (i + 1 >= argc) {
				_fail(err, errLen, "%s needs a value (%s)", shown, spec->argName);
				return false;
			}
			value = argv[++i];
		}
		struct RunOptions scratch = { 0 };
		if (!_apply(spec, shown, value, render, run ? run : &scratch, err, errLen)) {
			return false;
		}
	}
	if (positionalCount) {
		*positionalCount = npos;
	}
	return true;
}

// Prints text wrapped at word boundaries to width columns, each line after
// the first indented by indent. column is where the text starts on the first.
static void _wrap(FILE* out, const char* text, int width, int indent, int* column) {
	const char* p = text;
	while (*p) {
		size_t word = strcspn(p, " ");
		if (*column > indent && *column + 1 + (int) word > width) {
			fprintf(out, "\n%*s", indent, "");
			*column = indent;
		} else if (*column > indent) {
			fputc(' ', out);
			++*column;
		}
		fprintf(out, "%.*s", (int) word, p);
		*column += (int) word;
		p += word;
		while (*p == ' ') {
			++p;
		}
	}
}

static void _printOption(FILE* out, const struct OptionSpec* spec) {
	char left[64];
	char shortPart[8] = "    ";
	if (spec->shortName) {
		snprintf(shortPart, sizeof(shortPart), "-%c, ", spec->shortName);
	}
	snprintf(left, sizeof(left), "  %s--%s%s%s", shortPart, spec->longName, spec->argName ? " " : "",
	         spec->argName ? spec->argName : "");
	const int helpColumn = 28;
	const int width = 78;
	int len = (int) strlen(left);
	fputs(left, out);
	const char* p = spec->help;
	bool first = true;
	while (*p) {
		const char* nl = strchr(p, '\n');
		size_t n = nl ? (size_t) (nl - p) : strlen(p);
		char line[256];
		snprintf(line, sizeof(line), "%.*s", (int) n, p);
		int column;
		if (first && len < helpColumn - 1) {
			fprintf(out, "%*s", helpColumn - len, "");
		} else {
			fprintf(out, "\n%*s", helpColumn, "");
		}
		column = helpColumn;
		_wrap(out, line, width, helpColumn, &column);
		first = false;
		p += n + (nl ? 1 : 0);
	}
	fputc('\n', out);
}

void OptionsUsage(FILE* out, const char* program) {
	fprintf(out,
	        "usage: %s [options] INPUT...\n"
	        "\n"
	        "Renders Game Boy Advance music rips (.minigsf / .gsf) to WAV. INPUT is a file,\n"
	        "a folder of them, or a wildcard. Each WAV goes next to its input unless -o\n"
	        "says otherwise. TIME is seconds or [h:]m:ss[.fff].\n"
	        "\n"
	        "Examples:\n"
	        "  %s \"01 Title.minigsf\"\n"
	        "  %s -o wav/ --mix blam rips/mother3/\n"
	        "  %s -b 24 -g -1 -j 4 --overrides levels.txt *.minigsf\n",
	        program, program, program, program);
	enum OptionGroup g;
	for (g = 0; g < GROUP_COUNT; ++g) {
		fprintf(out, "\n%s:\n", GROUP_NAMES[g]);
		size_t i;
		for (i = 0; i < OPTION_COUNT; ++i) {
			if (OPTIONS[i].group == g) {
				_printOption(out, &OPTIONS[i]);
			}
		}
	}
}
