#!/bin/sh
# build_web.sh -- the whole game for the browser, with online play.
#
#   tools/build_web.sh <your UMK3 .ipa, or its Payload/UMK3.app/UMK3> [out dir]
#
# Needs Emscripten's emcc on PATH (source toolchain/emsdk/emsdk_env.sh) and
# Python 3. Writes <out dir> (default build-web/game): umk3.html, umk3.js,
# umk3.wasm and the page's helpers -- the files server/deploy/publish-web.sh
# puts on the site.
#
# ## What the output holds, and what it does not
#
# The native build compiles the fight engine's tables out of the player's own
# binary (tools/logic_tables.py, level_info.py, seq_data.py): fine for a
# program built on the player's machine, wrong for one served to others. This
# build compiles the tables' LAYOUT instead (tools/logic_tables.py --layout:
# names, sizes, where each object sits in the binary, which words are
# pointers) and every player's page fills them from that player's own .ipa
# (runtime/umk3_tables_rt.c, web/game.html). So the binary given here is read
# for addresses and sizes only, its bytes go nowhere, and it is deleted from
# the scratch directory when the build ends. Level_Info and the playback
# scripts need no build step at all.
#
# The sources and defines are the umk3-game target's (CMakeLists.txt,
# launcher/build_game.ps1) with the browser's platform layer, the multiplayer
# layer (UMK3_HAVE_MP) and the run-time tables (UMK3_RUNTIME_TABLES).
set -eu

[ $# -ge 1 ] || { sed -n '3,4p' "$0" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IN=$1
OUT=${2:-$ROOT/build-web/game}
PY=${PYTHON:-python3}
# Windows ships a python3 that only opens the Store: try it, not just find it.
"$PY" -c 'import sys' >/dev/null 2>&1 || PY=python
command -v emcc >/dev/null 2>&1 || { echo "emcc not on PATH: source toolchain/emsdk/emsdk_env.sh" >&2; exit 1; }
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT INT TERM
mkdir -p "$OUT" "$SCRATCH/obj"

# ---- 1. the binary, read for its layout only
case "$IN" in
*.ipa)
    "$PY" -I - "$IN" "$SCRATCH/UMK3" <<'PYEOF'
import re, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    names = [n for n in z.namelist() if re.match(r"^Payload/[^/]+\.app/UMK3$", n)]
    if not names:
        sys.exit("%s: no Payload/*.app/UMK3 -- not a UMK3 .ipa" % sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        f.write(z.read(names[0]))
PYEOF
    BIN=$SCRATCH/UMK3 ;;
*)  BIN=$IN ;;
esac
"$PY" -I "$ROOT/launcher/check_binary.py" "$BIN" 90d6f56a18e2303f8f2053b02a42742e

# ---- 2. the tables' layout (no game bytes), checked against the binary
"$PY" -I "$ROOT/tools/logic_tables.py" "$BIN" "$SCRATCH/logic_tables_layout.c" --layout
# check_logic_tables.py writes the full tables (game bytes) to a temp
# directory and leaves it: point it at the scratch, which goes at exit.
TMPDIR=$SCRATCH TEMP=$SCRATCH TMP=$SCRATCH "$PY" -I "$ROOT/tools/check_logic_tables.py" "$BIN"

# ---- 3. compile
cd "$ROOT"
{
    ls runtime/game_main.c runtime/debug_menu.c runtime/draw_gl.c \
       runtime/gamecode_globals.c runtime/gamecode_stubs.c runtime/lime_menu.c \
       runtime/lime_platform.c runtime/lime_app.c runtime/wav.c \
       runtime/fight_runtime.c runtime/mp_session.c runtime/umk3_tables_rt.c \
       runtime/platform/sdl_gl.c runtime/platform/sdl_audio.c
    ls decomp/gamecode/*.c decomp/lime/*.c runtime/lime/*.c decomp/gamecode/logic/*.c
    echo "$SCRATCH/logic_tables_layout.c"
} > "$SCRATCH/sources"

CFLAGS="-std=gnu11 -O2 -w -include stddef.h
 -sUSE_SDL=2 -sUSE_SDL_MIXER=2 -sSDL2_MIXER_FORMATS=mp3
 -D_snprintf=snprintf -D_strdup=strdup -D_stricmp=strcasecmp
 -DUMK3_HAVE_MK3 -DUMK3_HAVE_MP -DUMK3_RUNTIME_TABLES -DUMK3_REAL_GL -DUMK3_SHELL
 -Iruntime -Idecomp/lime -Idecomp/gamecode/logic"
export CFLAGS SCRATCH
echo "compiling $(wc -l < "$SCRATCH/sources") files, $JOBS at a time"
# shellcheck disable=SC2016
xargs -P "$JOBS" -I{} sh -c 'f="$1"; o="$SCRATCH/obj/$(printf %s "$f" | tr "/\\\\:" "___").o"
    emcc $CFLAGS -c "$f" -o "$o" || { echo "FAILED: $f" >&2; exit 255; }' _ {} < "$SCRATCH/sources"

# ---- 4. link
# ASYNCIFY for the main loop's one sleep (plat_poll's emscripten_sleep, so the
# browser gets its frame); nothing reached through a function pointer ever
# sleeps, so the engine's dispatch tables need no instrumentation.
#
# EMULATE_FUNCTION_POINTER_CASTS: the fight engine calls most of its code
# through tables of one pointer type (MK3THREADFUNC and friends) whose
# targets are not all declared that way. i686 does not mind; wasm traps an
# indirect call whose type differs from the callee's. Emulation makes those
# calls behave as they do natively, at a cost a 56 Hz engine can afford.
#
# INITIAL_MEMORY: the static data -- the front end's and the engine's
# globals, zero-filled -- is about 67 MB before the first allocation.
#
# -lidbfs.js and web/save_mount.js keep the game's saves in the browser.
# LDFLAGS_EXTRA: e.g. --profiling-funcs, to keep function names for debugging.
#
# $warnOnce: the GL emulation's glShadeModel stub and several of its
# immediate-mode paths call warnOnce without declaring it as a dependency
# (src/lib/libglemu.js, Emscripten 6.0.12), so nothing links it in and the
# first call throws a ReferenceError that ends the main loop. LIME's mesh
# renderer calls glShadeModel for every 3D model, starting with the fighter
# a player picks on the select screen.
# shellcheck disable=SC2086
emcc -O2 "$SCRATCH"/obj/*.o -o "$OUT/umk3.html" ${LDFLAGS_EXTRA:-} \
    -sUSE_SDL=2 -sUSE_SDL_MIXER=2 -sSDL2_MIXER_FORMATS=mp3 \
    -sLEGACY_GL_EMULATION=1 -sGL_UNSAFE_OPTS=0 -sGL_FFP_ONLY=1 \
    '-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$warnOnce' \
    -sASYNCIFY=1 -sASYNCIFY_IGNORE_INDIRECT=1 \
    -sEMULATE_FUNCTION_POINTER_CASTS=1 \
    -sINVOKE_RUN=1 -sINITIAL_MEMORY=134217728 \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4294967296 \
    -sFORCE_FILESYSTEM=1 -sSTACK_SIZE=1048576 -lidbfs.js \
    -sEXPORTED_RUNTIME_METHODS=UTF8ToString \
    --js-library web/mp_webrtc.js \
    --shell-file web/game.html \
    --pre-js web/asset_mount.js \
    --pre-js web/save_mount.js
cp web/ipa.js web/ipa_store.js "$OUT/"
echo "built $OUT/umk3.html -- publish with server/deploy/publish-web.sh $OUT"
