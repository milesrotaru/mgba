/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "render.h"

#include "alphadream.h"
#include "blmix.h"
#include "capture.h"
#include "hifi.h"
#include "hooks.h"
#include "mp2k.h"
#include "msglog.h"
#include "platform.h"
#include "psf.h"
#include "wav.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/vfs.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Loads the full ROM a rip was made from, for its sample data. Everything the
// rip kept must match it, apart from a few bytes a ripper patches (its
// driver hooks); anything else means a different game or revision.
static bool _loadSampleRom(const char* path, const struct GSFImage* image, uint8_t** data, size_t* size) {
	FILE* f = PlatFOpen(path, "rb");
	if (!f) {
		MsgWrite(MSG_ERROR, "Could not open the sample ROM %s", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 0x2000000) {
		fclose(f);
		MsgWrite(MSG_ERROR, "%s doesn't look like a GBA ROM", path);
		return false;
	}
	*data = malloc(len);
	*size = fread(*data, 1, len, f);
	fclose(f);
	size_t n = *size < image->size ? *size : image->size;
	size_t kept = 0;
	size_t differ = 0;
	size_t i;
	for (i = 0; i < n; ++i) {
		if (image->data[i]) {
			++kept;
			differ += image->data[i] != (*data)[i];
		}
	}
	if (!kept || differ > 4096) {
		MsgWrite(MSG_ERROR, "%s doesn't match the rip (%zu of its %zu kept bytes differ); is it the same game and version?", path,
		         differ, kept);
		free(*data);
		*data = NULL;
		return false;
	}
	MsgWrite(MSG_DETAIL, "Sample data from %s (matches the rip; %zu of %zu kept bytes differ, the ripper's patches)", path, differ,
	         kept);
	return true;
}

bool RenderTrack(const struct RenderOptions* opts, const char* input, const char* output, RenderProgress progress,
                 void* user, struct RenderResult* result) {
	memset(result, 0, sizeof(*result));
	struct GSFImage image;
	struct PSFTags tags;
	char err[512];
	if (!GSFLoad(input, &image, &tags, err, sizeof(err))) {
		MsgWrite(MSG_ERROR, "%s", err);
		return false;
	}

	bool ok = false;
	struct mCore* core = NULL;
	struct GBA* gba = NULL;
	bool mixerReady = false;
	bool hifiReady = false;
	bool romLoaded = false;
	uint8_t* sampleRom = NULL;
	size_t sampleRomSize = 0;
	struct BLMixer mixer;
	struct HiFi hifi;
	struct HiFi* activeHifi = NULL;
	struct MP2KHook mp2k;
	struct ADHook ad;
	bool haveMP2K = false;
	bool haveAD = false;
	struct Capture capture;
	struct WavWriter wav;
	bool wavOpen = false;
	double* buf = NULL;
	memset(&wav, 0, sizeof(wav));

	double length = opts->length;
	if (length < 0) {
		length = PSFParseTime(PSFTagGet(&tags, "length"));
	}
	if (length < 0) {
		length = DEFAULT_LENGTH;
	}
	double fade = opts->fade;
	if (fade < 0) {
		fade = PSFParseTime(PSFTagGet(&tags, "fade"));
	}
	if (fade < 0) {
		fade = opts->length >= 0 || PSFTagGet(&tags, "length") ? 0 : DEFAULT_FADE;
	}
	double gain = pow(10, opts->gainDb / 20);
	const char* volumeTag = PSFTagGet(&tags, "volume");
	if (opts->useVolumeTag && volumeTag) {
		double v = strtod(volumeTag, NULL);
		if (v > 0) {
			gain *= v;
		}
	}
	const char* title = PSFTagGet(&tags, "title");
	const char* game = PSFTagGet(&tags, "game");
	snprintf(result->title, sizeof(result->title), "%s%s%s", game ? game : "", game && title ? " - " : "", title ? title : "");

	bool multiboot = (image.entry >> 24) == 0x02;
	struct VFile* vf = VFileFromConstMemory(image.data, image.size);
	core = mCoreCreate(mPLATFORM_GBA);
	if (!vf || !core) {
		MsgWrite(MSG_ERROR, "Could not create the GBA core");
		if (vf) {
			vf->close(vf);
		}
		goto done;
	}
	core->init(core);
	mCoreInitConfig(core, NULL);
	struct mCoreOptions coreOpts = {
		.skipBios = true,
		.useBios = opts->bios != NULL,
		.volume = 0x100,
	};
	mCoreConfigLoadDefaults(&core->config, &coreOpts);
	core->loadConfig(core, &core->config);

	gba = core->board;
	// Don't let the core guess cartridge vs. multiboot from the image; the GSF
	// entry point already says which it is
	if (multiboot ? !GBALoadMB(gba, vf) : !GBALoadROM(gba, vf)) {
		// The core may or may not own vf by now; leaking this one is safer than
		// closing it twice
		MsgWrite(MSG_ERROR, "Could not load the program image");
		goto done;
	}
	romLoaded = true;
	if (opts->bios) {
		struct VFile* bios = VFileOpen(opts->bios, O_RDONLY);
		if (!bios) {
			MsgWrite(MSG_ERROR, "Could not open the BIOS %s", opts->bios);
			goto done;
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

	BLMixerInit(&mixer, GBA_ARM7TDMI_FREQUENCY, opts->rate);
	mixerReady = true;

	struct MemoryView view;
	MemoryViewInit(&view, gba->cpu);
	uint32_t hookAddress = multiboot ? 0 : MP2KFindHook(image.data, image.size);
	struct ADDriver adDriver;
	bool foundAD = !hookAddress && !multiboot && ADFind(image.data, image.size, &adDriver);
	if (opts->verify && !hookAddress && !foundAD) {
		MsgWrite(MSG_ERROR, "No MP2K or AlphaDream sound driver found in this file, so there is nothing to verify");
		goto done;
	}
	const struct HiFiDriver* hifiDriver = hookAddress ? &MP2KBackend : foundAD ? &ADBackend : NULL;
	result->driver = hookAddress ? "MP2K" : foundAD ? "AlphaDream" : NULL;
	result->noDriver = opts->hifi && !hifiDriver;
	if (opts->hifi && hifiDriver && opts->sampleRom && !_loadSampleRom(opts->sampleRom, &image, &sampleRom, &sampleRomSize)) {
		goto done;
	}
	if (opts->hifi && hifiDriver) {
		HiFiInit(&hifi, hifiDriver, &mixer, &view.d, sampleRom ? sampleRom : image.data, sampleRom ? sampleRomSize : image.size,
		         GBA_ARM7TDMI_FREQUENCY, opts->rampMs / 1000.0);
		hifiReady = true;
		hifi.mode = opts->hifiMode;
		hifi.mutedChannels = opts->muteChannels;
		hifi.bandwidth = opts->bandwidth;
		hifi.soloWav = opts->soloWav;
		memcpy(hifi.linearWavs, opts->linearWavs, sizeof(opts->linearWavs));
		hifi.linearWavCount = opts->linearWavCount;
		// A full ROM has no holes, only real zeros
		hifi.fillHoles = !opts->noFillHoles && !sampleRom;
		if (opts->sampleStats) {
			hifi.collectStats = true;
			hifi.stats = calloc(HIFI_MAX_SAMPLE_STATS, sizeof(*hifi.stats));
		}
		if (opts->sourceCutoff > 0) {
			hifi.sourceCutoff = opts->sourceCutoff;
		}
		activeHifi = &hifi;
	}
	if (foundAD && (opts->verify || activeHifi)) {
		MsgWrite(MSG_DETAIL, "AlphaDream driver found; hooking its mix routine at %08X", adDriver.mix);
		ADHookInstall(&ad, gba, &adDriver, opts->verify, activeHifi, opts->bandwidthDriver);
		haveAD = true;
	}
	if (hookAddress && (opts->verify || activeHifi)) {
		MsgWrite(MSG_DETAIL, "MP2K driver found; hooking SoundMainRAM entry at %08X", hookAddress);
		MP2KHookInstall(&mp2k, gba, hookAddress, opts->verify, activeHifi, opts->bandwidthDriver);
		haveMP2K = true;
	}

	CaptureInit(&capture, &gba->audio, &mixer, &gba->timing, opts, activeHifi);

	if (!WavOpen(&wav, output, opts->format, opts->rate)) {
		MsgWrite(MSG_ERROR, "Could not open %s for writing: %s", output, strerror(errno));
		goto done;
	}
	wavOpen = true;

	uint64_t fadeStart = (uint64_t) llround(length * opts->rate);
	uint64_t fadeFrames = (uint64_t) llround(fade * opts->rate);
	uint64_t total = fadeStart + fadeFrames;
	uint64_t produced = 0;
	double peak = 0;
	size_t bufFrames = 1 << 16;
	buf = malloc(bufFrames * 2 * sizeof(double));
	int lastPercent = -1;
	bool writeOk = true;

	while (produced < total) {
		core->runFrame(core);
		// Flush the PSG up to now before reading out
		CaptureSync(&capture, &gba->audio, mTimingCurrentTime(&gba->timing));
		size_t n;
		double limit = (double) mTimingGlobalTime(&gba->timing);
		if (activeHifi && HiFiHorizon(activeHifi) < limit) {
			limit = HiFiHorizon(activeHifi);
		}
		while ((n = BLMixerRead(&mixer, limit, buf, bufFrames)) > 0) {
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
			if (!WavWrite(&wav, buf, n)) {
				MsgWrite(MSG_ERROR, "Could not write to %s (is the disk full?)", output);
				writeOk = false;
				break;
			}
			produced += n;
			if (produced >= total) {
				break;
			}
		}
		if (!writeOk) {
			break;
		}
		int percent = (int) (produced * 100 / (total ? total : 1));
		if (progress && percent != lastPercent) {
			progress(user, percent);
			lastPercent = percent;
		}
	}

	bool closed = WavClose(&wav, writeOk);
	wavOpen = false;
	if (writeOk && !closed) {
		MsgWrite(MSG_ERROR, "Could not finish writing %s", output);
		writeOk = false;
	}
	if (!writeOk) {
		goto done;
	}
	result->seconds = (double) produced / opts->rate;
	result->peak = peak;

	int fifo;
	for (fifo = 0; fifo < 2; ++fifo) {
		if (capture.fifoCount[fifo]) {
			MsgWrite(MSG_DETAIL, "DirectSound %c: %.2f Hz", 'A' + fifo, GBA_ARM7TDMI_FREQUENCY / capture.fifoPeriod[fifo]);
		}
	}
	if (peak > 1 && opts->format != FORMAT_F32) {
		MsgWrite(MSG_WARN, "clipped (peak %.2f dBFS); lower the level with -g, or use -b 32f", 20 * log10(peak));
	}
	if (mixer.lateEvents) {
		MsgWrite(MSG_WARN, "%llu events arrived after their output was finalized", (unsigned long long) mixer.lateEvents);
	}
	result->lateEvents = mixer.lateEvents;

	if (opts->verify && (haveAD || haveMP2K)) {
		const char* name = haveAD ? "AlphaDream" : "MP2K";
		uint64_t frames = haveAD ? ad.frames : mp2k.frames;
		uint64_t bad = haveAD ? ad.badFrames : mp2k.badFrames;
		uint64_t badSamples = haveAD ? ad.badSamples : mp2k.badSamples;
		uint64_t samples = haveAD ? ad.samples : mp2k.samples;
		result->verified = true;
		result->verifyFrames = frames;
		result->verifyBad = bad;
		MsgWrite(bad ? MSG_WARN : MSG_INFO, "%s verify: %llu frames, %llu differing (%llu of %llu samples)", name,
		         (unsigned long long) frames, (unsigned long long) bad, (unsigned long long) badSamples,
		         (unsigned long long) samples);
	}
	if (activeHifi) {
		const char* name = hifi.driver->name;
		if (hifi.locked) {
			result->path = RENDER_HIFI;
			MsgWrite(MSG_DETAIL,
			         "%s high-precision: locked to %.2f Hz FIFO clock, FIFO A/B carry halves %d/%d, %llu resyncs, %llu samples lost, %llu voice fade-outs dropped",
			         name, GBA_ARM7TDMI_FREQUENCY / hifi.latchPeriod, hifi.halfForFifo[0], hifi.halfForFifo[1],
			         (unsigned long long) hifi.resyncs, (unsigned long long) hifi.lostSamples,
			         (unsigned long long) hifi.droppedGhosts);
			if (hifi.resyncs || hifi.lostSamples || hifi.droppedGhosts) {
				MsgWrite(MSG_WARN, "%s re-render glitches: %llu voice resyncs, %llu samples lost, %llu fade-outs dropped", name,
				         (unsigned long long) hifi.resyncs, (unsigned long long) hifi.lostSamples,
				         (unsigned long long) hifi.droppedGhosts);
			}
			if (hifi.holesFilled) {
				MsgWrite(MSG_DETAIL, "%s high-precision: filled %llu zeroed sample bytes (rip holes) in %zu samples", name,
				         (unsigned long long) hifi.holesFilled, hifi.repairCount);
			}
		} else if (!hifi.failed) {
			result->path = RENDER_PSG_ONLY;
			MsgWrite(MSG_DETAIL, "%s high-precision: the driver never produced PCM output (PSG-only track?); nothing to re-render", name);
		} else {
			result->path = RENDER_FALLBACK;
		}
		result->resyncs = hifi.resyncs;
		result->samplesLost = hifi.lostSamples;
		result->fadeOutsDropped = hifi.droppedGhosts;
		size_t i;
		for (i = 0; i < hifi.statsCount; ++i) {
			const struct HiFiSampleStats* st = &hifi.stats[i];
			MsgWrite(MSG_OUT, "sample %08X notes %u seconds %.3f meanrate %.1f maxrate %.1f maxgain %.4f", st->wav, st->notes,
			         st->seconds, st->seconds > 0 ? st->rateSeconds / st->seconds : 0, st->maxRate, st->maxGain);
		}
	}
	ok = true;

done:
	if (wavOpen) {
		WavClose(&wav, false);
	}
	free(buf);
	if (hifiReady) {
		HiFiDeinit(&hifi);
	}
	free(sampleRom);
	if (gba) {
		gba->audio.observer = NULL;
	}
	if (mixerReady) {
		BLMixerDeinit(&mixer);
	}
	if (core) {
		if (romLoaded) {
			core->unloadROM(core);
		}
		mCoreConfigDeinit(&core->config);
		core->deinit(core);
	}
	GSFImageDeinit(&image);
	PSFTagsDeinit(&tags);
	result->ok = ok;
	return ok;
}
