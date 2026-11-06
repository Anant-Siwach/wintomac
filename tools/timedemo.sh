#!/bin/sh
# Quake `timedemo demo1` with a recompiled QuakeSpasm or vkQuake. Opens a window: run it yourself, it is not
# part of `make check` or tools/bench.sh. Sound goes to SDL's dummy driver unless SDL_AUDIO_DRIVER is set.
#
#   tools/timedemo.sh <recompiled .bin> <game .exe> <label> [runs] [extra game args...]
#
# Each run's full output goes to build/timedemo/<label>.<n>.log; the fps lines are appended to
# build/timedemo/results.txt as "<label> <frames> <seconds> <fps>". Compare labels with tools/timedemo_compare.py.
# The game is stopped as soon as the timedemo result is printed (or after 300 s).
set -e
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); exe=$2; label=$3; runs=${4:-3}
shift 3; [ $# -gt 0 ] && shift
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/build/timedemo; mkdir -p "$out"
export SDL_AUDIO_DRIVER=${SDL_AUDIO_DRIVER:-dummy}
cd "$(dirname "$exe")"
for n in $(seq "$runs"); do
  log=$out/$label.$n.log
  "$bin" "$(basename "$exe")" -window -width 1280 -height 720 +vid_vsync 0 +host_maxfps 0 "$@" +timedemo demo1 > "$log" 2>&1 &
  pid=$!
  t=0
  while kill -0 $pid 2>/dev/null && ! grep -q "frames .* seconds .* fps" "$log" && [ $t -lt 300 ]; do sleep 1; t=$((t + 1)); done
  kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
  line=$(grep -o "[0-9]* frames *[0-9.]* seconds *[0-9.]* fps" "$log" | tail -1)
  echo "$label run $n: ${line:-no result (see $log)}"
  [ -n "$line" ] && echo "$label $(echo "$line" | awk '{print $1, $3, $5}')" >> "$out/results.txt"
done
