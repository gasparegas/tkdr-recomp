# tkdr-recomp

A static recompilation of *Tekken: Dark Resurrection* (PSP, ULUS-10139) to native
code and WebAssembly. The game's MIPS executable is translated to C, then compiled
for your machine or for the browser together with a runtime that reimplements the
PSP's system libraries (threads, file system, graphics, audio, video, save data).

## This repository contains no game data

**Nothing from the game is distributed here, and nothing ever will be.** No
executable, no assets, no translated code, no encryption keys.

You must own the game and dump it yourself. Everything derived from the game is
generated **locally** from your dump, into `game/` and `build/`, which are
git-ignored. Do not share those directories or any build made from them: they
contain material derived from copyrighted code and data.

This is an independent fan project for preservation and research. It is not
affiliated with or endorsed by Bandai Namco, Sony or anyone else. *Tekken* is a
trademark of Bandai Namco Entertainment Inc.; *PSP* and *PlayStation* are trademarks
of Sony Interactive Entertainment.

## What you need

- Your own UMD of **Tekken: Dark Resurrection, ULUS-10139 (USA) v1.00**, dumped
  to an ISO or CSO with a PSP running custom firmware. The build script checks
  the ISO against its [redump](http://redump.org) hash and warns on mismatch.
- The game's **decrypted** executable. The EBOOT on the disc is encrypted. This
  project does not ship PSP keys. Have [PPSSPP](https://www.ppsspp.org) produce the
  decrypted copy from your own disc:
  1. In PPSSPP: *Settings → Tools → Developer tools*, enable **Dump decrypted
     EBOOT.BIN on game boot** (in `ppsspp.ini`: `DumpFileTypes = 1`).
  2. Boot your ISO until the first logo, then quit.
  3. The file appears as `PSP/SYSTEM/DUMP/ULUS10139_EBOOT.BIN` in PPSSPP's
     memory stick folder (on macOS: `~/.config/ppsspp/`).
- macOS or Linux with `python3`, `cmake`, `ninja`, a C/C++ compiler and SDL2;
  for the browser build, Emscripten. On macOS:
  `brew install cmake ninja sdl2 emscripten`.

## Build

```sh
tools/build.sh --iso path/to/game.iso --eboot path/to/ULUS10139_EBOOT.BIN --native --web
```

The script verifies the decrypted executable (it must be exactly the supported
version), translates it to C, builds a minimal FFmpeg (H.264 decoder only) and
compiles the targets you asked for.

## Run

Native:

```sh
build/native/tkdr build/gen/image.bin path/to/game.iso
```

Browser (needs a server sending the COOP/COEP headers that wasm threads require):

```sh
python3 tools/serve.py build/web 8080
```

Open `http://localhost:8080/tkdr.html` and pick your ISO; it is read locally and
never uploaded. Save data stays in the browser (IndexedDB).

| Keyboard | PSP |
|---|---|
| Arrows | D-pad |
| Enter / Space | Start / Select |
| Z / X / C / S | □ / ✕ / ○ / △ |
| Q / W | L / R |

Gamepads work too (face buttons in the PSP layout; the left stick also drives the d-pad).

## Status

Boots, plays movies with sound, menus, save/load, and full matches against the CPU
at 60 fps (native and in the browser on a recent desktop). Missing: SAS reverb,
ad-hoc multiplayer.

## How it works

- `recomp/`: loads the PRX with the kernel's relocation rules, decodes Allegrex
  instructions, finds functions (`.eh_frame`, call targets, code pointers, jump
  tables, cross-jump fragments) and emits one C function per guest function.
- `runtime/`: guest memory, a PSP-like single-core priority scheduler on host
  threads, HLE of the system libraries, a VFPU interpreter, a multi-threaded
  software rasterizer for the GE, SAS/ATRAC3+/H.264 audio and video, and an SDL2
  frontend that also builds with Emscripten.
- `tools/`: build helpers, ISO/CSO tools and a local web server.

Debugging aids are described in `NOTES.md`.

## Licenses

Third-party code keeps its own license:

- `third_party/at3_standalone/`: ATRAC3/ATRAC3+ decoders from FFmpeg, in the
  standalone form extracted by the PPSSPP project, LGPL-2.1-or-later.
- FFmpeg (H.264 decoder), downloaded and built by `tools/build_ffmpeg.sh`,
  LGPL-2.1-or-later.

PPSSPP's source served as documentation of PSP behaviour; no PPSSPP code is
included apart from the decoder extraction above.
