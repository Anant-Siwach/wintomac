#!/bin/sh
# publish_audit.sh [dir]: check a tree (default: repo root) before publishing.
# Fails if it finds anything that must not be redistributed: game files and data, Windows/PS executables,
# Valve SDK headers, Apple-licensed tools, keys. Run it on the exact tree you are about to push
# (e.g. after `git add`, run it on `git ls-files` output via --git).
set -e
root=${1:-.}; [ "$root" = "--git" ] && root=.
cd "$root"
if [ "$1" = "--git" ] || [ -d .git ]; then
  list() { git ls-files; }
else
  list() { find . -type f -not -path './.git/*' | sed 's|^\./||'; }
fi
bad=0
flag() { echo "BLOCK: $1  ($2)"; bad=1; }

list | while IFS= read -r f; do
  case "$f" in
    third_party/games/*|third_party/corpus/*) flag "$f" "game/corpus files are never published" ;;
    third_party/proton-lsteamclient/*) flag "$f" "Steamworks SDK headers: redistribution forbidden" ;;
    third_party/metal-shaderconverter/*) flag "$f" "Apple-licensed: check the licence before any redistribution" ;;
    *.exe|*.dll|*.EXE|*.DLL|*.sys|*.elf|*.self|*.sprx|*.prx|*.bin|*.pkg|*.wad|*.pk3|*.pak) flag "$f" "binary game or system files" ;;
    *.pem|*.key|*.p12|*keys*.txt) flag "$f" "possible keys or secrets" ;;
  esac
done > /tmp/publish_audit.$$ || true
if [ -s /tmp/publish_audit.$$ ]; then
  cat /tmp/publish_audit.$$; rm -f /tmp/publish_audit.$$
  echo "publish_audit: FAILED. Remove or ignore the files above before publishing."
  exit 1
fi
rm -f /tmp/publish_audit.$$
# large files are a smell even when allowed
big=$(list | while IFS= read -r f; do if [ -f "$f" ] && [ "$(stat -f %z "$f")" -gt 5000000 ]; then echo "$f"; fi; done)
if [ -n "$big" ]; then echo "WARN: files over 5 MB (intended?):"; echo "$big"; fi
echo "publish_audit: OK"
