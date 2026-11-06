// Types of the generated Steamworks ABI tables (runtime/gen/steam_ifaces.h, tools/gen_steam.py).
// Shared by runtime/steam.c and the test mock (tests/mock/mock_steam_api.c).
#pragma once
#include <stdint.h>

typedef struct { uint8_t kind, size; uint16_t flags; uint16_t conv; } SteamArg;   // K_REC: flags = Windows size
typedef struct { const char *name, *key; uint16_t host; uint8_t nargs; SteamArg ret; uint32_t args; } SteamMethod;
typedef struct { const char *version, *cls, *sdk; uint32_t start; uint16_t n; } SteamIface;   // sorted by version
typedef struct { const char *name; uint16_t win_size, mac_size, nf; uint32_t first; } SteamConv;   // nf=0: same layout
typedef struct { uint16_t w, m, n; } SteamField;
typedef struct { int id; uint16_t n; uint32_t first; } SteamCallbackInfo;   // sorted by id
typedef struct { const char *name, *cls, *method, *key; } SteamFlatMethod;
typedef struct { const char *name; SteamArg ret; uint8_t nargs; uint32_t args; } SteamFlatPlain;
typedef struct { const char *name, *version; uint8_t kind; } SteamFlatAccessor;   // kind: 0 user, 1 gameserver, 2 global
enum { K_VOID, K_INT, K_FLT, K_PTR, K_REC, K_PREC, K_POLY, K_FN, K_HOOK, K_IFACE };
// K_INT flags&1: signed. K_PREC flags&1: const pointee (input only); conv = steam_convs index.
// K_IFACE (ISteamClient::GetISteam*): flags = index of the version-string argument.
#include "gen/steam_ifaces.h"
