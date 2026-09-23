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

Limits
------

For games using Nintendo's MP2K ("Sappy") driver, the DirectSound stream is
already the driver's own software mix: 8-bit, at the driver's mixing rate,
with its own low-precision resampling of each voice. gsf2wav reproduces
that stream as cleanly as it can be reproduced, but it can't recover detail
the driver discarded. Going past that ceiling needs a high-level re-mix of the
driver's voices.

Tests
-----

`test/run.py path/to/gsf2wav` builds synthetic GSFs (needs clang with the ARM
target, ld.lld, llvm-objcopy and numpy), renders them and checks aliasing,
passband flatness, the hold-mode image level and `_lib` loading.
