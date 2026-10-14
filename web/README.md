# Experimental browser build

This build runs the existing native menu and its current experimental fight
scene through SDL2 and Emscripten. The page can read the user's own `.ipa` or
an already extracted `.app`/`res` directory locally in the browser. It extracts
`Payload/*.app/res/` plus small bundle-root files such as `Info.plist`; it does
not send user files to a server. The `.ipa` supplies game assets; it is not
compiled into WebAssembly. The port is compiled ahead of time from this
repository's C.

The fight scene is not the retail fight runtime: its state machine is still a
stand-in, so this is not a complete or verified browser port.

## Requirements

- Emscripten with `emcmake` and `emcc`
- SDL2_mixer's Emscripten port (CMake enables it with MP3 support)
- A modern desktop browser with WebGL, `DecompressionStream` and local file
  selection support
- Your own legally obtained `.ipa` or extracted app resources, available locally
  to the browser

No game binary or assets are included in this repository. At runtime, the page
lets the user select an `.ipa` or choose an extracted app/resources folder,
then copies the `res` files and small bundle-root files into the page's
in-memory virtual filesystem one at a time, keeping each resource's path
relative to the app and its decompressed file contents unchanged. Streaming
avoids retaining a second in-memory copy of all extracted files while mounting.
The original IPA is not edited.
After an IPA is selected, a copy is saved in the browser's IndexedDB storage
when **Guardar selección y comenzar** is clicked, then loaded automatically on
future visits to the same site in the same browser.
An extracted app/resources directory is saved the same way, including its game
resources and small bundle files. The same button starts the game after saving.
The **Borrar copia guardada** button clears either saved source. The app
executable is not copied into WebAssembly. Nothing is uploaded to a server or
written to the repository.
Loading assets still requires browser memory; local storage availability and
retention depend on browser quota and privacy settings.

To transfer a copy into this workspace for local analysis, use the separate
upload page and receiver:

```sh
python3 tools/ipa_upload_server.py --bind 0.0.0.0 --port 8765
```

Forward port 8765 as a private workspace port and open the one-time link printed
by the server. The receiver accepts a single IPA with a `Payload/*.app/res/`
tree and stores it as `IPA/UMK3.ipa`. It refuses to overwrite an existing file;
the `IPA/` directory is ignored by Git. Stop the receiver after the transfer.
This is a workspace upload, distinct from the browser game page's local-only
asset selection.

## Build and run

From the repository root:

```sh
emcmake cmake -S . -B build-web -DUMK3_BACKEND=sdl2
cmake --build build-web --target umk3-test
python3 -m http.server 8000 --directory build-web
```

Open `http://localhost:8000/` (or `http://localhost:8000/umk3-browser-v6.html`),
then select the original `.ipa`; the browser reads the app's `res/` tree and
retains its directory structure without asking you to extract it or pick files
one by one. Choosing an already extracted app/resources folder is an
alternative. Use the mouse or touch for the menu. Press F2 to enter the
experimental fight scene and F3 to return. The local HTTP server is needed
because browsers do not generally load WebAssembly from `file://` URLs.

On Ubuntu, the distro Emscripten config may set `FROZEN_CACHE = True`, which
prevents the compiler from creating its cache. For that package config, make a
user-writable copy and use a user cache:

```sh
cp /usr/share/emscripten/.emscripten /tmp/umk3-emscripten.config
sed -i 's/^FROZEN_CACHE = True/FROZEN_CACHE = False/' /tmp/umk3-emscripten.config
export EM_CONFIG=/tmp/umk3-emscripten.config
export EM_CACHE="$HOME/.cache/emscripten"
```

Run the configure and build commands above in that shell. Other Emscripten
installations that already allow cache writes do not need this workaround.

The ZIP reader can be checked without Emscripten with
`node --test tests/test_ipa_loader.js tests/test_ipa_store.js tests/test_asset_mount.js`.
The upload receiver can be checked with `python3 tests/test_ipa_upload_server.py`.

## The engine's tables: layout at build time, bytes at run time

A native build (the `umk3-game` CMake target, `launcher/build_game.ps1`) runs
`tools/logic_tables.py`, `tools/level_info.py` and `tools/seq_data.py` against
the player's binary and compiles the result: the fight engine's ~1,100 data
tables, Level_Info's arena names and the playback scripts, as bytes in the
executable. That is fine for a program built on the player's own machine from
the player's own `.ipa`. A `.wasm` served as a static page is handed to other
people, so it must not contain them.

The browser build compiles two things in their place:

| In the `.wasm` | From the player's `.ipa`, at run time |
|---|---|
| `logic_tables_layout.c` from `tools/logic_tables.py --layout`: the same symbols, types, sizes and order, all zero, plus a descriptor table of names, binary addresses, sizes, word indices and the binary's own pointer values | `Payload/UMK3.app/UMK3` (armv7 slice, LC_UUID `90d6f56a18e2303f8f2053b02a42742e`): every table's bytes |
| `runtime/umk3_tables_rt.c`: the filler, plus the run-time `umk3_relocate_level_info()` and `umk3_seq_addr()` that replace `level_info.c` and `seq_data.c` | Level_Info's string pool and the playback scripts, found by the two scripts' own rules |

At start-up, before `fight_runtime_init()` and `GameCodeInit()`, the program
calls `umk3_tables_fill_file(path, err, sizeof err)` (or `umk3_tables_fill`
with the bytes). It checks the uuid, maps each object to its bytes in the
binary, checks that every word it is about to relocate holds the pointer the
layout recorded, and only then writes; `umk3_relocate_level_info()` then runs
where `runtime/game_main.c` already calls it. A wrong or damaged file is
refused with a message and nothing is half-written. The full game's page
(`web/game.html`) hands the program the executable as `/assets/UMK3` beside
`res/` (`web/ipa.js`'s `includeExecutable`), and `tools/build_web.sh` compiles
these two files in place of the three generated ones; see
[docs/ONLINE.md](../docs/ONLINE.md). The menu test page above still mounts
only `res/`.

What a maintainer with the binary runs, again whenever the binary,
`tools/logic_tables.map` or the generator changes:

```sh
python3 tools/logic_tables.py <UMK3 binary> build-web/logic_tables_layout.c --layout
python3 tools/check_logic_tables.py <UMK3 binary>   # the same analysis, checked against the binary
```

`level_info` and `seq_data` need no step: the filler reads their inputs from
the binary's symbol table and sections itself. The layout file holds no game
content, but it is derived from the binary (symbol names, addresses, pointer
values), so, like everything else derived from it, it stays out of the
repository unless the maintainers decide otherwise. `--report` output holds
word values and belongs in neither the build nor the repository.

Not covered by this path: `runtime/gamecode_globals.c`, generated by
`tools/mkglobals.py` and committed, also holds initial values read from the
binary (Level_Info's 4,084 bytes among them).

Both halves are tested without the game's binary, against synthetic Mach-O
files: `python3 tests/test_logic_layout.py` (its compiled parts need a 32-bit
compiler, and Emscripten for the wasm one, and are skipped without them) and
`tests/test_tables_rt.c` (build commands in its header).
