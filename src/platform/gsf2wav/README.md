gsf2wav
=======

A testbed for high-fidelity playback of Game Boy Advance music rips (GSF /
minigsf), built on mGBA's GBA core. It renders a rip to WAV, using the game's
own code on an emulated console as the source of truth, but it doesn't use
mGBA's audio output. It takes the raw inputs to the GBA's DAC, and for games
using Nintendo's MP2K sound driver it re-renders the driver's voices itself at
high precision.

It was developed on **Mother 3** (all 276 tracks of the 2006-04-20 rip) and
also tested on **Wario Land 4** (all 113 tracks of the 2001-08-21 rip).
Other MP2K games should work; `--mp2k-verify` checks a new one. See
[Status](#status-and-known-issues).

Contents: [Background](#background) · [What's added, and why](#whats-added-and-why) ·
[Results on Mother 3](#results-on-mother-3) · [Building and usage](#building-and-usage) ·
[Status and known issues](#status-and-known-issues) ·
[Working on this](#working-on-this-people-and-agents) · [References](#references)


Background
----------

### How a GBA makes music

The GBA has two kinds of sound hardware:

- **PSG**: the four Game Boy channels (two square waves, a 4-bit wave channel,
  noise). Hardware synthesizes these.
- **DirectSound**: two 8-bit PCM FIFOs, fed by DMA and clocked by a timer. The
  hardware only plays samples; software has to produce them.

Most commercial games produce the DirectSound stream with Nintendo's **MP2K**
driver (also called m4a, or "Sappy"). Once per video frame it mixes up to 12
sampled-instrument voices in software, in a routine called `SoundMainRAM`:

- each voice is linearly interpolated down to a fixed mixing rate (Mother 3
  uses 15768 Hz, so nothing above 7.9 kHz survives)
- each voice's contribution is floored to 8 bits
- the voices are summed in bytes that wrap on overflow instead of clipping
- volume envelopes move in steps, once per frame (60 Hz)

So the DirectSound stream a GBA plays is already lossy before it reaches the
DAC. On Mother 3 the 8-bit truncation alone costs about 25 dB of
signal-to-noise on a typical track.

### How emulators play it

Most GSF players (lazygsf and players built on it, among others) run mGBA and
use its normal audio output. mGBA samples the audio hardware on a grid set by
the game's SOUNDBIAS register (usually 32768 Hz), clamps to the 10-bit DAC
range, and resamples to the output rate. That is faithful to the hardware, but
it adds its own resampling on top of the driver's losses.

### mGBA's "XQ" mode

mGBA 0.8.0 (2020) added an experimental "XQ" audio mode: a high-level
re-implementation of the MP2K mixer that tried to render the voices at better
quality instead of playing the driver's 8-bit mix. Some mGBA-based players
expose it. It was known for clicks, and mGBA's unreleased 0.11 changelog
removes it ("Remove broken XQ audio pending rewrite"). This fork is based on
a tree without it.

Reading its code (`src/gba/extra/audio-mixer.c` in mGBA 0.8–0.10) turns up
several causes. These are code-reading findings; the code wasn't run here:

- Each channel takes its key and instrument from its *track*, not from the
  channel itself, so chords play the wrong sample data.
- A voice's playback position is only reset if the channel happened to be
  idle at a vblank, so a reused channel can start a new note mid-sample.
- Every sample loops; the driver's no-loop flag is ignored.
- Nearest-neighbour sample stepping, and volume that jumps once per update.
- Its producer and consumer run at slightly different rates (686 vs. 685.78
  samples per tick), and an underrun appears to read an uninitialized
  value.
- It switches itself off on tracks that use XCMD extended commands.

The underlying problem: it approximated the driver instead of reproducing it,
and had no way to check itself against the real thing.


What's added, and why
---------------------

The approach here: first reproduce the driver *exactly*, prove it against the
game every frame, and only then change the arithmetic.

### Core changes (small, off unless a tool enables them)

- **`GBAAudioObserver`** (`include/mgba/internal/gba/audio.h`,
  `src/gba/audio.c`). A tool can observe the audio hardware's raw state. It
  gets a `sync` call before every change to audio state, and a `fifoSample`
  call with the exact CPU cycle each DirectSound sample latches at.
  *Why:* this bypasses mGBA's sampling grid, DAC clamp and resampler, with no
  cost when unused.
- **`GBAInstallCodeHook`** (`include/mgba/internal/gba/gba.h`,
  `src/gba/gba.c`). A single tool-owned code hook, built on the same
  BKPT-patch-and-`ARMRunFake` mechanism mGBA's cheat engine uses, in the
  `CPU_COMPONENT_MISC_1` slot.
  *Why:* the MP2K renderer needs to run at one exact instruction every frame.

### The tool (`src/platform/gsf2wav/`)

| File | What it does | Why |
|---|---|---|
| `psf.c` | PSF/minigsf loader: zlib, CRC, tags, `_lib` chains in psflib order, case-insensitive lib lookup | Rips are usually made on case-insensitive filesystems |
| `blmix.c` | Band-limited renderer evaluated at the output instants: steps (BLEP) and points (windowed sinc), Kaiser kernel, ~120 dB stopband, double precision | No intermediate sample rate anywhere in the chain |
| `mp2k.c` | Literal C port of Mother 3's `SoundMainRAM` (SDK 3.0 revision): envelope state machine, fixed-frequency and interpolated paths, loop wrap, reverb, byte-lane wraparound | A model of the driver that is exact, not approximate |
| `mp2k_hifi.c` | High-precision re-render of the driver's voices from its state | Recovers what the driver's mix throws away |
| `main.c` | CLI, capture observer, WAV writer, MP2K hook | |

How a render works:

1. **PSG** is walked on an 8-cycle grid between register writes. That is the
   wave channel's timer granularity, so every level change is caught exactly.
   Each change is rendered as a band-limited step.
2. **DirectSound**, for non-MP2K audio, is sinc-interpolated at its own
   sample rate, with the exact cycle each sample latched at. `--fifo-hold`
   renders the hardware's sample-and-hold instead.
3. **MP2K voices**:
   - gsf2wav finds `SoundMain` in ROM by its literal pool, and hooks the jump
     into `SoundMainRAM`. At that point the sequencer has run and the mixer
     hasn't.
   - It reads the driver's channel state each frame and renders each voice
     straight from its source PCM to the output rate, at the driver's exact
     positions and pitches:
     - windowed-sinc resampling, band-limited to both the sample's Nyquist and
       the output's
     - volumes aren't truncated, and nothing wraps
     - volume steps are ramped over 2 ms, and cut notes fade over the same
       time
     - new notes start a few ms early, so the kernel's pre-ringing isn't
       truncated
     - the driver's reverb is modeled
   - Timing comes from the hardware. For each queued frame, the most
     distinctive 64-sample stretch of its exact mix is found in the captured
     FIFO stream. It must match exactly once, and a later frame must confirm
     it at the predicted place. Every frame from then on is placed where the
     hardware played it.
4. `--mp2k-verify` runs the C port on a copy of the driver state every frame
   and diffs the result against what the game actually wrote. That is what
   keeps the model honest.

`--mp2k-mix linear` is a middle option: the driver's own algorithm (linear
interpolation at its mixing rate), minus the 8-bit truncation and wrap.


Results on Mother 3
-------------------

Driver: MP2K, SDK 3.0 revision, 15768 Hz, 264 samples per frame, 12 channels.

- **Port accuracy:** bit-exact against the game. 23 tracks × 60 s, 43 million
  samples, zero differences.
- **Coverage:**
  - All 276 tracks rendered and encoded.
  - Every track with PCM content locked onto the hardware's timing, with zero
    position resyncs, zero lost samples and zero dropped fade-outs.
  - One track ("Porky's Porkies") is PSG-only, so there's nothing to
    re-render.
- **Against the driver's own mix:** zero lag, and levels within 0.15 dB (the
  driver truncates its volume values).
  - The driver's 8-bit truncation is a −25 dB residual on a typical track.
  - The float version of the driver's algorithm and the sinc renderer agree
    to 0.001 dB below 2 kHz.
  - Above the driver's 7.9 kHz ceiling, pitched-up voices now carry real
    content, at about −38 dB.
- **Clicks:** no systematic discontinuities at frame boundaries (>16 kHz
  energy is flat across the frame phase), and no hard note cuts.
- **Synthetic tests** (`test/run.py`):
  - PSG aliasing below −160 dB
  - square-wave harmonics within 0.001 dB of theory up to 20 kHz
  - DirectSound images at −148 dB
  - hold mode matches sample-and-hold theory
  - `_lib` loading renders bit-identically to a plain GSF
- **Speed:** about 4–12× realtime per core on Mother 3. The whole set, about
  7 hours of audio, took about 25 minutes on 4 cores.

Results on Wario Land 4
-----------------------

Driver: the same MP2K revision, configured differently: 13379 Hz, 224 samples
per frame, 8 channels, a DMA period of 7, and `maxLines` 70.

- **Port accuracy:** bit-exact with no changes. 19 tracks × 60 s, 30 million
  samples, zero differences.
- **Coverage:** all 113 tracks locked and rendered cleanly. "Wario's
  Roulette" originally failed to lock: its only instrument is a 96-byte
  looping wave, and the old lock (the first 32 samples of two consecutive
  frames) never found a distinctive enough snippet. That led to the current
  lock.
- **Same caveat as Mother 3's organ:** tiny synth waves pitched well up (here
  2.5–5.5×) sound clean in sinc and aliased in the game's own mix. Which one
  is right is a matter of taste.
- **`maxLines`:** when nonzero, the driver skips voices if the CPU runs late
  in a frame. The port doesn't model it. It never triggered in these rips
  (zero differences), but it could matter during real gameplay.

Bugs found and fixed along the way, as a record of what the checks caught:

- A running sum of sampled impulses tilted the BLEP response by
  1/sinc(f/rate), +2.6 dB at 20 kHz.
- An inverted rate ratio lowpassed every voice at about 2.4 kHz.
- Quiet song intros were finalized before their audio arrived.
- Note-ons truncated the sinc's pre-ringing, which left a click at the frame
  boundary.
- Output buffers were too small for a late timing lock: 0.9 s lost on
  "Memory of Mother" at 48 kHz.


Building and usage
------------------

    mkdir build && cd build
    cmake .. -DBUILD_GSF2WAV=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DUSE_FFMPEG=OFF
    make gsf2wav

This needs zlib. The binary is `build/gsf2wav/gsf2wav`.

    gsf2wav [options] INPUT.minigsf OUTPUT.wav

Output and length:

    -r, --rate HZ         output sample rate (default 48000)
    -b, --bits FMT        32f, 24 or 16 (default 32f; integer formats are TPDF dithered)
    -l, --length TIME     play length before fade, overrides the length tag
    -f, --fade TIME       fade length, overrides the fade tag
    -g, --gain DB         extra gain in dB
        --no-volume-tag   ignore the volume tag

TIME is seconds or `[h:]m:ss[.fff]`. Without tags, length defaults to 150 s
and fade to 10 s.

MP2K rendering (on by default when the driver is found):

        --no-hifi         output the driver's own mix instead of re-rendering its voices
        --mp2k-mix MODE   sinc (default) or linear
        --mp2k-bandwidth HZ   cap each voice's bandwidth; "driver" = the driver's Nyquist
        --mp2k-source-cutoff F  each voice's cutoff as a fraction of its playback rate (default 0.47)
        --mp2k-linear-samples LIST  render these samples (hex header addresses) like the
                          driver, linear at its mixing rate; everything else stays sinc
        --ramp MS         volume change and note cut smoothing (default 2; 0 = the driver's steps)

Other audio:

        --fifo-hold       DirectSound as the hardware's sample-and-hold, not sinc-interpolated
        --psg-grid CYCLES PSG sampling grid (default 8, already exact)
        --bios FILE       use a real GBA BIOS instead of mGBA's built-in one

Isolation and diagnostics:

        --mute LIST       silence psg, pcm, or MP2K channels 0-11, e.g. --mute psg,0,3
        --solo-sample ADDR  only voices playing the sample whose header is at hex ADDR
        --sample-stats    list the MP2K samples played (stdout)
        --mp2k-verify     check the MP2K port against the game's mixer every frame

The BIOS barely matters for Mother 3. Over 60 s of three songs it only calls
`Halt`, `Div`, `CpuSet`, `CpuFastSet` and `LZ77UnCompVram`, all of which
mGBA's built-in BIOS reproduces exactly. It never calls the BIOS's own sound
routines.

Rendering a whole set to tagged Opus (needs `opusenc`):

    python3 src/platform/gsf2wav/tools/render_set.py build/gsf2wav/gsf2wav SET_DIR OUT_DIR \
        --bitrate 96 --zip out.zip [--overrides FILE] -- -r 48000 -b 32f

An overrides file gives per-track options (one line each: track filename
prefix, then gsf2wav options). Mother 3's is
`tools/data/mother3_overrides.txt`: it renders 006's organ linearly and lowers
four tracks' levels. Wario Land 4's is `tools/data/wl4_overrides.txt`. The
full command that produced the delivered Mother 3 set:

    python3 src/platform/gsf2wav/tools/render_set.py build/gsf2wav/gsf2wav rips/mother3 OUT_DIR \
        --bitrate 96 --zip "Mother 3.zip" \
        --overrides src/platform/gsf2wav/tools/data/mother3_overrides.txt -- -r 48000 -b 32f


Status and known issues
-----------------------

- **Only the SDK 3.0 mixer revision is ported.** Later revisions added
  compressed and reversed samples, which will render wrong. Run
  `--mp2k-verify` on a new game first; it reports any frame where the port
  and the game disagree.
- **Some samples are gritty in sinc mode.** In Mother 3, track 006's organ
  (`082ED1BC`) is a jagged, pulse-like waveform played 3–5× above its recorded
  rate. Sinc reproduces its jumps faithfully, and the game's linear
  interpolation smooths them over. The opposite is true for low-pitched voices:
  006's intro chords (played as low as 0.33×) sound grainy in linear mode,
  because linear interpolation leaves images of the stretched waveform, and
  they're clean in sinc. Neither mode suits the whole song, so
  `--mp2k-linear-samples 082ED1BC` renders just the organ linearly. The organ
  is only used in 006. Simple statistics on the sample data don't pick out
  samples like this (see `tools/sample_noise.py`), so overrides are chosen by
  ear, with `--solo-sample` and `--sample-stats` to find the candidates.
- **Tracks can clip.** Float output keeps overs (Mother 3's unused Giygas
  battle track peaks at +2.2 dBFS). Lossy encoding also overshoots: Opus
  pushed three Mother 3 tracks with float peaks of −0.07 to −0.33 dBFS past
  0 dBFS once decoded. Keep peaks around −1 dBFS with `-g` (per track, via an
  overrides file), and check by decoding (`opusdec --float`).
- **Lengths come from tags.** There's no loop-count option yet. The robust way
  would be reading the sequencer's GOTO commands; `tools/find_loop.py` finds
  loop periods from audio but not loop starts.
- **Late timing lock.** The re-render holds output until it finds the
  driver's first audible frame in the FIFO stream. It gives up after about
  15 s and falls back to the captured stream.

### Not started: live playback in the libretro core

The same rendering could run during gameplay in RetroArch's mGBA core. This
is an assessment, not a plan anyone has started on:

- **Integration point.** `retro_run` in `src/platform/libretro/libretro.c`
  calls `core->runFrame()`, then drains the core's `mAudioBuffer` into
  `audioCallback`. A hi-fi path would install the audio observer and the MP2K
  code hook when the game loads, and read the band-limited mixer instead. It
  would also report 48 kHz in `retro_get_system_av_info` in place of the
  core's own rate.
- **Latency is small.** The driver mixes each frame about one frame before
  the hardware plays it, so the re-render isn't behind real time. The added
  delay is the renderer's look-ahead: about 30 ms with the current kernels,
  mostly `blmix.c`'s 900 Hz minimum source cutoff and 48 zero crossings. A
  shorter kernel could bring it under 10 ms.
- **Cost** is roughly 40M multiply-adds per second for 12 voices at 48 kHz.
  That's fine on a desktop; low-end hardware would want a shorter kernel.
- **What needs building:**
  1. Play the core's normal audio until the lock lands, then crossfade,
     instead of holding output back.
  2. Verify the exact mix against the FIFO every frame (the `--mp2k-verify`
     machinery), and on a mismatch fall back and re-lock. This covers loading
     screens, frames that skip `SoundMain`, soft resets and driver
     reinitialization.
  3. Reset and re-lock on savestate loads; the renderer's state isn't
     serialized. Rewind needs either real serialization or a short crossfade
     per step.
  4. Turn it off while fast-forwarding.
  5. Core options: enable, mix mode, per-sample overrides.
- **Estimate:** a prototype (Mother 3, no savestate support) in a few days;
  robust enough to leave on in two to three weeks, mostly testing gameplay
  edge cases. Play-testing needs RetroArch on a real machine.


Working on this (people and agents)
-----------------------------------

**The repository is public, so never commit ROM-derived data:** no rips,
renders, disassembly listings or extracted ROM images. `rips/` at the repo
root is gitignored for GSF sets. `src/platform/gsf2wav/tools/fetch_set.py URL [name]` downloads and
extracts a set into it. Sample addresses and statistics
(`tools/data/mother3_sample_stats.csv`) are fine.

**Checks to run before trusting a change** (paths from the repo root):

1. `python3 src/platform/gsf2wav/test/run.py build/gsf2wav/gsf2wav` for the synthetic suite (needs
   clang with the ARM target, ld.lld, llvm-objcopy and numpy). All five checks
   must pass.
2. `--mp2k-verify` on a few tracks of the game you're working on. It must
   report zero differing frames. Any mixer change must keep this true.
3. A sweep across the set. For example,
   `python3 src/platform/gsf2wav/tools/batch.py build/gsf2wav/gsf2wav rips/mother3 --every 3 --grep high-precision -- -l 45 -f 0`
   should report every track locked, with 0 resyncs, 0 samples lost and 0
   fade-outs dropped.
4. For changes that affect sound: compare renders with the tools below, and
   A/B by ear. The measurements have caught real bugs, but the last word on
   sound quality was always a listening test.

**Tools** (`tools/`):

| Script | Purpose |
|---|---|
| `fetch_set.py` | Download a GSF set into `rips/` |
| `batch.py` | Run gsf2wav over a set in parallel, filtering its stderr |
| `render_set.py` | Render a set to tagged Opus and zip it |
| `compare.py` | Lag, gain and residual between two renders in a common band |
| `bands.py` | Octave-band energy of several renders |
| `residual_blocks.py` | Residual per 20 ms block, to find localized mismatches |
| `clicks.py` | Crude click detector (high-passed energy spikes) |
| `find_loop.py` | Loop period from a long render (start detection unreliable) |
| `mp2k_scan.py` | Find `SoundMain` / the `SoundMainRAM` entry in a GSF |
| `disasm_mp2k.py` | Disassemble a game's driver (keep the output out of git) |
| `sample_stats.py` | Per-sample usage across a set (via `--sample-stats`) |
| `sample_noise.py` | Heuristic sample-noisiness ranking (doesn't find the organ) |
| `gsfpy.py` | Minimal Python PSF loader used by the above |

Code follows mGBA's style (tabs, `CamelCase` types and functions,
underscore-prefixed static helpers, MPL-2.0 headers). The branch history has one commit per feature or fix, and
each message explains the measurement behind it.


References
----------

Other people's code, or unlicensed. Not included here, but used while writing
this:

- pret's decompiled MP2K driver: `src/m4a_1.s` and
  `include/gba/m4a_internal.h` in https://github.com/pret/pokeemerald. This is
  a later revision than Mother 3's; `mp2k.c` follows Mother 3's own
  disassembly where they differ.
- kode54's psflib, for the `_lib` load order: https://github.com/kode54/psflib
- lazygsf, a GSF library built on mGBA: https://buffering.party/software/lazygsf/
- mGBA's "XQ" mixer, `src/gba/extra/audio-mixer.c` in mGBA 0.8–0.10.
- A 2SF player resampler with BLEP/BLAM modes:
  https://github.com/yshui/2sftowav/blob/master/src/vio2sf/desmume/resampler.c
