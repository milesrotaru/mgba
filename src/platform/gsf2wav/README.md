gsf2wav
=======

> **Read this first.** This is a hobby experiment, not a polished or
> authoritative tool. I'm not an emulator developer, an audio engineer or a
> ripper, and I don't have deep experience with DSP or the GBA. Most of the code
> was written by Claude, an AI assistant, working with me. To keep it honest we
> leaned on measurements (checking the reimplemented mixer against the game
> frame by frame, plus a few synthetic tests) and on listening, which is
> subjective and was never blind. Both of us can be wrong. If you know this
> area and spot a mistake, I'd really like to hear about it.
>
> It only exists because of other people's work: mGBA, lazygsf, psflib, pret's
> decompilation of the MP2K driver, and the rippers who made the GSF sets. See
> [Credits](#credits-and-things-i-read). Nothing here is meant as criticism of
> any of them.

gsf2wav renders GBA music rips (GSF / minigsf) to WAV, using mGBA's GBA core to
run the game's own code. It doesn't use mGBA's audio output. It takes the raw
inputs to the GBA's DAC, and for two sound drivers it knows about (Nintendo's
MP2K, and the one AlphaDream used in Mario & Luigi: Superstar Saga) it also
re-renders the driver's voices itself, at higher resolution than the driver's
own mix.

I wanted to hear the music with less of the original mixer's lossiness and less
of an emulator's resampling. **That is a deliberate departure from what the
hardware produced**, which some people won't want. The closest thing to the
hardware's signal that this tool offers is `--no-hifi --fifo-hold`, though I
haven't compared it against a recording of a real GBA.

It was developed on **Mother 3** (all 276 tracks of the 2006-04-20 rip) and
also tried on **Wario Land 4** (113 tracks) and **Mario & Luigi: Superstar Saga**
(51 tracks). Three games is not many. Other games may well break it;
`--verify` will at least tell you if the mixer port disagrees with the
game. See [Status](#status-and-known-issues).

Contents: [Background](#background) · [What's added, and why](#whats-added-and-why) ·
[Mother 3](#results-on-mother-3) · [Wario Land 4](#results-on-wario-land-4) ·
[Superstar Saga](#results-on-mario--luigi-superstar-saga) ·
[Building and usage](#building-and-usage) ·
[Status and known issues](#status-and-known-issues) ·
[Working on this](#working-on-this-people-and-agents) ·
[Credits](#credits-and-things-i-read)


Background
----------

### How a GBA makes music

The GBA has two kinds of sound hardware:

- **PSG**: the four Game Boy channels (two square waves, a 4-bit wave channel,
  noise). Hardware synthesizes these.
- **DirectSound**: two 8-bit PCM FIFOs, fed by DMA and clocked by a timer. The
  hardware only plays samples; software has to produce them.

Most commercial games produce the DirectSound stream with Nintendo's **MP2K**
driver (also called m4a, or "Sappy"). As I understand it, once per video frame
it mixes up to 12 sampled-instrument voices in software, in a routine called
`SoundMainRAM`:

- each voice is linearly interpolated down to a fixed mixing rate (Mother 3
  uses 15768 Hz, so nothing above 7.9 kHz survives)
- each voice's contribution is floored to 8 bits
- the voices are summed in bytes that wrap on overflow instead of clipping
- volume envelopes move in steps, once per frame (60 Hz)

So the DirectSound stream a GBA plays is already lossy before it reaches the
DAC. On Mother 3 the 8-bit truncation alone costs about 25 dB of
signal-to-noise on a typical track, by my measurement.

### How emulators play it

Most GSF players (lazygsf and players built on it, among others) run mGBA and
use its normal audio output. mGBA samples the audio hardware on a grid set by
the game's SOUNDBIAS register (usually 32768 Hz), clamps to the 10-bit DAC
range, and resamples to the output rate. That is faithful to the hardware. My
only observation is that it adds its own resampling on top of what the driver
already does, and I wanted to see what happens without that.

### mGBA's "XQ" mode

mGBA 0.8.0 (2020) added an experimental "XQ" audio mode: a high-level
re-implementation of the MP2K mixer, meant to render the voices at better
quality than the driver's own 8-bit mix. That is a hard problem, and I'm not
claiming to have solved it in general. As far as I know it was removed pending
a rewrite (the unreleased 0.11 changelog says so), and this fork is based on a
tree that doesn't have it.

I mention it because it's the closest prior art, and because it shaped what I
tried: **reimplement the driver first, check the reimplementation against the
real one every frame, and only then change the arithmetic.** That way, if the
output sounds different, I know it's the arithmetic I changed and not a mistake
in the model. That's a choice that suited my situation (I have the games and
the rips, and no deep knowledge of the driver), not a claim that it's the right
approach for an emulator.


What's added, and why
---------------------

### Core changes (small, off unless a tool enables them)

- **`GBAAudioObserver`** (`include/mgba/internal/gba/audio.h`,
  `src/gba/audio.c`). A tool can observe the audio hardware's raw state. It
  gets a `sync` call before every change to audio state, and a `fifoSample`
  call with the exact CPU cycle each DirectSound sample latches at.
  *Why:* this lets the tool bypass mGBA's sampling grid, DAC clamp and
  resampler. It costs nothing when unused.
- **`GBAInstallCodeHook`** (`include/mgba/internal/gba/gba.h`,
  `src/gba/gba.c`). A single tool-owned code hook, built on the same
  BKPT-patch-and-`ARMRunFake` mechanism mGBA's cheat engine uses, in the
  `CPU_COMPONENT_MISC_1` slot.
  *Why:* the renderer needs to run at one exact instruction every frame.

These changes only add code and touch no existing behaviour, but I'm an
outsider to mGBA's internals, so please treat them as a sketch, not a proposal
for upstream.

### The tool (`src/platform/gsf2wav/`)

| File | What it does | Why |
|---|---|---|
| `psf.c` | PSF/minigsf loader: zlib, CRC, tags, `_lib` chains in psflib order, case-insensitive lib lookup | Rips are usually made on case-insensitive filesystems |
| `blmix.c` | Band-limited renderer evaluated at the output instants: steps (BLEP) and points (windowed sinc), Kaiser kernel, about 120 dB stopband, double precision | No intermediate sample rate anywhere in the chain |
| `mp2k.c` | C reimplementation of Mother 3's `SoundMainRAM` (SDK 3.0 revision): envelopes, fixed-frequency and interpolated paths, loop wrap, reverb, byte-lane wraparound | A model of the driver that I can check against the game exactly |
| `hifi.c` | Re-renders the driver's voices from its state; the renderer is driver-neutral | Recovers what the driver's mix throws away |
| `alphadream.c` | The same for AlphaDream's driver | Superstar Saga |
| `render.c` | Renders one track: loads it, runs the core, collects the audio, writes the WAV; every error path cleans up | One function a batch can call from several threads |
| `capture.c` | Receives the PSG and DirectSound levels from the core as they change | Bypasses the core's own sampling grid and resampler |
| `hooks.c` | Code hooks that catch each sound driver mid-frame, for the renderer and for `--verify` | |
| `options.c`, `overrides.c` | The option table (help text is generated from it), strict value checking, and the per-track overrides file | |
| `wav.c` | Streaming WAV writer: float, or 24/16-bit with dither | |
| `main.c` | Inputs, output names, a pool of worker threads, progress and summary | |
| `platform.c` | The few OS calls: UTF-8 paths on Windows, directory scans, wildcards, console | Windows' C library can't open Japanese file names |
| `msglog.c` | Collects each render's messages so renders on different threads don't interleave | |
| `gsftrace.c` | Small aid for finding a driver in an unknown game | |

How a render works:

1. **PSG** is walked on an 8-cycle grid between register writes. That is the
   wave channel's timer granularity, so every level change should be caught.
   Each change is rendered as a band-limited step.
2. **DirectSound**, for audio the tool doesn't know how to re-render, is
   sinc-interpolated at its own sample rate, using the exact cycle each sample
   latched at. `--fifo-hold` renders sample-and-hold instead.
3. **MP2K voices** (and AlphaDream's, similarly):
   - gsf2wav finds `SoundMain` in the ROM by its literal pool, and hooks the
     jump into `SoundMainRAM`. At that point the sequencer has run and the
     mixer hasn't.
   - It reads the driver's channel state each frame and renders each voice
     straight from its source PCM to the output rate, at the driver's
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
     it at the predicted place. After that, every frame is placed where the
     hardware played it.
4. `--verify` runs the reimplemented mixer on a copy of the driver state
   every frame and compares the result with what the game actually wrote. This
   is the main thing I relied on to catch mistakes in the model.

There are four ways to resample a voice (`--mix`), which differ mostly in
taste and in how they treat high frequencies. I only have my own ears and a few
spectra to go on:

- **`sinc`** (default): windowed sinc from the source to the output rate. It's
  the most conservative choice from a signal-processing view. To my ear it can
  make low-quality samples sound muddy at low playback rates.
- **`linear`**: the driver's own algorithm (linear interpolation at its mixing
  rate), minus the 8-bit truncation and wrap.
- **`lerp`**: linear interpolation straight from each source to the output
  rate, with no bandlimiting. It keeps everything the mixing rate would cut,
  and adds interpolation images above each source's band. On Wario Land 4's
  track 002 that's about 44 dB more energy in 10–16 kHz than sinc; that extra
  energy is images, not content from the samples. I found it bright.
- **`blam`**: linear interpolation, bandlimited to the output rate. Each
  voice's kernel is the linear-interpolation triangle convolved with the output
  lowpass, evaluated exactly (the second difference of the lowpass's second
  integral, from a Hermite-interpolated table). At low playback rates it keeps
  linear's brightness; at high rates the lowpass stops the aliasing lerp lets
  fold back. On WL4 002 it sits 4–13 dB under lerp from 6.7 to 22 kHz.
  The name and the idea come from the "blam" option in the resampler used by a
  2SF player (see Credits). I measured that resampler's behaviour, and wrote
  this one separately from the math. It's a different algorithm and may not
  sound like theirs.


Results on Mother 3
-------------------

Driver: MP2K, SDK 3.0 revision, 15768 Hz, 264 samples per frame, 12 channels.
These are measurements on the rip I had; none are blind listening tests.

- **Port accuracy:** matches the game exactly on everything I checked. 23
  tracks × 60 s, 43 million samples, zero differences.
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
  - Above the driver's 7.9 kHz ceiling, pitched-up voices now carry content, at
    about −38 dB.
- **Clicks:** I didn't find systematic discontinuities at frame boundaries
  (>16 kHz energy is flat across the frame phase), or hard note cuts. My click
  detector is crude, though.
- **Synthetic tests** (`test/run.py`):
  - PSG aliasing below −160 dB
  - square-wave harmonics within 0.001 dB of theory up to 20 kHz
  - DirectSound images at −148 dB
  - hold mode matches sample-and-hold theory
  - `_lib` loading renders bit-identically to a plain GSF
- **Speed:** about 4–12× realtime per core. The whole set, about 7 hours of
  audio, took about 25 minutes on 4 cores.


Results on Wario Land 4
-----------------------

Driver: the same MP2K revision, configured differently: 13379 Hz, 224 samples
per frame, 8 channels, a DMA period of 7, and `maxLines` 70.

- **Port accuracy:** matches with no changes. 19 tracks × 60 s, 30 million
  samples, zero differences.
- **Coverage:** all 113 tracks locked and rendered. "Wario's Roulette"
  originally failed to lock: its only instrument is a 96-byte looping wave,
  and my first lock (the first 32 samples of two consecutive frames) never
  found a distinctive enough snippet. That led to the current lock.
- **Same caveat as Mother 3's organ:** tiny synth waves pitched well up (here
  2.5–5.5×) sound clean in sinc and aliased in the game's own mix. Which one is
  right is a matter of taste.
- **`maxLines`:** when nonzero, the driver skips voices if the CPU runs late
  in a frame. The port doesn't model it. It never triggered in these rips, but
  it could matter during real gameplay.

### Mistakes along the way

Written down in case they help anyone else, and because they show what the
checks were for:

- A running sum of sampled impulses tilted the BLEP response by
  1/sinc(f/rate), +2.6 dB at 20 kHz.
- An inverted rate ratio lowpassed every voice at about 2.4 kHz.
- Quiet song intros were finalized before their audio arrived.
- Note-ons truncated the sinc's pre-ringing, which left a click at the frame
  boundary.
- Output buffers were too small for a late timing lock: 0.9 s lost on
  "Memory of Mother" at 48 kHz.
- I put the organ grit in Mother 3's track 006 down to its waveform, and worked
  around it track by track. It was really missing bytes in the rip (see
  [Rips have holes](#status-and-known-issues)), and I only realised when the
  same damage showed up as loud distortion in a Superstar Saga track.
- I delivered the first Superstar Saga renders through the plain audio path
  before understanding the game's driver, and later had to redo them.


Results on Mario & Luigi: Superstar Saga
----------------------------------------

Driver: **AlphaDream's own**, not MP2K (there is no `'Smsh'` in the ROM). It
was found with `gsftrace` (below): the sound DMAs read two 528-byte buffers
in IWRAM, one ARM routine in IWRAM writes them, and the Thumb routine that
calls it once per frame sits right before a literal pool naming all of the
IWRAM routines. `alphadream.h` documents the channel and sample formats as I
understand them.

It's simpler than MP2K, and in one way cruder: 8 channels, unsigned 8-bit
samples, **no interpolation at all** (nearest sample, 10-bit position
fraction), 15768 Hz, one frame of 264 samples per video frame, and true
stereo (FIFO A is left, B is right). Each voice's contribution is multiplied
by a packed left/right volume so one 32-bit add mixes both sides; the output
is bits 8–15 of each half, which wraps instead of clipping. The envelope
steps once per frame, like MP2K's.

- **Port accuracy:** matches. All 51 tracks × 60 s, 96 million samples, zero
  differences.
- **Coverage:** all 50 tracks with sampled instruments locked and rendered with
  0 resyncs; "Mario Bros Miss" uses only the PSG.
- **Linear mode** here means the driver's own nearest-sample playback at
  15768 Hz, without its 8-bit rounding. It matches the game's output to −33 dB
  with zero lag. The sinc-family modes come out about half a source sample
  (~0.03 ms) earlier, because nearest-sample playback delays the signal by that
  much on average.
- **The rip's zeroed code.** GSF rippers keep only the bytes the game touched
  while being ripped, so paths it never took are zeros (`movs r0, r0` in
  Thumb). Two show up here: the envelope's decay branch goes straight to the
  sustain level, and part of the division routine is missing (it only matters
  for samples over 256K frames). The port follows the rip, since that's what
  the emulated game runs.
- **Loader:** the rip's `.gsflib` size field counts its own 12-byte header, so
  the section claims 12 bytes more than the file holds. gsf2wav loads what is
  there and zeroes the rest. As far as I can tell lazygsf reads 12 bytes past
  its buffer for the same file. I could be misreading it.
- **Level:** the driver mixes hot. 16 tracks' decoded Opus peaks were above
  −1 dBFS in blam mode (up to +0.28), so `tools/data/mlss_overrides.txt` lowers
  them. The three "99 Unknown Song" tracks share a prefix, which is why
  overrides keys can also be a full quoted track name.


Building and usage
------------------

### Quick start

On Windows, drag `.minigsf` files (or a folder of them) onto `gsf2wav.exe`;
see the README.txt in the Windows zip. From a terminal:

    gsf2wav "01 Title.minigsf"            writes "01 Title.wav" next to it
    gsf2wav -o wav/ rips/mother3/         every track in the folder, into wav/
    gsf2wav -j 4 --mix blam --overrides levels.txt -o wav/ rips/

Each WAV is written as `NAME.wav.part` and renamed when it's complete, so a
WAV that exists is always a whole one, and `--skip-existing` can resume a
batch. The older form `gsf2wav [options] INPUT OUTPUT.wav` still works. Exit
status is 0 if everything rendered, 1 if some track failed, 2 for a command
line problem.

### Building on Linux

    mkdir build && cd build
    cmake .. -DBUILD_GSF2WAV=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DUSE_FFMPEG=OFF
    make gsf2wav

This needs zlib. The binary is `build/gsf2wav/gsf2wav`. `gsftrace`, the
reverse-engineering aid, is built alongside it.

### Building for Windows

`tools/build-windows.sh` cross-compiles a 64-bit `gsf2wav.exe` with MinGW-w64
(the apt packages are listed at the top of the script) and zips it with a
plain-text README, the license and the overrides files. The exe is statically
linked, so it needs only DLLs every Windows installation has. I tested it by
running the full test suite and the 37-case comparison against the Linux build
under Wine (`GSF2WAV_RUNNER=wine64 python3 test/run.py gsf2wav.exe`); the audio
came out byte-for-byte identical. **It hasn't been run on a real Windows
machine.** Windows-specific code is in `platform.c`: UTF-8 file names (Japanese
rip folders), both path separators, wildcard expansion (cmd.exe leaves that to
the program), and a "press Enter" pause when the program was started from
Explorer and would otherwise close its window before anyone could read it.

### Options

`gsf2wav --help` prints this (it's generated from the same table the parser
uses). The `--mp2k-mix`, `--mp2k-bandwidth`, `--mp2k-source-cutoff`,
`--mp2k-linear-samples` and `--mp2k-verify` names from before the AlphaDream
driver existed are still accepted.

    usage: gsf2wav [options] INPUT...

    Renders Game Boy Advance music rips (.minigsf / .gsf) to WAV. INPUT is a file,
    a folder of them, or a wildcard. Each WAV goes next to its input unless -o
    says otherwise. TIME is seconds or [h:]m:ss[.fff].

    Examples:
      gsf2wav "01 Title.minigsf"
      gsf2wav -o wav/ --mix blam rips/mother3/
      gsf2wav -b 24 -g -1 -j 4 --overrides levels.txt *.minigsf

    Output:
      -o, --output PATH         folder for the WAVs (made if missing), or the file
                                name for a single input
      -r, --rate HZ             sample rate (default 48000)
      -b, --bits FMT            32f (float, the default), 24 or 16; integer
                                formats get TPDF dither
      -l, --length TIME         play time before the fade (default: the file's
                                length tag, else 150 s)
      -f, --fade TIME           fade-out time (default: the file's fade tag, else
                                10 s)
      -g, --gain DB             extra gain in dB; negative turns it down
          --no-volume-tag       ignore the file's volume tag

    Sound:
          --no-hifi             play the game's own mix instead of re-rendering
                                its voices
          --mix MODE            how each voice is resampled when re-rendering:
                                sinc (default): from its source straight to the
                                output rate
                                linear: the driver's own method, at its mixing
                                rate, without its 8-bit loss
                                lerp: linear interpolation at the output rate, not
                                bandlimited
                                blam: linear interpolation, bandlimited to the
                                output rate
          --bandwidth HZ        cap each voice's bandwidth; "driver" means the
                                driver's own Nyquist
          --source-cutoff F     each voice's cutoff as a fraction of its own
                                playback rate (default 0.47)
          --linear-samples LIST
                                render these samples (hex header addresses,
                                comma-separated) the way the
                                driver does, whatever --mix says
          --ramp MS             volume change and note cut smoothing (default 2; 0
                                = the driver's steps)
          --fifo-hold           play DirectSound as the hardware's sample-and-hold
                                instead of
                                sinc-interpolating it
          --psg-grid CYCLES     PSG sampling grid in CPU cycles (default 8,
                                already exact)
          --bios FILE           use a real GBA BIOS instead of mGBA's built-in one

    Samples (rips are missing bytes of the game's sample data):
          --sample-rom FILE     the full game ROM the rip came from: sample data
                                is read from it, which
                                recovers the bytes the ripper never saw (the game
                                still runs from the rip)
          --no-fill-holes       read samples exactly as the rip has them; normally
                                zero bytes (never read
                                while ripping) are filled in from their neighbours

    Batches:
          --overrides FILE      per-track options: one line per track, "name
                                options..." (see the README)
          --skip-existing       don't render tracks whose WAV already exists
      -j, --jobs N              render N tracks at once (default: one per core, at
                                most 8)
      -q, --quiet               only warnings and errors
      -v, --verbose             also print driver, timing and sample details

    Diagnostics:
          --mute LIST           silence psg, pcm, or driver channels 0-11, e.g.
                                psg,0,3
          --solo-sample ADDR    only voices playing the sample at hex header
                                address ADDR (mutes the PSG)
          --sample-stats        print which samples played: notes, seconds, mean
                                and max playback rate, max gain
          --verify              check the driver port against the game's own
                                mixer, every frame

    Other:
          --version             print the version and where the source is
      -h, --help                this text

An overrides file (`--overrides`) gives per-track options. Each line is a
track's file name without its extension, or just its first word (usually the
track number), then options; the whole name wins over the first word, `#`
starts a comment, and a name with spaces goes in quotes. A track's options are
applied after the command line's, so they can override it. The files I've
made are `tools/data/mother3_overrides.txt`, `wl4_overrides.txt` and
`mlss_overrides.txt`; they mostly just lower levels so lossy encoding doesn't
clip. The whole file is checked before anything renders.

The BIOS barely mattered for Mother 3 in what I tried. Over 60 s of three songs
it only calls `Halt`, `Div`, `CpuSet`, `CpuFastSet` and `LZ77UnCompVram`, all of
which mGBA's built-in BIOS should reproduce. It never calls the BIOS's own
sound routines.

Making Opus files (needs `opusenc`) is a separate step, which
`tools/render_set.py` does for a whole set (tags from the rip, one zip):

    python3 src/platform/gsf2wav/tools/render_set.py build/gsf2wav/gsf2wav rips/mother3 OUT_DIR \
        --bitrate 96 --overrides src/platform/gsf2wav/tools/data/mother3_overrides.txt \
        -- -r 48000 -b 32f --mix blam


Status and known issues
-----------------------

Things I know are wrong or unfinished, and things I don't know:

- **Only the SDK 3.0 mixer revision is ported.** Later revisions added
  compressed and reversed samples, which will render wrong. Run
  `--verify` on a new game first; it reports any frame where the port and
  the game disagree. I've only done that on three games.
- **I haven't compared any of this against real hardware,** and the listening
  I've done is mine alone and not blind. The sound-quality claims above should
  be read as "this is what I heard", not as findings.
- **Rips have holes in sped-up samples.** GSF rippers zero every ROM byte the
  game didn't read while being ripped, and a sample played well above its
  recorded rate is only read every few bytes. The driver never reads the
  holes, but sinc and its relatives read every byte, so they hear the gaps.
  By default zero bytes in a sample are treated as missing and filled by
  interpolating their neighbours (`--no-fill-holes` turns that off). This is a
  heuristic: a genuine zero in a sample gets filled too, which is probably
  harmless but I haven't proven it. `--sample-rom FILE` reads the real data
  from a full ROM of the same game (it must match every byte the rip kept,
  apart from a few patches the ripper made). How many samples this affects:
  - Mother 3: 6 of the 306 samples the songs use have holes where they're
    played, in 9 tracks (006, 020, 071, 101, 113, 152, 179, 203, 244). Filling
    gets most within −24 to −38 dB of the real data; one sample in 203, played
    at up to 16×, is too sparse to fill (−3.6 dB) and needs the ROM.
  - Superstar Saga: 12 of 59 samples, in 16 tracks. Its samples are unsigned,
    so a zeroed byte reads as a full-scale negative spike, and the damage
    sounds like distortion, not grit.
  - Wario Land 4: 3 of 337, in 6 track numbers (006, 011, 020, 033, 084, 095;
    006 is three files, 006a–c).
  - `tools/rom_holes.py` does this survey for any set, given its ROM.
  - This is also what I'd wrongly blamed on the waveform for Mother 3's 006
    organ. `--linear-samples` is still there for taste: low-pitched voices
    still differ audibly between modes.
- **Tracks can clip.** Float output keeps overs (Mother 3's unused Giygas
  battle track peaks at +2.2 dBFS). Lossy encoding also overshoots: Opus pushed
  three Mother 3 tracks with float peaks of −0.07 to −0.33 dBFS past 0 dBFS once
  decoded. Keep peaks around −1 dBFS with `-g` (per track, via an overrides
  file), and check by decoding (`opusdec --float`).
- **Lengths come from tags.** There's no loop-count option. The robust way
  would be reading the sequencer's GOTO commands; `tools/find_loop.py` finds
  loop periods from audio but not loop starts.
- **Late timing lock.** The re-render holds output until it finds the driver's
  first audible frame in the FIFO stream. It gives up after about 15 s and
  falls back to the captured stream.
- **Not a ripping tool,** and not a replacement for anything. It renders
  existing rips; it doesn't make them.

### Not started: live playback in the libretro core

The same rendering could in principle run during gameplay in RetroArch's mGBA
core. I haven't started on this, and everything below is guesswork from
reading the code, not experience with libretro:

- `retro_run` in `src/platform/libretro/libretro.c` calls `core->runFrame()`,
  then drains the core's `mAudioBuffer` into `audioCallback`. A hi-fi path
  would install the audio observer and the code hook when the game loads, and
  read the band-limited mixer instead.
- The driver mixes each frame about one frame before the hardware plays it, so
  the re-render shouldn't be behind real time, but the renderer's look-ahead
  adds latency (I'd guess about 30 ms with the current kernels).
- Things that would need work: playing normal audio until the lock lands,
  falling back when the mix stops matching (loading screens, soft resets),
  savestates and rewind (the renderer's state isn't serialized), fast-forward,
  and core options.


Working on this (people and agents)
-----------------------------------

**The repository is public, so never commit ROM-derived data:** no rips,
renders, disassembly listings or extracted ROM images. `rips/` at the repo root
is gitignored for GSF sets. `src/platform/gsf2wav/tools/fetch_set.py URL [name]`
downloads and extracts a set into it. Sample addresses and statistics
(`tools/data/mother3_sample_stats.csv`) are fine. Please don't post full
renders of copyrighted soundtracks either; short clips for comparison are a
judgement call.

**Checks to run before trusting a change** (paths from the repo root):

1. `python3 src/platform/gsf2wav/test/run.py build/gsf2wav/gsf2wav` for the
   synthetic suite (needs clang with the ARM target, ld.lld, llvm-objcopy and
   numpy). The first five checks are about the audio; the rest exercise the
   command line on the synthetic GSFs (folders, `-o`, overrides, `-j`, errors,
   non-ASCII names). All 27 should pass. Set `GSF2WAV_RUNNER=wine64` to run the
   Windows exe under Wine.
2. `--verify` on a few tracks of the game you're working on. It should
   report zero differing frames. Any mixer change must keep this true.
3. A sweep across the set. For example,
   `python3 src/platform/gsf2wav/tools/batch.py build/gsf2wav/gsf2wav rips/mother3 --every 3 --grep high-precision -- -l 45 -f 0`
   should report every track locked, with 0 resyncs, 0 samples lost and 0
   fade-outs dropped.
4. For changes that affect sound: compare renders with the tools below, and
   A/B by ear. The measurements caught real bugs, but every time the last word
   on whether something sounded right was a listening test.

**Tools** (`tools/`):

| Script | Purpose |
|---|---|
| `build-windows.sh`, `package_windows.py` | Cross-build `gsf2wav.exe` with MinGW-w64 and zip it with its docs |
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
| `sample_noise.py` | Heuristic sample-noisiness ranking (didn't find the organ) |
| `rom_holes.py` | Which samples a rip has holes in, measured against the full ROM |
| `gsfpy.py` | Minimal Python PSF loader used by the above |

**Finding an unknown driver** (`build/gsf2wav/gsftrace`, built with gsf2wav):
`gsftrace GAME.minigsf` prints the sound DMAs' source buffers; `--watch LO-HI`
lists the code addresses writing a memory range; `--regs ADDR[t] N` prints the
registers the first N times code at a ROM address runs; `--dump DIR` saves
IWRAM, EWRAM and the ROM image (keep them out of git). A new driver then needs
a backend for the renderer (`struct HiFiDriver` in `hifi.h`: its voices
each frame, its exact output, its own algorithm in floating point);
`alphadream.c` is a compact example. This is how I found AlphaDream's driver,
and it may not generalize.

Code follows mGBA's style (tabs, `CamelCase` types and functions,
underscore-prefixed static helpers, MPL-2.0 headers). The history is in small
commits, most with a message explaining what was measured.


Credits and things I read
-------------------------

**Provenance, plainly:** this isn't a clean-room implementation. While writing
the mixer port, Claude and I had pret's decompiled MP2K driver open, plus a
disassembly of each game's own driver, and the struct layouts and many field
names in `mp2k.h` are the ones pret uses. The AlphaDream driver was worked out
from a disassembly of the game. I haven't checked the licenses of pret's
repository (as far as I know it's a decompilation of Nintendo's code with no
license granting reuse). No code was copied from any project listed below, but
I can't rule out having reproduced structure from memory, and I'd rather say so
than have anyone find out. If any of this is a problem for the people whose work
it builds on, please tell me and I'll change it.

Thank you to:

- **The mGBA developers**, especially endrift. This whole thing is a fork of
  their emulator and runs on its GBA core. mGBA is MPL-2.0, and the additions
  here are under the same license. The name "mGBA" belongs to them.
- **The people who ripped the GSF sets I tested on**, and everyone in the
  ripping community who worked out the format and the tools. I downloaded the
  rips from https://gsf.joshw.info/, and I don't know who to credit for each
  one (they'll be in each set's readme and tags).
- **kode54**, for psflib (`_lib` load order,
  https://github.com/kode54/psflib) and lazygsf
  (https://buffering.party/software/lazygsf/), which I read to see how a
  GSF player is put together. No code from either is included.
- **pret**, for the MP2K decompilation
  (`src/m4a_1.s` and `include/gba/m4a_internal.h` in
  https://github.com/pret/pokeemerald). It's a later revision than Mother 3's,
  and `mp2k.c` follows Mother 3's own disassembly where they differ.
- **The mGBA "XQ" mixer** (`src/gba/extra/audio-mixer.c` in mGBA 0.8–0.10),
  which I read for background. It's not included, and this tree doesn't have
  it.
- **The 2SF player resampler** with BLEP/BLAM modes
  (https://github.com/yshui/2sftowav/blob/master/src/vio2sf/desmume/resampler.c),
  which I read and measured to compare behaviour. I don't know its license and
  no code from it is used here.

Mistakes in this README and the code are mine (and Claude's), not theirs.
