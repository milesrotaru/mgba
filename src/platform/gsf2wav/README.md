gsf2wav
=======

Renders GSF / minigsf rips to WAV using mGBA's GBA core, taking the audio
straight from the DAC inputs instead of the core's playback path.

Building
--------

    cmake -DBUILD_GSF2WAV=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF ..
    make gsf2wav

Requires zlib.

Usage
-----

    gsf2wav [options] INPUT.minigsf OUTPUT.wav

    -r, --rate HZ         output sample rate (default 48000)
    -b, --bits FMT        32f, 24 or 16 (default 32f; integer formats are TPDF dithered)
    -l, --length TIME     play length before fade, overrides the length tag
    -f, --fade TIME       fade length, overrides the fade tag
    -g, --gain DB         extra gain in dB
        --no-volume-tag   ignore the volume tag
        --fifo-hold       reconstruct DirectSound with sample-and-hold, like the hardware
        --psg-grid CYCLES PSG sampling grid in CPU cycles (default 8)
        --bios FILE       use a real GBA BIOS instead of the built-in HLE one
        --no-hifi         for MP2K games, output the driver's own mix instead of re-rendering its voices
        --mp2k-mix MODE   sinc (default) or linear; see below
        --mp2k-bandwidth HZ limit each voice's bandwidth in sinc mode ("driver" = the driver's Nyquist),
                          so sample grit the game's mixing rate hid doesn't come through
        --mp2k-source-cutoff F  each voice's cutoff as a fraction of its own playback rate (default 0.47)
        --ramp MS         MP2K volume change and note cut smoothing, sinc mode (default 2 ms; 0 = as the driver)
        --mute LIST       silence sources: psg, pcm, or MP2K channel numbers 0-11, e.g. psg,0,3
        --solo-sample ADDR  only MP2K voices playing the sample whose header is at hex ADDR (mutes PSG)
        --mp2k-verify     check the MP2K mixer port against the game's own mixer

`_lib` chains are loaded in psflib order (`_lib`, the file itself, then
`_lib2`, `_lib3`, ...) and lib names are matched case-insensitively.

How the audio is produced
-------------------------

mGBA's normal audio path samples the mixer on a grid set by the game's
SOUNDBIAS resolution (usually 32768 Hz), clamps to the 10-bit DAC range and
then resamples. gsf2wav bypasses all of that through a small observer hook in
`GBAAudio`:

- **DirectSound (FIFO A/B)**: every sample is captured with the exact cycle at
  which the FIFO latched it. By default the stream is treated as what it is, a
  PCM signal at the game's timer rate, and is sinc-interpolated at the output
  instants with the lowpass at the lower of the two Nyquist frequencies. That
  removes the zero-order-hold images the real DAC produces (about -22 dB for a
  1 kHz tone at 13379 Hz). `--fifo-hold` keeps the hold instead, rendered
  band-limited, which is what the hardware does.
- **PSG (channels 1-4)**: the core evaluates the PSG lazily and the hook fires
  before every register change, so the PSG can be walked on a fine grid
  (8 cycles by default: the wave channel's timer granularity, and a divisor
  of the square channels' 16) and every level change rendered as a
  band-limited step.

Both paths are evaluated in continuous time directly at the output sample
instants: there's no intermediate sample rate. The kernel is a Kaiser-windowed
sinc with 48 zero crossings and ~120 dB stopband. Mixing is in double precision
with no clamping; output defaults to 32-bit float, and the peak level is
reported.

MP2K games: re-rendering the driver's voices
--------------------------------------------

Most GBA games use Nintendo's MP2K ("m4a", "Sappy") sound driver, which mixes
all its PCM voices in software each frame: linear interpolation to a fixed
mixing rate (often 13379 or 15768 Hz), each voice's contribution floored to
8 bits, summed in bytes that wrap instead of clipping. The DirectSound stream
is that mix, so the best a capture can do is reproduce it cleanly: its
bandwidth stops at the mixing rate's Nyquist, and its noise floor is the 8-bit
truncation. On Mother 3 that truncation alone costs about 25 dB of
signal-to-noise on a typical track.

When gsf2wav finds the driver (by the literal pool of `SoundMain`), it hooks
the jump from `SoundMain` into `SoundMainRAM`. At that point the sequencer has
run for the frame and the mixer hasn't, so the driver's channel structs say
exactly what is about to be mixed. `mp2k.c` is a literal C port of the SDK 3.0
`SoundMainRAM`; `--mp2k-verify` runs it on a copy of the driver state every
frame and diffs the result against what the game writes. On Mother 3 it is
bit-exact: 23 tracks x 60 s, 43 million samples, zero differences.

From that state, `mp2k_hifi.c` renders the voices itself:

- `--mp2k-mix sinc` (default): each voice is resampled straight from its
  source PCM to the output rate with a windowed sinc, bandlimited to both its
  own Nyquist and the output's, at exactly the positions and pitches the
  driver uses. Volumes aren't truncated and nothing wraps. Envelope and volume
  steps, which the driver applies once per frame, are ramped over `--ramp`
  milliseconds, and cut-off voices fade out over the same time instead of
  stopping dead. New notes are rendered from a few milliseconds before they
  start so the sinc's pre-ringing isn't truncated. Voices pitched above the
  mixing rate keep the high end the driver's mix had no room for.
- `--mp2k-mix linear`: the driver's own algorithm, linear interpolation at the
  mixing rate, just without its 8-bit truncation. Useful as a reference: it
  differs from the driver's output only by that truncation.

The driver's reverb (a mono feedback echo one DMA period and one period less a
frame back) is modeled in both modes. PSG channels come from the hardware
emulation as usual.

Timing comes from the hardware. The driver's frames play back-to-back on the
FIFO timer, so gsf2wav finds the first audible frame's exact mix in the
captured FIFO stream once, then places every frame where the hardware played
it. If it can't lock (or the driver isn't MP2K, or is reconfigured
mid-song), it falls back to the captured stream.

Known limits: only the SDK 3.0 mixer revision is ported so far (no compressed
or reversed samples, which later revisions added); `--mp2k-verify` will say if
a game's driver disagrees with the port.

Tests
-----

`tools/` has the scripts used during development: a Python GSF loader, the
MP2K signature scanner, a batch runner, and comparison tools (level/lag/
residual between two renders, octave bands, click detection).
`tools/fetch_set.py URL` downloads a GSF set into the gitignored `rips/`.

`test/run.py path/to/gsf2wav` builds synthetic GSFs (needs clang with the ARM
target, ld.lld, llvm-objcopy and numpy), renders them and checks aliasing,
passband flatness, the hold-mode image level and `_lib` loading.
