#!/bin/bash
# PS5 program -> native Apple-silicon .app, in one command (docs/ps5.md).
#
#   tools/ps5_to_mac.sh <title_dir | eboot.bin | homebrew.elf> [--name N] [options]
#
# Chain: AnyPS5 relinker --windows  ->  Windows x86-64 .exe + AnyPS5's Windows PS5-library DLLs (libs/*.prx)
#        ->  wintomac recompiler (python -m recomp)  ->  native arm64 binary  ->  dist/<Name>.app (tools/package_app.py)
#
# <title_dir> is a folder holding eboot.bin (and optionally sce_module/ or sce_modules/ or prx/, sce_sys/param.json
# and the title's other files); it becomes app0/ for the program. A single ELF (homebrew) works too.
# Input must already be a plain (decrypted) ELF: this tool never decrypts or handles keys.
#
# Options:
#   --name N            app name (default: the input's folder / file name)
#   --out DIR           where the .app goes (default: dist)
#   --anyps5 release|main  which AnyPS5 to use (default: $PS5_ANYPS5, else release): release = the pinned release in
#                       third_party/anyps5-win (tools/ps5_setup.sh), main = AnyPS5 built from main in
#                       third_party/anyps5-main (tools/ps5_setup.sh --main)
#   --libs DIR          AnyPS5 Windows libs (default: <anyps5 dir>/prx/libs)
#   --relinker PATH     AnyPS5 relinker (default: <anyps5 dir>/build-relinker/...; must match --libs' version)
#   --with LIB.prx      also recompile this PS5 library (repeatable; for libraries the title loads at run time
#                       with sceSysmoduleLoadModule, which the static dependency scan cannot see)
#   --all-libs          recompile every library in --libs (slow: ~300 MB of x86 code)
#   --seeds FILE        feed back missed code addresses (wintomac_misses.txt from a run) and recompile
#   --recompile         recompile even if build/<slug>.gen is current
#   --link-data         don't copy the title's files into the .app (run them in place; app not portable)
#   --relink-args "..." extra relinker options (e.g. "--windows-fixed-base 0x400000"; also $PS5_RELINK_ARGS)
#                       Media/Modules and Media/Plugins (Unity) under the title folder are passed as --module-dir
#                       automatically when the relinker has that option (AnyPS5 main).
#   --no-app            stop after linking build/<slug>.bin (no bundle)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
ANYPS5="${PS5_ANYPS5:-release}"; LIBS=""; RELINKER=""
NAME=""; OUT="$ROOT/dist"; WITH=(); ALL=0; SEEDS=""; RECOMPILE=0; LINKDATA=0; RELINK_ARGS="${PS5_RELINK_ARGS:-}"; NOAPP=0; INPUT=""

die() { echo "ps5_to_mac: $*" >&2; exit 1; }
stage() { echo; echo "==== [$1] $2"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --libs) LIBS="$(cd "$2" && pwd)"; shift 2 ;;
        --relinker) RELINKER="$2"; shift 2 ;;
        --anyps5) ANYPS5="$2"; shift 2 ;;
        --with) WITH+=("$2"); shift 2 ;;
        --all-libs) ALL=1; shift ;;
        --seeds) SEEDS="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; RECOMPILE=1; shift 2 ;;
        --recompile) RECOMPILE=1; shift ;;
        --link-data) LINKDATA=1; shift ;;
        --relink-args) RELINK_ARGS="$RELINK_ARGS $2"; shift 2 ;;
        --no-app) NOAPP=1; shift ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) [ -z "$INPUT" ] || die "more than one input given"; INPUT="$1"; shift ;;
    esac
done
case "$ANYPS5" in
    release) AD="$ROOT/third_party/anyps5-win"; SETUP="tools/ps5_setup.sh" ;;
    main) AD="$ROOT/third_party/anyps5-main"; SETUP="tools/ps5_setup.sh --main" ;;
    *) die "--anyps5 must be release or main (got $ANYPS5)" ;;
esac
LIBS="${LIBS:-$AD/prx/libs}"
RELINKER="${RELINKER:-$AD/build-relinker/core/relinker/relinker}"
[ -n "$INPUT" ] || die "usage: tools/ps5_to_mac.sh <title_dir | eboot.bin | homebrew.elf> [--name N] (see --help)"
[ -e "$INPUT" ] || die "no such file or folder: $INPUT"
[ -x "$RELINKER" ] || die "AnyPS5 relinker not found at $RELINKER. Run $SETUP first (docs/ps5.md)."
[ -f "$LIBS/libc.prx" ] || die "AnyPS5 Windows libraries not found in $LIBS. Run $SETUP first (docs/ps5.md)."
[ -x "$PY" ] || die "wintomac's Python venv is missing: python3 -m venv .venv && .venv/bin/pip install iced-x86==1.21.0"

if [ -d "$INPUT" ]; then
    SRC_DIR="$(cd "$INPUT" && pwd)"
    ELF="$SRC_DIR/eboot.bin"
    [ -f "$ELF" ] || die "$SRC_DIR has no eboot.bin"
    DEFAULT_NAME="$(basename "$SRC_DIR")"
else
    ELF="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
    SRC_DIR=""
    DEFAULT_NAME="$(basename "$INPUT")"; DEFAULT_NAME="${DEFAULT_NAME%.*}"
fi
head -c 4 "$ELF" | od -An -c | grep -q "E   L   F" \
    || die "$ELF is not an ELF file. Encrypted or SELF-wrapped files are not supported: supply a plain ELF you dumped yourself (tools/unwrap_self.py in AnyPS5 unwraps decrypted SELF containers)."
NAME="${NAME:-$DEFAULT_NAME}"
SLUG="$(echo "$NAME" | tr 'A-Z' 'a-z' | sed -E 's/[^a-z0-9]+/_/g; s/^_+|_+$//g')"; SLUG="${SLUG:-game}"
WORK="$ROOT/build/ps5/$SLUG"
GAME="$WORK/game"          # the folder that runs: <slug>.exe, libs/, app0/
EXE="$SLUG.exe"
mkdir -p "$GAME/libs"
echo "ps5_to_mac: $ELF -> $NAME ($WORK; AnyPS5 $ANYPS5: $LIBS)"

# ---- 1. relink -------------------------------------------------------------------------------------------------------
stage 1/5 "AnyPS5 relinker --windows"
MODS=()
RELINK_IN="$WORK/in"
rm -rf "$RELINK_IN"; mkdir -p "$RELINK_IN"
ln -s "$ELF" "$RELINK_IN/eboot.bin"
if [ -n "$SRC_DIR" ]; then
    for d in sce_module sce_modules prx; do [ -d "$SRC_DIR/$d" ] && ln -s "$SRC_DIR/$d" "$RELINK_IN/$d"; done
fi
# modules the title loads at run time from other folders (Unity: Media/Modules, Media/Plugins): --module-dir (AnyPS5 main)
MODDIRS=()
if [ -n "$SRC_DIR" ] && "$RELINKER" --help 2>&1 | grep -q -- '--module-dir'; then
    for d in Media/Modules Media/Plugins; do [ -d "$SRC_DIR/$d" ] && MODDIRS+=(--module-dir "$d"); done
    [ ${#MODDIRS[@]} = 0 ] || ln -s "$SRC_DIR/Media" "$RELINK_IN/Media"
fi
HAS_MOD=0; for d in sce_module sce_modules prx; do [ -e "$RELINK_IN/$d" ] && HAS_MOD=1; done
SKIP=(); [ $HAS_MOD = 1 ] || [ ${#MODDIRS[@]} != 0 ] || SKIP=(--skip-sce-module)
# app0/: the title's own files (symlinked; package_app copies the real data into the .app)
mkdir -p "$GAME/app0"
if [ -n "$SRC_DIR" ]; then
    for f in "$SRC_DIR"/* "$SRC_DIR"/.[!.]*; do [ -e "$f" ] || continue; ln -sfn "$f" "$GAME/app0/$(basename "$f")"; done
    if [ ${#MODDIRS[@]} != 0 ]; then
        # the relinker writes <dir>/*.guest.prx under app0/: make Media/ and the module folders real directories
        # (of symlinks) so nothing is written into the title's own folder
        rm -f "$GAME/app0/Media"; mkdir -p "$GAME/app0/Media"
        for f in "$SRC_DIR"/Media/*; do ln -sfn "$f" "$GAME/app0/Media/$(basename "$f")"; done
        for d in Media/Modules Media/Plugins; do
            [ -d "$SRC_DIR/$d" ] || continue
            rm -f "$GAME/app0/$d"; mkdir -p "$GAME/app0/$d"
            for f in "$SRC_DIR/$d"/*; do [ -e "$f" ] && ln -sfn "$f" "$GAME/app0/$d/$(basename "$f")"; done
        done
    fi
else
    ln -sfn "$ELF" "$GAME/app0/eboot.bin"
fi
# AnyPS5 reads app0/sce_sys/param.json (title name etc.) and throws without it: homebrew has none, so make one.
if [ ! -e "$GAME/app0/sce_sys/param.json" ]; then
    [ -L "$GAME/app0/sce_sys" ] && { echo "note: $SRC_DIR/sce_sys has no param.json; AnyPS5 may refuse to start"; } || {
        mkdir -p "$GAME/app0/sce_sys"
        printf '{\n  "titleId": "HBRW00000",\n  "localizedParameters": { "defaultLanguage": "en-US", "en-US": { "titleName": "%s" } }\n}\n' "$NAME" > "$GAME/app0/sce_sys/param.json"
        echo "note: no sce_sys/param.json in the input; wrote a minimal one (title \"$NAME\")"
    }
fi
# writable folders AnyPS5's runtime uses beside the executable (log, /data, /temp0)
mkdir -p "$GAME/devlog/app" "$GAME/data" "$GAME/temp0"
[ ${#MODDIRS[@]} = 0 ] || echo "module folders: ${MODDIRS[*]}"
set +e
# the relinker runs in the input folder so that --module-dir paths are relative to it
# shellcheck disable=SC2086
(cd "$RELINK_IN" && "$RELINKER" --windows ${SKIP[@]+"${SKIP[@]}"} ${MODDIRS[@]+"${MODDIRS[@]}"} $RELINK_ARGS "$RELINK_IN/eboot.bin" "$GAME/$EXE" > "$WORK/relink.log" 2>&1)
rc=$?
set -e
grep -v '%' "$WORK/relink.log" | grep -E "FAIL|warning|Guest module|Converted modules|complete" || true
[ $rc = 0 ] || die "relinker failed (exit $rc); full log: $WORK/relink.log. AnyPS5 decides what it can convert; see its docs/user/USAGE.md."
# converted guest modules land under app0/... next to the exe; they are DLLs and get recompiled too
MODS=(); while IFS= read -r m; do MODS+=("$m"); done < <(cd "$GAME" && find . -name '*.guest.prx' -not -path './libs/*' | sed 's|^\./||' | sort)
echo "relinked: $GAME/$EXE (+ ${#MODS[@]} guest modules)"

# ---- 2. PS5 libraries it needs ---------------------------------------------------------------------------------------
stage 2/5 "AnyPS5 libraries (libs/*.prx) to recompile with it"
NEEDED=$(python3 -I "$ROOT/tools/ps5_needed.py" "$ELF")
if [ -n "$SRC_DIR" ]; then
    for m in "$RELINK_IN"/sce_module*/* "$RELINK_IN"/prx/*; do
        [ -f "$m" ] && head -c 4 "$m" | od -An -c | grep -q "E   L   F" && NEEDED="$NEEDED $(python3 -I "$ROOT/tools/ps5_needed.py" "$m")"
    done
fi
SEEDLIST="libc.prx libkernel.prx libkernel_web.prx $NEEDED ${WITH[*]+${WITH[*]}}"
if [ $ALL = 1 ]; then SEEDLIST="$(cd "$LIBS" && ls)"; fi
MODPATHS=(); for m in ${MODS[@]+"${MODS[@]}"}; do MODPATHS+=("$GAME/$m"); done
# shellcheck disable=SC2086
python3 -I "$ROOT/tools/ps5_deps.py" "$LIBS" $SEEDLIST ${MODPATHS[@]+"${MODPATHS[@]}"} > "$WORK/deps.txt"
[ -s "$WORK/deps.txt" ] || die "no AnyPS5 libraries matched (is $LIBS a libs/ folder of AnyPS5's Windows release?)"
rm -f "$GAME"/libs/*
while read -r f; do cp -p "$f" "$GAME/libs/"; done < "$WORK/deps.txt"
# macOS reserves [0xFC0000000, 0x7000000000) in every process; AnyPS5's AGC driver wants 0xFE0000000.
# Move it to a mappable address in the copies (docs/ps5_graphics.md). Idempotent; no-op if the driver isn't used.
python3 -I "$ROOT/tools/ps5_mac_patch.py" "$GAME/libs" || die "ps5_mac_patch.py failed on $GAME/libs"
missing_seeds=""; for s in $NEEDED ${WITH[*]+${WITH[*]}}; do b="${s%.sprx}"; b="${b%.prx}"; [ -f "$LIBS/$b.prx" ] || missing_seeds="$missing_seeds $s"; done
[ -z "$missing_seeds" ] || echo "note: not in AnyPS5's libs (the title may still run if it never calls them):$missing_seeds"
DLLS=(); while read -r f; do DLLS+=("libs/$(basename "$f")"); done < "$WORK/deps.txt"
for m in ${MODS[@]+"${MODS[@]}"}; do DLLS+=("$m"); done
echo "${#DLLS[@]} DLLs: ${DLLS[*]}"

# ---- 3. Win32 API coverage --------------------------------------------------------------------------------------------
stage 3/5 "Win32 API coverage (imports with no wintomac shim)"
(cd "$GAME" && python3 -I "$ROOT/tools/api_coverage.py" --list --with-redist "$EXE" "${DLLS[@]}" > "$WORK/missing_shims.txt" 2>&1) || true
if [ -s "$WORK/missing_shims.txt" ]; then
    echo "imports with no shim (a call to one stops the program with \"[rt] called import with no shim\"):"
    sed 's/^/  /' "$WORK/missing_shims.txt" | head -40
    echo "  (full list: $WORK/missing_shims.txt; how to add one: docs/ps5.md, 'Fixing gaps')"
else
    echo "every import is covered"
fi

# ---- 4+5. recompile, link, bundle (tools/package_app.py) -------------------------------------------------------------
stage 4/5 "recompile + link (wintomac)"
RFLAGS=""; [ -n "$SEEDS" ] && RFLAGS="--seeds-file $SEEDS"
PKG=("$PY" -I "$ROOT/tools/package_app.py" "$GAME" "$EXE" "${DLLS[@]}" --name "$NAME" --out "$OUT")
[ $RECOMPILE = 1 ] && PKG+=(--recompile)
[ -n "$RFLAGS" ] && PKG+=(--recomp-flags "$RFLAGS")
[ $LINKDATA = 1 ] && PKG+=(--link-data)
RECOMP_CMD=".venv/bin/python -m recomp $GAME/$EXE $(for d in "${DLLS[@]}"; do printf '%s ' "$GAME/$d"; done)-o build/$SLUG.gen --seeds-file $GAME/wintomac_misses.txt"
if [ $NOAPP = 1 ]; then
    GEN="$ROOT/build/$SLUG.gen"
    if [ $RECOMPILE = 1 ] || [ ! -f "$GEN/report.json" ]; then
        # shellcheck disable=SC2086
        (cd "$ROOT" && "$PY" -m recomp "$GAME/$EXE" $(for d in "${DLLS[@]}"; do printf '%s ' "$GAME/$d"; done) -o "$GEN" -j "${WINTOMAC_JOBS:-8}" $RFLAGS) \
            || die "recompile failed. Retry by hand: $RECOMP_CMD"
    fi
    (cd "$ROOT" && make -s link GEN="$GEN" OUT="$ROOT/build/$SLUG.bin") || die "link failed (runtime build error above)"
    stage 5/5 "done (no .app: --no-app)"
    echo "run headless:  cd $GAME && WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy $ROOT/build/$SLUG.bin $EXE"
    exit 0
fi
(cd "$ROOT" && "${PKG[@]}") || die "packaging failed (see the [package] lines above). Recompile by hand: $RECOMP_CMD"
stage 5/5 "done"
APP="$OUT/$NAME.app"
LAUNCH="$APP/Contents/MacOS/$(echo "$NAME" | sed -E 's/[^A-Za-z0-9._-]+//g')"
cat <<EOF
app:      $APP
headless: WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy "$LAUNCH"
windowed: "$LAUNCH"          (or double-click the app; log: ~/Library/Logs/wintomac/$NAME.log)
If it stops with "called import with no shim": that API needs a shim (docs/ps5.md, 'Fixing gaps').
If it reports missed code (wintomac_misses.txt in the app's game folder), feed it back:
  tools/ps5_to_mac.sh "$INPUT" --name "$NAME" --seeds "$APP/Contents/Resources/game/wintomac_misses.txt"
EOF
