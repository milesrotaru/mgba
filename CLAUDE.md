# Notes for agents working in this fork

This is a fork of mGBA whose only purpose is **gsf2wav**: a command-line tool
that renders GBA music rips (GSF / minigsf) to WAV, and for two sound drivers
(Nintendo's MP2K, and AlphaDream's in Mario & Luigi: Superstar Saga) re-renders
the driver's voices at higher resolution than the game's own mix. Everything
is in `src/platform/gsf2wav/`. Its `README.md` is the full technical record
(design, results per game, mistakes, options, status, credits); read it before
changing anything substantial. This file is the short version, plus the
working knowledge that doesn't belong in a user-facing README.

## Hard rules

- **The repository is public. Never commit ROM-derived data:** no rips,
  renders (WAV/Opus), disassembly listings, memory dumps, extracted ROM
  images or ROMs. `rips/`, `build-windows/` and `dist/` are gitignored. Sample
  addresses and statistics (`tools/data/*.csv`) are fine. Check `git status`
  for stray files before every commit.
- **Don't download ROMs.** The maintainer owns the games and uploads their
  own dumps when a task needs them. Rips come from https://gsf.joshw.info/
  via `tools/fetch_set.py`.
- **Don't crawl gsf.joshw.info.** Its robots.txt is `Disallow: /` for all
  agents. Fetch only specific sets the maintainer asks for, one at a time; no
  bulk downloads or index scraping without the site owner's permission.
  Check robots.txt before automated fetches from any site.
- **Work on `master`** and push to `origin master`. Don't open pull requests
  unless asked.
- **Tone of the docs is deliberately humble.** The maintainer is a hobbyist,
  not an emulator developer or ripper, and is wary of overclaiming to that
  community. Hedge sound-quality claims as what was heard, not findings.
  Never invent facts, people, credits or listeners; if something isn't known,
  say so. The provenance note (not clean-room; pret's names in `mp2k.h`)
  stays.
- New files carry the MPL-2.0 header ("Copyright (c) 2026 gsf2wav
  contributors"). Code follows mGBA style: tabs, `CamelCase` types and
  functions, `_underscore` static helpers.
- Changes to upstream mGBA files stay additive and minimal (see below).

## Layout

Upstream files touched (additions only): `include/mgba/internal/gba/audio.h`
and `src/gba/audio.c` (`GBAAudioObserver`), `include/mgba/internal/gba/gba.h`
and `src/gba/gba.c` (`GBAInstallCodeHook`), root `CMakeLists.txt`
(`BUILD_GSF2WAV`, and the Windows epoxy check relaxed for gsf2wav-only
builds), root `README.md` (fork notice), `.gitignore`.

`src/platform/gsf2wav/`:

| File | Role |
|---|---|
| `main.c` | Inputs (files, folders, wildcards), output naming, worker pool, report lines, exit codes 0/1/2 |
| `options.c/.h` | Table-driven options (`OPTIONS[]`); help text is generated from it; `RenderOptions` (per track) vs `RunOptions` (per run) |
| `overrides.c/.h` | Per-track option files: `KEY options...`, key = full stem (quoted) or first word; validated at load |
| `render.c/.h` | `RenderTrack()`: one track end to end, thread-safe, cleans up on every path |
| `capture.c/.h` | Audio observer: PSG on an 8-cycle grid (BLEP), DirectSound points (sinc) or hold |
| `hooks.c/.h` | Code hooks that catch each driver mid-frame, for the renderer and `--verify` |
| `hifi.c/.h` | Driver-neutral re-renderer (`struct HiFiDriver` backend), timing lock, mix modes, hole filling; also the MP2K backend |
| `mp2k.c/.h` | Bit-exact C port of Mother 3's `SoundMainRAM` (SDK 3.0 revision) |
| `alphadream.c/.h` | AlphaDream driver: detection, frame read, exact and float mixers, backend |
| `blmix.c/.h` | Band-limited step/point renderer (Kaiser, ~120 dB) |
| `psf.c/.h` | PSF/GSF loader, `_lib` chains, case-insensitive lib lookup |
| `wav.c/.h` | Streaming WAV writer, float or 24/16-bit with TPDF dither |
| `platform.c/.h` | OS layer: UTF-8 paths on Windows, dirs, wildcards, console pause |
| `msglog.c/.h` | Thread-local message log so parallel renders don't interleave |
| `gsftrace.c` | Reverse-engineering aid for unknown drivers |
| `test/run.py` | Synthetic suite (27 checks: 5 audio, 22 CLI); builds its own test ROM |
| `tools/` | Python helpers; table in the README. `regress.py` is the real-rip regression check |
| `tools/data/` | Per-set level overrides (`*_overrides.txt`) and sample statistics |
| `packaging/windows/README.txt` | Template for the Windows zip's README |
| `cmake/mingw-w64-x86_64.cmake` | Cross toolchain for the Windows build |

## Build

Linux (needs zlib):

    cmake -S . -B build -DBUILD_GSF2WAV=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DUSE_FFMPEG=OFF
    make -C build gsf2wav gsftrace        # -> build/gsf2wav/gsf2wav, build/gsf2wav/gsftrace

Windows (cross-compiled, static): `src/platform/gsf2wav/tools/build-windows.sh
[BUILD_DIR [OUT_DIR]]` (defaults `build-windows/`, `dist/`). The apt packages
are listed at the top of the script. It produces
`dist/gsf2wav-VERSION-windows-x64.zip` and prints SHA-256s. The version string
comes from `git describe --always --dirty` at configure time, so commit
before building anything you ship.

Sanitizer builds used so far (separate build dirs, `CMAKE_BUILD_TYPE=Debug`):
`-fsanitize=thread -O1 -g`, and `-fsanitize=address,undefined
-fno-omit-frame-pointer -O1 -g` (same flags in `CMAKE_EXE_LINKER_FLAGS`).
Both were clean on the test suite and multi-job batch runs.

## Verify a change

1. `python3 src/platform/gsf2wav/test/run.py build/gsf2wav/gsf2wav`: all 27
   must pass. Needs clang with the ARM target, ld.lld, llvm-objcopy, numpy.
2. `--verify` on a few tracks of each driver: zero differing frames.
3. With the rips present: `tools/regress.py run OLD_EXE old/`,
   `tools/regress.py run NEW_EXE new/`, `tools/regress.py diff old new`. A
   refactor must leave all 37 cases byte-identical. Sound changes should
   touch only the cases they're meant to. To get "old", build the previous
   commit in a separate build dir (e.g. a `git worktree`).
4. Windows: run 1 with `GSF2WAV_RUNNER=/usr/lib/wine/wine64` and 3 with
   `--wine`, against the exe from the zip. In the cloud container Wine 9 is
   at `/usr/lib/wine/wine64` and not on PATH (regress.py uses that path unless
   `WINE` is set). Use a `WINEPREFIX` in a scratch dir, `WINEDEBUG=-all`,
   `LANG=C.UTF-8`. Only Wine has been tested, never real Windows.
5. For sound: `tools/compare.py`, `bands.py`, `clicks.py`, then listen.

## Rips and ROMs (not in git; re-create in a fresh container)

    rips/mother3/  Mother 3 (2006-04-20) set, 276 tracks, lib AGB-A3UJ-JPN.gsflib
    rips/wl4/      Wario Land 4 (2001-08-21) set, 113 tracks
    rips/mlss/     Mario & Luigi: Superstar Saga (2003-11-17) set, 51 tracks
    rips/roms/     "Mother 3 (Japan).gba", "Wario Land 4 (USA, Europe).gba",
                   "Mario & Luigi - Superstar Saga (USA).gba" (maintainer's dumps)

`tools/fetch_set.py URL [name]` fetches a set from gsf.joshw.info (needs
py7zr). The ROMs must come from the maintainer.

## How the delivered renders were made

All three sets were delivered as Opus 96 kbps zips, rendered with:

    python3 src/platform/gsf2wav/tools/render_set.py build/gsf2wav/gsf2wav rips/SET OUT \
        --overrides src/platform/gsf2wav/tools/data/SET_overrides.txt \
        -- -r 48000 -b 32f --mix blam --sample-rom "rips/roms/GAME.gba"

The overrides files only lower levels so decoded Opus peaks stay around
−1 dBFS. They were tuned from decoded Opus peaks (`opusdec --float`), not WAV
peaks, because Opus overshoots. Re-tune them if the mix changes.

Delivered so far: full Mother 3 and WL4 sets in blam (before `--sample-rom`
existed), then ROM-repaired replacements for only the affected tracks (M3: 006
020 071 101 113 152 179 203 244; WL4: 006a–c 011 020 033 084 095), and a
complete Superstar Saga pack with `--sample-rom`.

## Things that bit us (read before debugging)

- **Rip holes.** GSF rippers zero every ROM byte the game didn't read. Sped-up
  samples get read every few bytes, so the rip has zeroed gaps. The driver
  never reads them, but the hi-fi resamplers read every byte. Default: zero
  bytes are treated as missing and interpolated (`--no-fill-holes` to
  disable). `--sample-rom` reads real data from a ROM (rejected if more than
  4096 kept bytes differ). For AlphaDream's unsigned samples a hole is a
  full-scale spike (the MLSS 05 "distorted high notes"). Mother 3's 006 organ
  grit was the same thing; it was first misattributed to the waveform.
  `tools/rom_holes.py` surveys a set against its ROM.
- **Zeroed code in rips.** Paths not taken during ripping are `movs r0,r0`.
  In MLSS the envelope's decay branch is gone (decay goes straight to
  sustain); the port follows the rip.
- **`.gsflib` size field** in the MLSS rip counts its 12-byte header; the
  loader loads what exists and zero-fills.
- **AlphaDream channels released past a non-looping sample's end** must be
  reported as off, or the timing lock resyncs.
- **MinGW name clashes:** MSVCRT already defines `_expand` and `_dup`; static
  helpers with those names broke the Windows build. `CommandLineToArgvW` needs
  `<shellapi.h>`.
- **Static Windows link:** CMake's MinGW platform prefers `libz.dll.a`; pass
  `-DZLIB_LIBRARY=/usr/x86_64-w64-mingw32/lib/libz.a` (the script does).
  Don't use the bundled zlib: its build renames `src/third-party/zlib/zconf.h`
  in the source tree (`git checkout` it back if that happens).
- **After a failed build, a stale exe stays in place.** Check timestamps or
  `--version` before trusting a test run.
- **Outputs are written as `NAME.wav.part`** and renamed on success, so
  `--skip-existing` is safe to resume with.
- Monitor/background tasks running `tail -F` never exit on their own; stop
  them when done.

## Driver facts (for orientation; details in the README and headers)

- MP2K: found via its literal pool (`0x03007FF0`, `'Smsh'`); hook at the
  `SoundMainRAM` entry. Mother 3: 15768 Hz, 264 samples/frame, 12 channels.
  WL4: 13379 Hz, 224/frame, 8 channels, `maxLines` 70 (not modeled; never
  triggered).
- AlphaDream (MLSS): mix routine at `0819A2F0` (Thumb), channels at
  `03001B50` (8 × 16 bytes), sample table at `08A806B8`; unsigned 8-bit,
  nearest-sample with a 10-bit fraction, 15768 Hz, 264/frame; FIFO A = left,
  B = right; packed L/R volume; output wraps. Detection is ~30 signature bytes
  (`_mixHead`/`_mixBody`).
- Mix modes (`--mix`): `sinc` (default), `linear` (the driver's algorithm in
  float), `lerp` (unbandlimited), `blam` (linear-interpolation triangle
  convolved exactly with a Kaiser-windowed sinc lowpass). The maintainer
  prefers `blam`.

## Driver databases (which games use which driver)

Checked 2026-10; nothing from them is committed (licenses unclear).

- VGMPF "Game Boy Advance sound drivers" (wiki, by title): the best
  hand-curated map. Lists MLSS correctly under AlphaDream, plus three more
  AlphaDream-driver games (Hamtaro: Ham-Ham Games, Hamtaro: Rainbow Rescue,
  Minna no Soft Series: Happy Trump 20), the first candidates for
  `alphadream.c`.
- loveemu's AGBinator CSV (gist, by serial) and the granbone
  mgba-fixed-audio-tempo compatibility DB (by ROM hash): automated scanners.
  Both call MLSS MP2K, because the ROM links MP2K's library (VGMPF says for
  its Mario Bros. minigame; unconfirmed, since no rip track runs it). A
  per-ROM "driver" isn't necessarily the music's driver.
- caitsith2's ofslist.txt: MP2K song table offsets by serial, up to release
  #1894 (no Mother 3). WL4's entry checked out against the ROM. Useful for
  a future loop-count feature.
- None records the `SoundMainRAM` mixer revision, which is what decides
  whether `mp2k.c` matches a game. Only the game's code answers that:
  `tools/mp2k_fingerprint.py` statically (core hash `5bfa0c4bba80` is the
  verified revision; M3, WL4 and MLSS's minigame copy all have it), then
  `--verify` for certainty. Not yet tested on a revision that fails
  `--verify`; the README says pret's pokeemerald driver is a later
  revision, so a Pokémon Emerald set would be the obvious negative control.

## Open items (none in progress)

- Rename `MP2KChannel` fields away from pret's names.
- Replace AlphaDream's signature bytes with a structural match.
- Loop-count option (read the sequencer's GOTO; `find_loop.py` only finds
  periods).
- Later MP2K mixer revisions (compressed/reversed samples) are unported.
- Live playback in the libretro core: assessed in the README, not started.
- Test on real Windows; code signing.
