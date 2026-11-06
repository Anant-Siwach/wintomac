#!/bin/sh
# Fetch the Steamworks interface headers that Proton vendors (lsteamclient/steamworks_sdk_*), as input for
# tools/gen_steam.py (`make steam-gen`). Sparse checkout of github.com/ValveSoftware/Proton, lsteamclient/ only.
#
# Licence: Proton's own code is BSD-3-Clause (Valve), but the lsteamclient/ directory carries the Steamworks SDK
# licence (lsteamclient/LICENSE). We only read the headers as data to extract ABI facts (vtable slots, struct
# offsets, names); nothing from them is compiled into wintomac or redistributed. See docs/steam.md.
set -e
cd "$(dirname "$0")/.."
D=third_party/proton-lsteamclient
if [ -d $D/src/lsteamclient ]; then echo "$D already present"; exit 0; fi
mkdir -p $D
git clone --depth 1 --filter=blob:none --sparse https://github.com/ValveSoftware/Proton.git $D/src
git -C $D/src sparse-checkout set lsteamclient
git -C $D/src log -1 --format='Proton commit %H (%cd)'
