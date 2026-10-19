# Tekken: Dark Resurrection (PSP) recomp — notes

## Base version
- ULUS-10139, DISC_VERSION 1.00, firmware req. 2.60
- ISO SHA-1 `8193a65dee49685db49284408c89bdb35324bc32`, CRC32 `3ffd7a9d`, 1,750,237,184 bytes
- Game files live in `game/` (gitignored). `tools/cso2iso.py` rebuilds the ISO from the CSO.

## Executable (`game/tkpsp.elf`)
- `EBOOT.BIN` is encrypted (`~PSP`); `BOOT.BIN` is all zeros.
- Decrypted via PPSSPP 1.20.4 (`DumpFileTypes = 1` in `ppsspp.ini`) →
  `~/.config/ppsspp/PSP/SYSTEM/DUMP/ULUS10139_EBOOT.BIN`.
- ELF type `ET_SCE_PRX` (0xFFA0), module name `tkpsp`, relocatable (base 0).
- Full relocation sections kept (`.rel.text`, `.rel.rodata`, `.rel.data`, …):
  every code/data pointer is known — big win for static recompilation.
- No symbol table. Code is C++ with exceptions: `.eh_frame` has 7310 FDEs inside
  `.text` → exact function boundaries for ~74% of `.text` (rest: C code without EH).
- Compiler: `GCC 3.3.6+allegrex-2.2.2-psp-1.9.2` (951 objects), plus 6 objects
  from `3.3.3 … psp-1.8.4.internal` (SDK libs).
- Leftover source names in `.rodata`: hagame2d.cpp, haswg*.cpp, tbskinmodel.cpp,
  dwdataman.cpp, owtheater.cpp, zs_efct.cpp, …

## Sections
| Section | Addr | Size |
|---|---|---|
| .text | 0x00000040 | 0x272ae0 (~2.4 MB) |
| .rodata | 0x002c17a0 | 0x833a8 |
| .data | 0x00344b80 | 0x2c58c |
| .bss | 0x00371180 | 0x103b614 (~16 MB) |

## Imports (`tools/psp_imports.py`)
31 libraries, 280 functions. Notable: ThreadManForUser (41), sceMpeg (25),
sceHttp (23), IoFileMgrForUser (23), sceSasCore (21), sceUtility (20),
sceNetAdhoc* (31 total, ad-hoc multiplayer), sceGe_user (6), sceAtrac3plus (9).

## Data
- `USRDIR/TKPDATA0.BIN` (412 MB), `TKPDATA1.BIN` (844 MB): asset archives,
  header looks compressed/encrypted — not analysed yet.
- Sony modules bundled in USRDIR (audiocodec, libatrac3plus, mpeg, sascore,
  videocodec): system code, to be replaced by HLE.

## Analysis (`python3 -m recomp.analyze game/tkpsp.elf`)
- Loaded at 0x08804000 (same as the PSP kernel); entry 0x089F34A8.
- Relocations follow the kernel loader: HI16 pairs with the *next* LO16 in list
  order. The list is unsorted and HI/LO pairs of different registers interleave,
  so analysis targets are paired by register instead.
- 641,720 instructions, all decoded. FPU 5.2%, VFPU 2.3%.
- 13,798 functions: 7,310 from .eh_frame, rest from jal targets and code
  pointers in data (vtables/callbacks). Case labels are filtered out.
- 397 indirect `jr`: 388 recognised jump tables (sltiu bound + table in .rodata).
- 2,232 `jalr` (virtual calls / callbacks) → need a runtime address→function map.

## Debugging aids (environment variables)
| Variable | Effect |
|---|---|
| `HEADLESS=1` | no window; with `RUN_SECONDS=n` exit after n guest seconds |
| `TIME_SCALE=x`, `TIME_SCALE_UNTIL=s` | run guest time x times faster (until s seconds) |
| `INPUT="118:START,118.3:"` | scripted buttons at guest times (also as URL params on the web) |
| `GE_CAPTURE=secs` | save the first display list started after that guest time (GE state, VRAM, RAM) to `GE_CAPTURE_FILE` (default `ge_capture.bin`) |
| `GE_REPLAY=file` | no game: draw a capture `GE_REPLAY_ITERS` times (default 20), print ms/frame, save `GE_REPLAY_OUT=out.ppm`; with `RENDER_SCALE`, `RASTER_THREADS` |
| `HIDDEN_WINDOW=1` | GPU renderer without showing the window (tests) |
| `GL_SHOTS=dir` | save what the GPU renderer drew for the displayed framebuffer every guest second (PPM) |
| `GL_RECORD=start:seconds:dir` | every presented frame of the GPU renderer in that guest time window (PPM) |
| `GL_RECORD_MP4=start:seconds:file.mp4` | the whole window as shown (overlays too) in that guest time window, to an mp4 through ffmpeg (hardware encoder) without slowing the game |
| `SHOW_FPS=1` | start with the fps overlay (F2) on |
| `TIMER_TICK_US=15600` | round the scheduler's timed waits up to that tick, as Windows does (the game must still run at full speed) |
| `NO_GL=1` | no OpenGL: software renderer, presented with SDL_Renderer |
| `FPS=1` | (windowed) print the fps line every second |
| `TOUCH=1` | show the on-screen controls from the start (web: `?touch`) |
| `UI_PREVIEW_NOMENU=1`, `UI_PREVIEW_SIZE=WxH` | with `UI_PREVIEW`: overlay without the menu, at a window size (e.g. a phone) |
| `RENDER_SCALE=n` | internal resolution 1..4 (overrides the F1 > Video setting) |
| `TEX_DIR=dir` | texture pack folder (default `textures/ULUS10139`) |
| `TEX_DUMP=1` | dump every new texture as PNG into `TEX_DIR/new/` (PPSSPP names) |
| `SHOTS=dir` | dump the displayed framebuffer every guest second (PPM) |
| `AUDIO_DUMP=file.raw` | capture audio output (s16 stereo 44.1 kHz) |
| `FPS=1` | per-second fps, GPU busy/flush stats |
| `HLE_LOG=1` | log every system call; `HLE_BT=name` prints the guest call chain at that call |
| `WATCH=addr,...` | print guest words every second |
| `TRACE_RA=addr` | log indirect calls made from that return address |
| `GE_STATS=1` | histogram of primitive/vertex/texture formats |
| `RASTER_THREADS=n` | rasterizer worker count |
| `MPEG_DUMP=file` | save the movie stream fed by the game |
| `RECOMP_PROBE=addr,...` (emit time) | log args/return of those functions; `PROBE_CHAIN=6:84:440` follows pointers |
| `RECOMP_CHECK=1` (emit time) | report functions that clobber callee-saved registers |

## Hostable web build (RECOMP_DYNAMIC)

- `web/recomp/*.mjs` port `recomp/*.py` (identical analysis: same functions,
  jump tables, labels, relocations) and `wasmgen.mjs` emits one wasm function
  per guest function instead of C. Registers stay in the `Ctx` struct in shared
  memory; guest memory is `g_mem` (imported global `gmem`). Functions with
  labels are a `loop` around one `block` per segment, entered with `br_table`
  on a segment number (a jump sets it and restarts the loop).
- The module imports `hle_call`, `vfpu_exec`, `recomp_trap_code`,
  `recomp_call_miss` from the runtime and exports `dispatch(ctx, addr)` (binary
  search over function starts). The runtime calls it through `game_dispatch`
  (`runtime/web/game_lib.js`), which instantiates the module once per thread
  (every PSP thread is a Web Worker); the page compiles it once and posts it
  to the workers.
- The page writes `/game/game.txt`, `/game/image.bin`, `/game/code.wasm` before
  `main()`. Translations are cached in IndexedDB under EBOOT sha1 + a hash of the
  recompiler sources.
- Local test: `python3 tools/serve.py build/web-host 8082 --iso game/tekken_dr.iso
  --eboot game/tkpsp.elf`, open `/tkdr.html?iso=/game.iso&eboot=/eboot.bin`.

## GPU renderer (runtime/gpu/glr.c)

- raster.c keeps vertex decode, transform, lighting, near clipping and culling;
  with `g_glr_active` it records a `GlrState` per state change and clip-space
  vertices (perspective restored from 1/w) instead of rasterizing. Textures go
  once per version (`serial`, bumped on every redecode) and are evicted LRU.
- The GE thread hands the recorded buffer over at each display list end and may
  wait if more than 3 lists are not yet taken; the GL thread (frontend frame)
  uploads all vertices at once and replays states/draws, skipping redundant GL
  calls (they are slow in WebGL).
- Each PSP framebuffer is an FBO covering 512x512 PSP pixels at the render
  scale; PSP row 0 is GL row 0 (flipped only when presenting). An FBO starts
  from what VRAM holds. Not done yet: framebuffers read back by the CPU or used
  as textures, colour test, fog, doubled-alpha and absolute-difference blending.
