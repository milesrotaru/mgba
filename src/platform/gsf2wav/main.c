/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>

#include "blmix.h"
#include "psf.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_RATE 48000
#define DEFAULT_LENGTH 150.0
#define DEFAULT_FADE 10.0
#define DEFAULT_PSG_GRID 8

// Full scale of the GBA's 10-bit DAC around its bias point. A single
// DirectSound channel at 100% volume spans exactly this.
#define DAC_SCALE 512.0

enum SampleFormat {
	FORMAT_F32,
	FORMAT_S24,
	FORMAT_S16,
};

struct Options {
	const char* input;
	const char* output;
	const char* bios;
	unsigned rate;
	enum SampleFormat format;
	double length;
	double fade;
	double gainDb;
	bool useVolumeTag;
	bool fifoHold;
	unsigned psgGrid;
};

// Receives the raw DAC inputs from the core, bypassing the core's own
// resolution-limited sampling, bias clamp and output resampler.
struct Capture {
	struct GBAAudioObserver d;
	struct BLMixer* mixer;
	struct mTiming* timing;
	bool fifoHold;
	unsigned psgGrid;

	uint64_t psgLast;
	double psgL;
	double psgR;

	int8_t fifoValue[2];
	double fifoLevelL[2];
	double fifoLevelR[2];
	uint64_t fifoLast[2];
	double fifoPeriod[2];
	uint64_t fifoCount[2];
	double fifoLongGap[2];
	int fifoLongRun[2];
};

static void _fifoGains(struct GBAAudio* audio, int fifo, double* left, double* right) {
	bool enableL, enableR, full, forceOff;
	if (fifo == 0) {
		enableL = audio->chALeft;
		enableR = audio->chARight;
		full = audio->volumeChA;
		forceOff = audio->forceDisableChA;
	} else {
		enableL = audio->chBLeft;
		enableR = audio->chBRight;
		full = audio->volumeChB;
		forceOff = audio->forceDisableChB;
	}
	// Matches the core's (sample << 2) >> !volume, without the truncation
	double gain = forceOff ? 0 : (full ? 4 : 2) / DAC_SCALE;
	*left = enableL ? gain : 0;
	*right = enableR ? gain : 0;
}

static void _captureSync(struct GBAAudioObserver* observer, struct GBAAudio* audio, int32_t timestamp) {
	struct Capture* cap = (struct Capture*) observer;
	int32_t current = mTimingCurrentTime(cap->timing);
	uint64_t now = mTimingGlobalTime(cap->timing) + (timestamp - current);

	// The PSG runs lazily inside the core, so it can be stepped through any
	// interval in which no register changed. This observer is called before
	// every such change, so walking the grid here sees every level the PSG
	// actually produced, to within one grid step.
	uint64_t t;
	for (t = cap->psgLast + cap->psgGrid; t <= now; t += cap->psgGrid) {
		GBAudioRun(&audio->psg, timestamp - (int32_t) (now - t), 0xF);
		int16_t l = 0;
		int16_t r = 0;
		GBAudioSamplePSG(&audio->psg, &l, &r);
		double scale = 1.0 / (1 << (4 - audio->volume)) / DAC_SCALE;
		double sl = l * scale;
		double sr = r * scale;
		if (sl != cap->psgL || sr != cap->psgR) {
			// The edge happened somewhere in the last grid step; its midpoint is
			// the unbiased estimate
			BLMixerStep(cap->mixer, t - cap->psgGrid * 0.5, sl - cap->psgL, sr - cap->psgR);
			cap->psgL = sl;
			cap->psgR = sr;
		}
		cap->psgLast = t;
	}

	if (cap->fifoHold) {
		// Mixer control changes act immediately on held FIFO levels
		int fifo;
		for (fifo = 0; fifo < 2; ++fifo) {
			double gl, gr;
			_fifoGains(audio, fifo, &gl, &gr);
			double l = cap->fifoValue[fifo] * gl;
			double r = cap->fifoValue[fifo] * gr;
			if (l != cap->fifoLevelL[fifo] || r != cap->fifoLevelR[fifo]) {
				BLMixerStep(cap->mixer, now, l - cap->fifoLevelL[fifo], r - cap->fifoLevelR[fifo]);
				cap->fifoLevelL[fifo] = l;
				cap->fifoLevelR[fifo] = r;
			}
		}
	}
}

static void _captureFifo(struct GBAAudioObserver* observer, struct GBAAudio* audio, int fifo, uint64_t when, int8_t sample) {
	struct Capture* cap = (struct Capture*) observer;
	double gl, gr;
	_fifoGains(audio, fifo, &gl, &gr);
	cap->fifoValue[fifo] = sample;

	// Track the stream's sample period. A gap much longer than the last
	// period means the timer stopped and restarted, so keep the old period
	// rather than treating the silence as one very long sample. A shorter one
	// means the rate went up, which is taken as-is; a longer one is only
	// believed once it repeats.
	double period = cap->fifoPeriod[fifo];
	if (cap->fifoCount[fifo]) {
		double dt = (double) (when - cap->fifoLast[fifo]);
		if (dt > 0 && (period == 0 || dt < period * 1.5)) {
			period = dt;
			cap->fifoLongRun[fifo] = 0;
		} else if (dt > 0) {
			if (fabs(dt - cap->fifoLongGap[fifo]) < dt * 0.01) {
				++cap->fifoLongRun[fifo];
			} else {
				cap->fifoLongGap[fifo] = dt;
				cap->fifoLongRun[fifo] = 1;
			}
			if (cap->fifoLongRun[fifo] >= 3) {
				period = dt;
			}
		}
	}
	cap->fifoLast[fifo] = when;
	++cap->fifoCount[fifo];
	if (period <= 0) {
		// First sample of a stream: nothing to measure yet
		period = GBA_ARM7TDMI_FREQUENCY / 16384.0;
	}
	cap->fifoPeriod[fifo] = period;

	if (cap->fifoHold) {
		double l = sample * gl;
		double r = sample * gr;
		BLMixerStep(cap->mixer, when, l - cap->fifoLevelL[fifo], r - cap->fifoLevelR[fifo]);
		cap->fifoLevelL[fifo] = l;
		cap->fifoLevelR[fifo] = r;
	} else {
		// The DAC holds each sample for one period, which delays the stream by
		// half a period relative to the PSG. Center the point to keep them
		// aligned.
		BLMixerPoint(cap->mixer, when + period * 0.5, period, sample * gl, sample * gr);
	}
}

struct WavWriter {
	FILE* f;
	enum SampleFormat format;
	unsigned rate;
	uint64_t frames;
	uint32_t rng;
};

static void _put16(uint8_t* b, uint16_t v) {
	b[0] = v;
	b[1] = v >> 8;
}

static void _put32(uint8_t* b, uint32_t v) {
	b[0] = v;
	b[1] = v >> 8;
	b[2] = v >> 16;
	b[3] = v >> 24;
}

static unsigned _bytesPerSample(enum SampleFormat format) {
	switch (format) {
	case FORMAT_F32:
		return 4;
	case FORMAT_S24:
		return 3;
	case FORMAT_S16:
	default:
		return 2;
	}
}

static bool _wavHeader(struct WavWriter* w) {
	uint8_t h[58];
	unsigned bps = _bytesPerSample(w->format);
	bool isFloat = w->format == FORMAT_F32;
	uint32_t fmtSize = isFloat ? 18 : 16;
	uint64_t dataSize = w->frames * 2 * bps;
	if (dataSize > 0xFFFFFF00ULL) {
		dataSize = 0xFFFFFF00ULL;
	}
	size_t headerSize = 12 + 8 + fmtSize + (isFloat ? 12 : 0) + 8;
	memcpy(h, "RIFF", 4);
	_put32(&h[4], (uint32_t) (headerSize - 8 + dataSize));
	memcpy(&h[8], "WAVE", 4);
	memcpy(&h[12], "fmt ", 4);
	_put32(&h[16], fmtSize);
	_put16(&h[20], isFloat ? 3 : 1);
	_put16(&h[22], 2);
	_put32(&h[24], w->rate);
	_put32(&h[28], w->rate * 2 * bps);
	_put16(&h[32], 2 * bps);
	_put16(&h[34], bps * 8);
	size_t o = 36;
	if (isFloat) {
		_put16(&h[o], 0);
		o += 2;
		memcpy(&h[o], "fact", 4);
		_put32(&h[o + 4], 4);
		_put32(&h[o + 8], (uint32_t) w->frames);
		o += 12;
	}
	memcpy(&h[o], "data", 4);
	_put32(&h[o + 4], (uint32_t) dataSize);
	o += 8;
	return fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, o, w->f) == o;
}

static double _tpdf(struct WavWriter* w) {
	// Two uniform variables in [0, 1) summed gives triangular noise of +/-1 LSB
	w->rng = w->rng * 1664525 + 1013904223;
	double a = (w->rng >> 8) / 16777216.0;
	w->rng = w->rng * 1664525 + 1013904223;
	double b = (w->rng >> 8) / 16777216.0;
	return a - b;
}

static bool _wavWrite(struct WavWriter* w, const double* samples, size_t frames) {
	uint8_t buf[4096 * 2 * 4];
	size_t done = 0;
	while (done < frames) {
		size_t chunk = frames - done;
		if (chunk > 4096) {
			chunk = 4096;
		}
		size_t i;
		size_t o = 0;
		for (i = 0; i < chunk * 2; ++i) {
			double s = samples[done * 2 + i];
			switch (w->format) {
			case FORMAT_F32: {
				float f = (float) s;
				uint32_t bits;
				memcpy(&bits, &f, 4);
				_put32(&buf[o], bits);
				o += 4;
				break;
			}
			case FORMAT_S24: {
				double v = floor(s * 8388608.0 + _tpdf(w) + 0.5);
				int32_t q = v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : (int32_t) v;
				buf[o] = q;
				buf[o + 1] = q >> 8;
				buf[o + 2] = q >> 16;
				o += 3;
				break;
			}
			case FORMAT_S16: {
				double v = floor(s * 32768.0 + _tpdf(w) + 0.5);
				int32_t q = v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t) v;
				_put16(&buf[o], (uint16_t) q);
				o += 2;
				break;
			}
			}
		}
		if (fwrite(buf, 1, o, w->f) != o) {
			return false;
		}
		w->frames += chunk;
		done += chunk;
	}
	return true;
}

static void _nullLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

static struct mLogger _logger = { .log = _nullLog };

static void _usage(const char* arg0) {
	fprintf(stderr,
		"usage: %s [options] INPUT.minigsf OUTPUT.wav\n"
		"\n"
		"  -r, --rate HZ         output sample rate (default %u)\n"
		"  -b, --bits FMT        32f, 24 or 16 (default 32f; integer formats are TPDF dithered)\n"
		"  -l, --length TIME     play length before fade, overrides the length tag\n"
		"  -f, --fade TIME       fade length, overrides the fade tag\n"
		"  -g, --gain DB         extra gain in dB\n"
		"      --no-volume-tag   ignore the volume tag\n"
		"      --fifo-hold       reconstruct DirectSound as the hardware DAC does (sample-and-hold)\n"
		"                        instead of sinc-interpolating it at its own sample rate\n"
		"      --psg-grid CYCLES PSG sampling grid in CPU cycles (default %u)\n"
		"      --bios FILE       use a real GBA BIOS instead of the built-in HLE one\n"
		"\n"
		"TIME is seconds or [h:]m:ss[.fff]. Without tags, length defaults to %g s and fade to %g s.\n",
		arg0, DEFAULT_RATE, DEFAULT_PSG_GRID, DEFAULT_LENGTH, DEFAULT_FADE);
}

static bool _parseArgs(int argc, char** argv, struct Options* opts) {
	enum {
		OPT_NO_VOLUME_TAG = 0x100,
		OPT_FIFO_HOLD,
		OPT_PSG_GRID,
		OPT_BIOS,
	};
	static const struct option longOpts[] = {
		{ "rate", required_argument, NULL, 'r' },
		{ "bits", required_argument, NULL, 'b' },
		{ "length", required_argument, NULL, 'l' },
		{ "fade", required_argument, NULL, 'f' },
		{ "gain", required_argument, NULL, 'g' },
		{ "no-volume-tag", no_argument, NULL, OPT_NO_VOLUME_TAG },
		{ "fifo-hold", no_argument, NULL, OPT_FIFO_HOLD },
		{ "psg-grid", required_argument, NULL, OPT_PSG_GRID },
		{ "bios", required_argument, NULL, OPT_BIOS },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	memset(opts, 0, sizeof(*opts));
	opts->rate = DEFAULT_RATE;
	opts->format = FORMAT_F32;
	opts->length = -1;
	opts->fade = -1;
	opts->useVolumeTag = true;
	opts->psgGrid = DEFAULT_PSG_GRID;
	int c;
	while ((c = getopt_long(argc, argv, "r:b:l:f:g:h", longOpts, NULL)) != -1) {
		switch (c) {
		case 'r':
			opts->rate = strtoul(optarg, NULL, 10);
			if (opts->rate < 8000 || opts->rate > 768000) {
				fprintf(stderr, "Sample rate must be between 8000 and 768000\n");
				return false;
			}
			break;
		case 'b':
			if (strcmp(optarg, "32f") == 0 || strcmp(optarg, "32") == 0) {
				opts->format = FORMAT_F32;
			} else if (strcmp(optarg, "24") == 0) {
				opts->format = FORMAT_S24;
			} else if (strcmp(optarg, "16") == 0) {
				opts->format = FORMAT_S16;
			} else {
				fprintf(stderr, "Unknown sample format: %s\n", optarg);
				return false;
			}
			break;
		case 'l':
			opts->length = PSFParseTime(optarg);
			if (opts->length < 0) {
				fprintf(stderr, "Bad length: %s\n", optarg);
				return false;
			}
			break;
		case 'f':
			opts->fade = PSFParseTime(optarg);
			if (opts->fade < 0) {
				fprintf(stderr, "Bad fade: %s\n", optarg);
				return false;
			}
			break;
		case 'g':
			opts->gainDb = strtod(optarg, NULL);
			break;
		case OPT_NO_VOLUME_TAG:
			opts->useVolumeTag = false;
			break;
		case OPT_FIFO_HOLD:
			opts->fifoHold = true;
			break;
		case OPT_PSG_GRID:
			opts->psgGrid = strtoul(optarg, NULL, 10);
			if (opts->psgGrid < 1 || opts->psgGrid > 1024) {
				fprintf(stderr, "PSG grid must be between 1 and 1024 cycles\n");
				return false;
			}
			break;
		case OPT_BIOS:
			opts->bios = optarg;
			break;
		default:
			return false;
		}
	}
	if (argc - optind != 2) {
		return false;
	}
	opts->input = argv[optind];
	opts->output = argv[optind + 1];
	return true;
}

int main(int argc, char** argv) {
	struct Options opts;
	if (!_parseArgs(argc, argv, &opts)) {
		_usage(argv[0]);
		return 1;
	}
	mLogSetDefaultLogger(&_logger);

	struct GSFImage image;
	struct PSFTags tags;
	char err[512];
	if (!GSFLoad(opts.input, &image, &tags, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}

	double length = opts.length;
	if (length < 0) {
		length = PSFParseTime(PSFTagGet(&tags, "length"));
	}
	if (length < 0) {
		length = DEFAULT_LENGTH;
	}
	double fade = opts.fade;
	if (fade < 0) {
		fade = PSFParseTime(PSFTagGet(&tags, "fade"));
	}
	if (fade < 0) {
		fade = opts.length >= 0 || PSFTagGet(&tags, "length") ? 0 : DEFAULT_FADE;
	}
	double gain = pow(10, opts.gainDb / 20);
	const char* volumeTag = PSFTagGet(&tags, "volume");
	if (opts.useVolumeTag && volumeTag) {
		double v = strtod(volumeTag, NULL);
		if (v > 0) {
			gain *= v;
		}
	}
	const char* title = PSFTagGet(&tags, "title");
	const char* game = PSFTagGet(&tags, "game");
	fprintf(stderr, "%s%s%s\n", game ? game : "", game && title ? " - " : "", title ? title : opts.input);

	bool multiboot = (image.entry >> 24) == 0x02;
	struct VFile* vf = VFileFromConstMemory(image.data, image.size);
	struct mCore* core = mCoreCreate(mPLATFORM_GBA);
	if (!vf || !core) {
		fprintf(stderr, "Could not create GBA core\n");
		return 1;
	}
	core->init(core);
	mCoreInitConfig(core, NULL);
	struct mCoreOptions coreOpts = {
		.skipBios = true,
		.useBios = opts.bios != NULL,
		.volume = 0x100,
	};
	mCoreConfigLoadDefaults(&core->config, &coreOpts);
	core->loadConfig(core, &core->config);

	struct GBA* gba = core->board;
	// Don't let the core guess cartridge vs. multiboot from the image; the GSF
	// entry point already says which it is
	if (multiboot ? !GBALoadMB(gba, vf) : !GBALoadROM(gba, vf)) {
		fprintf(stderr, "Could not load program image\n");
		return 1;
	}
	if (opts.bios) {
		struct VFile* bios = VFileOpen(opts.bios, O_RDONLY);
		if (!bios) {
			fprintf(stderr, "Could not open BIOS %s\n", opts.bios);
			return 1;
		}
		core->loadBIOS(core, bios, 0);
	}
	core->reset(core);
	if (multiboot) {
		// Reset re-copies a multiboot image only if its heuristic recognizes it
		size_t size = image.size < GBA_SIZE_EWRAM ? image.size : GBA_SIZE_EWRAM;
		memcpy(gba->memory.wram, image.data, size);
	}
	if (image.entry >> 24 == 0x02 || image.entry >> 24 == 0x08) {
		gba->cpu->gprs[ARM_PC] = image.entry;
		ARMWritePC(gba->cpu);
	}

	struct BLMixer mixer;
	BLMixerInit(&mixer, GBA_ARM7TDMI_FREQUENCY, opts.rate);
	struct Capture capture = {
		.d = { .sync = _captureSync, .fifoSample = _captureFifo },
		.mixer = &mixer,
		.timing = &gba->timing,
		.fifoHold = opts.fifoHold,
		.psgGrid = opts.psgGrid,
	};
	capture.psgLast = mTimingGlobalTime(&gba->timing);
	gba->audio.observer = &capture.d;

	struct WavWriter wav = {
		.f = fopen(opts.output, "wb"),
		.format = opts.format,
		.rate = opts.rate,
		.rng = 0x2545F491,
	};
	if (!wav.f || !_wavHeader(&wav)) {
		fprintf(stderr, "Could not open %s for writing: %s\n", opts.output, strerror(errno));
		return 1;
	}

	uint64_t fadeStart = (uint64_t) llround(length * opts.rate);
	uint64_t fadeFrames = (uint64_t) llround(fade * opts.rate);
	uint64_t total = fadeStart + fadeFrames;
	uint64_t produced = 0;
	double peak = 0;
	size_t bufFrames = 1 << 16;
	double* buf = malloc(bufFrames * 2 * sizeof(double));
	int lastPercent = -1;
	bool showProgress = isatty(STDERR_FILENO);
	bool ok = true;

	while (produced < total) {
		core->runFrame(core);
		// Flush the PSG up to now before reading out
		_captureSync(&capture.d, &gba->audio, mTimingCurrentTime(&gba->timing));
		size_t n;
		while ((n = BLMixerRead(&mixer, (double) mTimingGlobalTime(&gba->timing), buf, bufFrames)) > 0) {
			if (n > total - produced) {
				n = total - produced;
			}
			size_t i;
			for (i = 0; i < n; ++i) {
				uint64_t pos = produced + i;
				double g = gain;
				if (pos >= fadeStart && fadeFrames) {
					g *= 1.0 - (double) (pos - fadeStart) / fadeFrames;
				}
				buf[i * 2] *= g;
				buf[i * 2 + 1] *= g;
				if (fabs(buf[i * 2]) > peak) {
					peak = fabs(buf[i * 2]);
				}
				if (fabs(buf[i * 2 + 1]) > peak) {
					peak = fabs(buf[i * 2 + 1]);
				}
			}
			if (!_wavWrite(&wav, buf, n)) {
				fprintf(stderr, "Write error\n");
				ok = false;
				break;
			}
			produced += n;
			if (produced >= total) {
				break;
			}
		}
		if (!ok) {
			break;
		}
		int percent = (int) (produced * 100 / (total ? total : 1));
		if (showProgress && percent != lastPercent) {
			fprintf(stderr, "\r%3d%%", percent);
			lastPercent = percent;
		}
	}
	if (showProgress) {
		fprintf(stderr, "\n");
	}

	if (ok && !_wavHeader(&wav)) {
		fprintf(stderr, "Could not finalize WAV header\n");
		ok = false;
	}
	fclose(wav.f);

	int fifo;
	for (fifo = 0; fifo < 2; ++fifo) {
		if (capture.fifoCount[fifo]) {
			fprintf(stderr, "DirectSound %c: %.2f Hz\n", 'A' + fifo, GBA_ARM7TDMI_FREQUENCY / capture.fifoPeriod[fifo]);
		}
	}
	fprintf(stderr, "Peak: %.2f dBFS%s\n", 20 * log10(peak > 0 ? peak : 1e-12),
	        peak > 1 && opts.format != FORMAT_F32 ? " (clipped; lower the gain or use -b 32f)" : "");
	if (mixer.lateEvents) {
		fprintf(stderr, "Warning: %llu events arrived after their output was finalized\n", (unsigned long long) mixer.lateEvents);
	}

	free(buf);
	gba->audio.observer = NULL;
	BLMixerDeinit(&mixer);
	core->unloadROM(core);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	GSFImageDeinit(&image);
	PSFTagsDeinit(&tags);
	return ok ? 0 : 1;
}
