#!/bin/sh
# RULE_LAUNCH_LINK launcher for AnyPS5 cross builds (cross_overrides.cmake): instead of linking a build-time tool
# for Windows, put the native build of the same tool (first argument) where the link would write its output (-o).
host="$1"; shift
out=""
while [ $# -gt 0 ]; do
    if [ "$1" = "-o" ]; then out="$2"; break; fi
    shift
done
[ -n "$out" ] || { echo "host_tool_link.sh: no -o in the link command" >&2; exit 1; }
cp -f "$host" "$out" && chmod +x "$out"
