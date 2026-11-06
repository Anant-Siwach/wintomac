# Packaging a game as a macOS `.app`

`tools/package_app.sh` (a wrapper for `tools/package_app.py`) turns a Windows game folder into a
self-contained, double-clickable `dist/<Name>.app`. The result runs on an Apple-silicon Mac that has no Homebrew,
no Xcode and no wintomac checkout.

```sh
tools/package_app.sh <game_dir> <exe> [dlls...] --name "Game" [--args "..."] [--icon path] [--link-data]
                     [--bundle-id com.example.game] [--version 1.0] [--out dist] [--recompile]
                     [--recomp-flags "--no-callopt"] [--microphone] [--exclude GLOB] [--no-sign]
# or
make package GAME=<game_dir> EXE=<exe> NAME="Game" DLLS="a.dll b.dll" ARGS="..."
```

* `dlls` are the DLLs recompiled together with the exe (same list you would give `python -m recomp`), relative to
  `game_dir`. For an existing build, `build/<slug>.gen/report.json` lists them under `images`.
* `--args` are passed to the game before any arguments given to the app itself.
* `--link-data` does not copy the game: the app runs the game from `game_dir` in place (for 50 GB games on the
  same Mac). The app is then not portable.
* `--icon` takes `.icns`, `.png` or `.ico`; otherwise the exe's own icon is used, else the generic app icon.

## What it does

1. **Recompile** `<exe>` + DLLs into `build/<slug>.gen` (`slug` = lower-cased name). An existing tree is reused
   unless `--recompile` is given or the tree is older than `recomp/*.py` / `runtime/recomp.h` (stale generated code
   no longer links).
2. **Link** it against the current runtime: `make link GEN=build/<slug>.gen OUT=build/<slug>.bin`.
3. **Build the bundle**:

```
<Name>.app/Contents/
  Info.plist                     CFBundleIdentifier com.wintomac.<slug>, LSMinimumSystemVersion (from the binaries'
                                 LC_BUILD_VERSION), NSHighResolutionCapable, LSApplicationCategoryType = games,
                                 NSMicrophoneUsageDescription only with --microphone
  MacOS/<Name>                   launcher script (CFBundleExecutable)
  MacOS/<slug>.bin               the recompiled game
  Frameworks/                    every non-system dylib the binary needs, recursively (SDL3, FAudio, Vulkan loader,
                                 vkd3d, ...) plus MoltenVK; install names rewritten to @rpath/<lib>, the binary's
                                 rpath is @executable_path/../Frameworks, Homebrew/build rpaths removed
  Resources/game/                the game folder (copied; *.pdb skipped)
  Resources/vulkan/icd.d/MoltenVK_icd.json   library_path ../../../Frameworks/libMoltenVK.dylib
  Resources/<Name>.icns          from the exe's RT_GROUP_ICON/RT_ICON (largest image) via sips + iconutil
```

   The launcher `cd`s into the game folder (the runtime maps `C:\` to the exe's folder), exports
   `WINTOMAC_APPNAME=<Name>` and `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` (the bundled MoltenVK; an already set value
   wins), drops Finder's `-psn_*` argument, and runs `<slug>.bin ./<exe> [--args] "$@"`.
4. **Sign** ad hoc, inside out: each dylib, the binary and the bundle (`codesign -s -`), with the
   `com.apple.security.cs.allow-jit` entitlement for the runtime's JIT. `codesign -v --strict` must pass.

## Save data

`C:\users\...` (AppData, Saved Games, Documents) and the HKCU registry file live under
`~/Library/Application Support/wintomac/<Name>/` for a packaged app (`runtime/appdata.c`): the launcher sets
`WINTOMAC_APPNAME`, and a binary started directly from inside `<Name>.app/Contents/MacOS/` derives the same name.
Unpackaged runs (`build/x.bin game.exe`) keep using `~/Library/Application Support/wintomac/`.

## Example: vkQuake

```sh
tools/package_app.sh third_party/games/vkquake/dist vkQuake.exe \
    libFLAC-8.dll libmad-0.dll libmikmod-3.dll libmpg123-0.dll libogg-0.dll libopus-0.dll libopusfile-0.dll \
    libvorbis-0.dll libvorbisfile-3.dll libxmp.dll SDL3.dll --name vkQuake

otool -L dist/vkQuake.app/Contents/MacOS/vkquake.bin      # only @rpath/... and /usr/lib, /System
codesign -v --strict dist/vkQuake.app
open dist/vkQuake.app                                      # or run Contents/MacOS/vkQuake for console output
```

Headless smoke test (no window, no sound). vkQuake ignores `+quit` at startup (it opens the quit-confirmation menu),
so stop it the way Cmd-Q would; it shuts down cleanly and exits 0:

```sh
WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy dist/vkQuake.app/Contents/MacOS/vkQuake > /tmp/vkq.log 2>&1 &
sleep 20; kill -TERM $!; wait $!; echo "exit=$?"; grep "Quake Initialized" /tmp/vkq.log
```

## Logs and the slow first launch

The launcher writes `~/Library/Logs/wintomac/<Name>.log` (launch header with time/args; the game's stdout+stderr; rotated
at 2 MB). `WINTOMAC_TRACE_STARTUP=1` (or running from a terminal) prefixes every line with seconds since launch and, from
a terminal, still echoes to the console; `WINTOMAC_LOG=0` disables logging. Cache dir: `~/Library/Caches/wintomac/<Name>/`.

**First launch can look frozen.** On a Mac that has never run the app, MoltenVK compiles every Metal pipeline/shader
before the first frame (`sample` shows the main thread in `vkCreateGraphicsPipelines` -> `MVKMetalCompiler::compile`).
Measured for vkQuake: cold Metal cache about 24-37 s, warm about 4 s. Metal keeps its cache in
`$(getconf DARWIN_USER_CACHE_DIR)/<bundle id>/com.apple.metal` (3 MB), keyed by shader content, so rebuilding the app does
not invalidate it. vkQuake has no VkPipelineCache file of its own, and a VkPipelineCache would not skip Apple's compile
anyway. So the launcher just says so on the first run (stderr plus a notification via `osascript`; skipped when
`WINTOMAC_HEADLESS` is set). Wait up to a minute; do not force-quit.

Cold-start retest: `rm -rf "$(getconf DARWIN_USER_CACHE_DIR)/com.wintomac.vkquake" ~/Library/Caches/wintomac/vkQuake`
then `open dist/vkQuake.app`, and watch `tail -f ~/Library/Logs/wintomac/vkQuake.log`.

## Limitations

* **Games that write into their own folder** (old engines: Quake's `id1/config.cfg`, many `.ini` files) write into
  `Contents/Resources/game`. That works when the app is in a user-writable place, but changes the bundle (so
  `codesign -v` reports modified resources afterwards; the app still launches). Apps in `/Applications` installed by
  another user cannot write there. A copy-on-write overlay into Application Support is future work.
* Ad-hoc signed: fine on the Mac that built it. Another Mac sees a quarantined download and needs
  right-click → Open (or `xattr -dr com.apple.quarantine <app>`). Distribution needs a Developer ID signature and
  notarisation (`codesign --options runtime --entitlements ...` + `notarytool`), not done here.
* Steam games: `libsteam_api.dylib` is user-supplied and not bundled (see `docs/steam.md`); put it next to the game
  data or set `WINTOMAC_STEAM_API`.
* Apple silicon only (the binary is arm64); MoltenVK comes from the build machine's Homebrew.
