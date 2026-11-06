#!/bin/bash
# make ps5-draw: PS5 graphics regression tests (docs/ps5_graphics.md).
#   gfx_flip  tests/ps5/gfx_flip.c: CPU-drawn pattern flipped through libSceVideoOut (present path), 3 frames checked
#             pixel by pixel (tests/ps5/check_frame.py)
#   pad_input tests/ps5/pad_input.c: scePad state from keys typed into the window and from a virtual gamepad
#   agc_draw  tests/ps5/agc_draw.c: raw AGC command buffer + RDNA2 shaders (tests/ps5/gen_agc_draw.py) drawn by
#             libSceAgcDriver (PS5 shader -> SPIR-V -> Vulkan); sample points checked (tests/ps5/check_agc_frame.py)
# Each: ps5-payload-sdk build -> AnyPS5 relinker --windows -> recompile with the AnyPS5 libraries it needs (patched by
# tools/ps5_mac_patch.py) -> headless run with APS5_DUMP_FRAMES (AnyPS5 reads the presented frame back on the GPU).
# Usage: tools/ps5_gfx_check.sh [gfx_flip|agc_draw ...] (default both); PS5_DRAW_NO_BUILD=1 reruns existing binaries. Skips (exit 0) without the AnyPS5 libraries.
# A cold build links ~8 M instructions of recompiled code (10+ min); later runs hit ccache.
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
LIBS=third_party/anyps5-win/prx/libs
RELINKER=third_party/anyps5-win/build-relinker/core/relinker/relinker
[ -f $LIBS/libSceAgcDriver.prx ] && [ -x $RELINKER ] && [ -x third_party/ps5-payload-sdk/sdk/ps5-payload-sdk/bin/prospero-clang ] \
  || { echo "ps5-draw: skipped (run tools/ps5_setup.sh --sdk first)"; exit 0; }
TESTS=("$@"); [ ${#TESTS[@]} = 0 ] && TESTS=(gfx_flip agc_draw pad_input)
fail=0
for T in "${TESTS[@]}"; do
  W=build/ps5/$T; GEN=build/ps5_$T.gen; BIN=build/ps5_$T.bin
  case $T in gfx_flip) LFLAGS="-lSceVideoOut";; agc_draw) LFLAGS="-lSceVideoOut -lSceAgcDriver";; pad_input) LFLAGS="-lSceVideoOut -lScePad -lSceUserService";; *) echo "unknown test $T"; exit 1;; esac
  [ -n "$PS5_DRAW_NO_BUILD" ] && [ -x "$BIN" ] || (
    set -e
    mkdir -p "$W/in" "$W/libs" "$W/app0"
    rm -f "$W/in/eboot.bin"; PS5_LDFLAGS= tools/ps5_build.sh tests/ps5/$T.c "$W/in/eboot.bin" $LFLAGS; [ -f "$W/in/eboot.bin" ]
    "$RELINKER" --windows --skip-sce-module "$W/in/eboot.bin" "$W/$T.exe" > "$W/relink.log" 2>&1 || { cat "$W/relink.log"; exit 1; }
    python3 -I tools/ps5_deps.py "$LIBS" libc.prx libkernel.prx libkernel_web.prx $(python3 -I tools/ps5_needed.py "$W/in/eboot.bin") > "$W/deps.txt"
    for f in $(cat "$W/deps.txt"); do cp -p "$f" "$W/libs/"; done
    python3 -I tools/ps5_mac_patch.py "$W/libs"
    ln -sfn "$ROOT/tests/ps5/app0/sce_sys" "$W/app0/sce_sys"
    if [ $T = agc_draw ]; then rm -f "$W/app0/agc_draw.bin"; python3 -I tests/ps5/gen_agc_draw.py "$W/app0/agc_draw.bin"; fi
    .venv/bin/python -m recomp "$W/$T.exe" $(sed "s|.*/|$W/libs/|" "$W/deps.txt") -o "$GEN" -j "${WINTOMAC_JOBS:-3}" > "$W/recomp.log" 2>&1 || { tail -20 "$W/recomp.log"; exit 1; }
    # the generated code is compiled at -O1 (several times faster to build than -O2; speed is irrelevant here)
    CF=$(make -s -p -n link GEN=x OUT=y 2>/dev/null | sed -n 's/^CFLAGS *:= *//p' | head -1 | sed 's/-O2/-O1/')
    WINTOMAC_JOBS="${WINTOMAC_JOBS:-3}" make -s link GEN="$GEN" OUT="$BIN" CFLAGS="$CF"
  ) || { echo "FAIL ps5-draw $T (build)"; fail=1; continue; }
  (
    cd "$W" && rm -f frame_00?.bmp
    if [ $T = pad_input ]; then
      # keyboard typed into the window (user32.c test hook) and a scripted virtual gamepad (xinput.c test hook)
      WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy WINTOMAC_TEST_DETACHED_KEYS=1 PAD_SECONDS=5 perl -e 'alarm 120; exec @ARGV' "$ROOT/$BIN" $T.exe > run_keys.log 2>&1 || exit 1
      WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy WINTOMAC_VIRTUAL_PAD=1 PAD_SECONDS=3 perl -e 'alarm 120; exec @ARGV' "$ROOT/$BIN" $T.exe > run_pad.log 2>&1 || exit 1
      grep -h "^t=" run_keys.log run_pad.log
      ok=1
      for want in "buttons=004000" "buttons=000010" "left=128,  0"; do grep -q "$want" run_keys.log || { echo "keyboard: no '$want'"; ok=0; }; done
      # xinput.c's virtual pad holds A, D-pad up, RB, left stick (+16000, -16000), right stick up -> Cross|R1|Up
      for want in "buttons=004810 left=190, 65 right=128,  0"; do grep -q "$want" run_pad.log || { echo "gamepad: no '$want'"; ok=0; }; done
      [ $ok = 1 ]; exit $?
    fi
    WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy APS5_DUMP_FRAMES=3 APS5_DUMP_SCALE=1 perl -e 'alarm 180; exec @ARGV' \
      "$ROOT/$BIN" $T.exe > run.log 2>&1
    rc=$?
    grep -E "^(sce|frame|flip|label|cpu|check)" run.log
    if [ $T = gfx_flip ]; then
      [ $rc = 0 ] && grep -Eq "flips done: (all|3 )" run.log && python3 -I "$ROOT/tests/ps5/check_frame.py" . 3
    else
      [ $rc = 0 ] && grep -q "label written" run.log && python3 -I "$ROOT/tests/ps5/check_agc_frame.py" .
    fi
  ) && echo "PASS ps5-draw $T" || { echo "FAIL ps5-draw $T (log $W/run.log)"; tail -15 "$W/run.log"; fail=1; }
done
exit $fail
