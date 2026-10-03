gsf2wav for Windows
===================

Version @VERSION@
Source: https://github.com/milesrotaru/mgba (commit @COMMIT@)

gsf2wav turns Game Boy Advance music rips (.minigsf and .gsf files) into WAV
files. For a few games it can also re-render the game's instruments at a
higher resolution than the Game Boy Advance's own mixer managed.

This is a hobby project, not a polished or authoritative one. Most of the code
was written with an AI assistant, and it has only been tried on three games
(Mother 3, Wario Land 4 and Mario & Luigi: Superstar Saga). README-technical.md
says what has and hasn't been checked. If you know this area and spot a
mistake, I'd like to hear about it.


QUICK START
-----------

1. Put the rips in one folder. A .minigsf file isn't music by itself: it points
   to a library file (.gsflib or .minigsflib) that has to be in the same
   folder.

2. Drag the .minigsf files, or the whole folder, onto gsf2wav.exe.

3. A window shows progress. The WAV files appear next to the originals,
   named after them. Press Enter when it says it's done.

The first time you run it, Windows may say "Windows protected your PC". The
program isn't code-signed. Choose "More info", then "Run anyway", if you trust
where you got it. (Or build it yourself; the source is linked above.)


FROM A TERMINAL
---------------

Open Command Prompt or PowerShell in the folder with gsf2wav.exe (in
PowerShell, type .\gsf2wav.exe).

    gsf2wav "C:\Music\Mother 3"                  every track in the folder
    gsf2wav "C:\Music\Mother 3\*.minigsf"        the same, with a wildcard
    gsf2wav -o C:\Out "C:\Music\Mother 3"        put the WAVs in C:\Out
    gsf2wav --mix blam -b 24 -o out rips         pick the resampling, 24-bit
    gsf2wav -j 4 --overrides overrides\mother3_overrides.txt -o out rips
    gsf2wav --help                               every option

By default it renders several tracks at once (one per processor core, up to
8) and prints one line per finished track. --skip-existing picks up a big job
where it left off: a WAV only appears once it's complete.

Lengths and fades come from the tags inside each rip; -l and -f override them.


WAV FILES ARE BIG
-----------------

The default is 32-bit float, 48 kHz stereo, about 70 MB for a three-minute
track, because that keeps everything the renderer produced. -b 24 or -b 16
makes smaller files. To make small files for a phone, convert afterwards. With
ffmpeg installed:

    for %f in (*.wav) do ffmpeg -i "%f" -c:a libopus -b:a 96k "%~nf.opus"

(In a .bat file, write %%f and %%~nf.)

Lossy encoders can overshoot full scale and clip on loud tracks. gsf2wav tells
you when tracks come within 1 dB of full scale. Turn them down with -g (for
example -g -1.5), or per track in an overrides file.


WHAT --mix MEANS
----------------

For games it knows how to re-render, --mix chooses how each instrument's
recording is resampled:

    sinc     careful, textbook resampling (the default)
    linear   the game's own method, without its 8-bit rounding
    lerp     linear interpolation at the output rate: bright, with some
             aliasing on high notes
    blam     linear interpolation, band-limited: keeps lerp's brightness on
             low notes but not the aliasing on high ones (the one I listen to)

Which sounds best is a matter of taste, and I only have my own ears to go on.
Try one track in each and listen.


OVERRIDES
---------

An overrides file sets options for particular tracks. Each line is a track's
number (the first word of its file name) or its whole name in quotes, then the
options for that track:

    # levels, so loud tracks don't clip when encoded
    001 -g -1.0
    "99 Unknown Song 3" -g -0.5

The overrides folder has the ones I made for the rips I tested. They're named
for those rips' file names, so they won't suit other sets.


RIPS WITH MISSING SAMPLES
-------------------------

Rippers only keep the bytes of a game a song actually read, so some
instruments in a rip have gaps in them, which can sound like grit or
distortion once they're played back at a different pitch. By default gsf2wav
fills the gaps in from their neighbours. If you have the game's ROM (the same
game and version the rip came from), --sample-rom "C:\path\game.gba" reads the
real data from it instead. gsf2wav checks that the ROM matches the rip, and
refuses it if it doesn't.


IF SOMETHING GOES WRONG
-----------------------

"could not open _lib": the .gsflib/.minigsflib file the track points to isn't
in the same folder.

"unrecognised sound driver": the game isn't one gsf2wav knows how to
re-render. It still renders the game's own audio, just without the extra
steps, so --mix and the other re-rendering options have no effect.

"-v" adds technical detail to each track's output, which helps if you report
a problem.


ABOUT THIS BUILD
----------------

gsf2wav is built on the mGBA emulator's Game Boy Advance core (by endrift and
the mGBA contributors) and also contains zlib. It's free software under the
Mozilla Public License 2.0 (LICENSE.txt). The source for this exact version is
at the address at the top of this file.

This .exe was cross-compiled on Linux with MinGW-w64, and tested by running it
under Wine (the full test suite, and a comparison of its audio with the Linux
build, which was byte-for-byte identical). It has not been tested on a real
Windows machine, so if it misbehaves on yours, please tell me.
