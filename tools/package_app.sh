#!/bin/sh
# Package a recompiled Windows game as a self-contained macOS .app. Thin wrapper; see tools/package_app.py and
# docs/packaging.md.
#   tools/package_app.sh <game_dir> <exe> [dlls...] --name "Game" [--args "..."] [--icon path] [--link-data]
root=$(cd "$(dirname "$0")/.." && pwd)
py="$root/.venv/bin/python"; [ -x "$py" ] || py=python3
exec "$py" -I "$root/tools/package_app.py" "$@"
