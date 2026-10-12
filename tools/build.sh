#!/bin/sh
# Build the game from YOUR OWN dump. Nothing from the game is in this repo:
# the C code and data image are generated locally from the files you provide.
#
#   tools/build.sh --iso <game.iso|game.cso> --eboot <decrypted EBOOT> [--native] [--web]
#   tools/build.sh --web-host
#
# The decrypted EBOOT is produced by PPSSPP (see README). Outputs:
#   build/native/tkdr   build/web/tkdr.html (+ .js/.wasm/.data)
# --web-host needs no game files: build/web-host is the browser version that
# recompiles each player's own EBOOT on their device, and can be put online.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EBOOT_SHA1=b9a0e3292738a833f4f8077e3959c3a0652c42cd  # ULUS-10139 v1.00, decrypted
ISO_SHA1=8193a65dee49685db49284408c89bdb35324bc32    # ULUS-10139 v1.00 (redump)

ISO= EBOOT= NATIVE= WEB= WEBHOST=
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO=$2; shift 2 ;;
    --eboot) EBOOT=$2; shift 2 ;;
    --native) NATIVE=1; shift ;;
    --web) WEB=1; shift ;;
    --web-host) WEBHOST=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$ISO$EBOOT$NATIVE$WEB" ] || [ -z "$WEBHOST" ] || GAME_FREE=1
if [ -z "$GAME_FREE" ]; then
  [ -n "$ISO" ] && [ -n "$EBOOT" ] || { sed -n '2,12p' "$0"; exit 2; }
  [ -n "$NATIVE$WEB$WEBHOST" ] || NATIVE=1
fi

need() { command -v "$1" >/dev/null || { echo "missing tool: $1 ($2)" >&2; exit 1; }; }
need python3 "https://www.python.org"
need cmake "brew install cmake"
need ninja "brew install ninja"
[ -z "$WEB$WEBHOST" ] || need emcc "brew install emscripten"
cd "$ROOT"

if [ -z "$GAME_FREE" ]; then
mkdir -p "$ROOT/game"
sha1() { shasum -a 1 "$1" | cut -d' ' -f1; }

# 1. the executable: must be exactly the supported version
[ "$(sha1 "$EBOOT")" = "$EBOOT_SHA1" ] || {
  echo "error: $EBOOT is not the decrypted EBOOT of ULUS-10139 v1.00 (sha1 mismatch)" >&2; exit 1; }
cp "$EBOOT" "$ROOT/game/tkpsp.elf"

# 2. the disc image (a CSO is expanded first)
case "$ISO" in
  *.cso|*.CSO) python3 "$ROOT/tools/cso2iso.py" "$ISO" "$ROOT/game/game.iso"; ISO="$ROOT/game/game.iso" ;;
esac
if [ "$(sha1 "$ISO")" != "$ISO_SHA1" ]; then
  echo "warning: $ISO does not match the redump hash of ULUS-10139 v1.00; continuing" >&2
fi
ISO_ABS="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"
if [ "$ISO_ABS" != "$ROOT/game/tekken_dr.iso" ] && { [ -L "$ROOT/game/tekken_dr.iso" ] || [ ! -e "$ROOT/game/tekken_dr.iso" ]; }; then
  ln -sf "$ISO_ABS" "$ROOT/game/tekken_dr.iso"  # never replaces a real file
fi

# 3. translate the executable to C (into build/gen, never committed)
python3 -m recomp.emit game/tkpsp.elf build/gen
fi

# 4. FFmpeg (H.264 decoder for the movies) and the builds
tools/build_ffmpeg.sh
if [ -n "$NATIVE" ]; then
  cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2" >/dev/null
  ninja -C build/native
  echo "native build: build/native/tkdr build/gen/image.bin game/tekken_dr.iso"
fi
if [ -n "$WEB" ]; then
  emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2" >/dev/null
  ninja -C build/web
  echo "web build: python3 tools/serve.py build/web 8080, then open http://localhost:8080/tkdr.html"
fi
if [ -n "$WEBHOST" ]; then
  emcmake cmake -S . -B build/web-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2" \
    -DRECOMP_DYNAMIC=ON >/dev/null
  ninja -C build/web-host
  echo "hostable web build (no game inside): build/web-host/ = tkdr.html tkdr.js tkdr.wasm recomp/"
  echo "  try it: python3 tools/serve.py build/web-host 8080, then open http://localhost:8080/tkdr.html"
fi
