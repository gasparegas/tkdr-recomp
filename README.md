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

## Hosting the browser version

```sh
tools/build.sh --web-host
```

needs no game files and produces `build/web-host/`: `tkdr.html`, `tkdr.js`,
`tkdr.wasm`, `recomp/` and `_headers`. **Nothing from the game is in it**, so this
folder is what can be put online (any static host that sends the COOP/COEP
headers in `_headers`, e.g. Cloudflare Pages or Netlify).

Each player picks their own ISO and their own decrypted EBOOT on the page. The
EBOOT is translated into WebAssembly **in their browser** (`web/recomp/`, a
JavaScript port of the recompiler that writes wasm directly), once, then kept
in that browser's storage. Files never leave the device.

Never host `build/web/` or `build/native/`: those contain the translated game.

### Your own server at home

`config.json`, next to the page, decides where the game comes from:

```json
{ "pickFiles": true,  "gameZip": "" }
{ "pickFiles": false, "gameZip": "http://my-nas.local/games/tekken.zip" }
```

On a host, write it from environment variables at deploy time:
`python3 tools/web_config.py build/web-host` (`PICK_FILES`, default `true`;
`GAME_ZIP` for `false`). On a public host leave `PICK_FILES` unset or `true`.

With `pickFiles: false` the page loads the zip (your ISO or CSO plus your EBOOT)
from that address instead of asking for files, keeps it in the browser and
plays. The server only has to answer HTTP range requests (any NAS or web
server does); serving the page from the same server avoids CORS setup.

**Use this only on a private network or behind a login, with your own copy.**
A page that anyone can open and that hands out the game is distributing it.
The public build always ships with `pickFiles: true` and no address, and a
build never overwrites an existing `config.json`.

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

On phones and tablets the browser build goes full screen with on-screen
controls (d-pad, face buttons, L/R, Start/Select, ≡ for settings); portrait puts
the game on top and the controls below. *F1 → Input* sets them to always/never
and their opacity; they also appear on touch laptops after the first touch.

Press **F1** for the settings overlay: input devices and bindings (keyboard,
gamepads, arcade sticks, hitboxes, with SOCD options) and texture packs.

## Graphics

The game is drawn by the graphics card (OpenGL 3.3, or WebGL 2 in the browser):
the CPU still decodes, transforms and lights the vertices exactly like the PSP's
GE, the GPU rasterizes, textures and blends. *F1 → Video* sets the internal
resolution (1× to 4×: 480×272 up to 1920×1088) and can switch to the software
renderer (a CPU rasterizer, used when no OpenGL is available).

## Texture packs

Packs made for PPSSPP work as they are: put `textures.ini` and the PNGs in
`textures/ULUS10139/` next to where you run the game (or point `TEX_DIR` at the
folder). Replacements may be any size, e.g. 4× the original for HD packs.

To make your own, enable *Dump new textures* in the F1 overlay (or run with
`TEX_DUMP=1`): every texture the game uses is saved to `textures/ULUS10139/new/`
under the same name PPSSPP would give it. Edit a PNG, move it up into
`textures/ULUS10139/` and press *Reload pack*. Movie frames are never dumped.

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
- `web/recomp/`: the same recompiler in JavaScript, emitting WebAssembly
  directly, for the hostable browser build (`RECOMP_DYNAMIC`).
- `tools/`: build helpers, ISO/CSO tools and a local web server.

Debugging aids are described in `NOTES.md`.

## Licenses

Third-party code keeps its own license:

- `third_party/at3_standalone/`: ATRAC3/ATRAC3+ decoders from FFmpeg, in the
  standalone form extracted by the PPSSPP project, LGPL-2.1-or-later.
- `third_party/imgui/`: Dear ImGui, MIT.
- `third_party/stb/`: stb_image and stb_image_write, public domain.
- FFmpeg (H.264 decoder), downloaded and built by `tools/build_ffmpeg.sh`,
  LGPL-2.1-or-later.

PPSSPP's source served as documentation of PSP behaviour; no PPSSPP code is
included apart from the decoder extraction above.
