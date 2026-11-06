// Mock libsteam_api.dylib for headless tests of the Steamworks bridge (runtime/steam.c).
// Host-native (arm64 macOS, Itanium C++ ABI), returns canned values, never talks to Steam.
// Selected with WINTOMAC_STEAM_API=build/libmock_steam_api.dylib (the Makefile does this for steam_* tests).
//
// Interface objects are built from the generated ABI table: each requested version gets a vtable in host
// (Itanium) slot order, with the mocked methods placed by method key and everything else pointing to a stub.
// The guest test (tests/src/steam_basic.c) calls them through MSVC slot numbers, so overload order, float
// arguments, hidden struct returns and struct repacking are all exercised for real.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "steam_abi.h"

#define EXPORT __attribute__((visibility("default")))
typedef uint64_t u64;

static int initialized;
static int unimplemented_calls;

// ---- objects -----------------------------------------------------------------------------------------------
typedef struct { void **vtbl; const char *version; } Obj;
static Obj *objs[64]; static int nobjs;

static u64 unimpl(void) { unimplemented_calls++; return 0; }

static const SteamIface *find_iface(const char *ver) {
    for (int i = 0; i < STEAM_NIFACES; i++) if (!strcmp(steam_ifaces[i].version, ver)) return &steam_ifaces[i];
    return NULL;
}
static void set(void **vt, const SteamIface *ifc, const char *key, void *fn) {
    for (int i = 0; i < ifc->n; i++)
        if (!strcmp(steam_methods[ifc->start + i].key, key)) { vt[steam_methods[ifc->start + i].host] = fn; return; }
    fprintf(stderr, "mock: %s has no %s\n", ifc->version, key);
    abort();
}

// ---- callbacks (Itanium CCallbackBase: vptr, uint8 flags, int iCallback) -------------------------------------
typedef struct CB { void **vptr; uint8_t flags; int icb; } CB;
typedef void (*Run1)(CB *, void *);
typedef void (*Run3)(CB *, void *, bool, u64);
typedef int (*SizeFn)(CB *);
static CB *cbs[32]; static int ncbs;
static struct { CB *cb; u64 call; } results[32]; static int nresults;
typedef struct { int id; u64 call; uint8_t data[64]; int size; int delivered; } Pending;
static Pending pend[32]; static int npend;
static Pending done[32]; static int ndone;   // completed call results, for GetAPICallResult

static void queue(int id, u64 call, const void *data, int size) {
    Pending *p = &pend[npend++]; p->id = id; p->call = call; memcpy(p->data, data, size); p->size = size; p->delivered = 0;
}

EXPORT void SteamAPI_RegisterCallback(CB *cb, int id) { cb->flags |= 1; cb->icb = id; cbs[ncbs++] = cb; }
EXPORT void SteamAPI_UnregisterCallback(CB *cb) {
    for (int i = 0; i < ncbs; i++) if (cbs[i] == cb) { cbs[i] = cbs[--ncbs]; break; }
    cb->flags &= ~1;
}
EXPORT void SteamAPI_RegisterCallResult(CB *cb, u64 call) { results[nresults].cb = cb; results[nresults++].call = call; }
EXPORT void SteamAPI_UnregisterCallResult(CB *cb, u64 call) {
    for (int i = 0; i < nresults; i++) if (results[i].cb == cb && results[i].call == call) { results[i] = results[--nresults]; break; }
}
EXPORT void SteamAPI_RunCallbacks(void) {
    Pending batch[32]; int n = npend; memcpy(batch, pend, sizeof(Pending) * n); npend = 0;
    for (int k = 0; k < n; k++) {
        Pending *p = &batch[k];
        if (p->call) {
            done[ndone++] = *p;
            for (int i = 0; i < nresults; i++)
                if (results[i].call == p->call) {
                    CB *cb = results[i].cb; results[i] = results[--nresults];
                    // libsteam_api checks the registered object's size against the payload
                    if (((SizeFn)cb->vptr[2])(cb) != p->size) { uint8_t z[64] = {0}; ((Run3)cb->vptr[1])(cb, z, true, p->call); }
                    else ((Run3)cb->vptr[1])(cb, p->data, false, p->call);
                    break;
                }
        } else {
            for (int i = 0; i < ncbs; i++)
                if (cbs[i]->icb == p->id) {
                    if (((SizeFn)cbs[i]->vptr[2])(cbs[i]) != p->size) continue;
                    ((Run1)cbs[i]->vptr[0])(cbs[i], p->data);
                }
        }
    }
}

// Callback payloads in the macOS layout (pack 4), written field by field.
static void persona_state_change(void) {   // PersonaStateChange_t (304): uint64 m_ulSteamID; int m_nChangeFlags
    uint8_t b[12]; u64 id = 76561197960287931ull; int fl = 0x401;
    memcpy(b, &id, 8); memcpy(b + 8, &fl, 4); queue(304, 0, b, 12);
}
static void user_stats_received(u64 call, u64 user) {   // UserStatsReceived_t (1101): uint64 game; EResult; CSteamID
    uint8_t b[20]; u64 game = 480; int res = 1;
    memcpy(b, &game, 8); memcpy(b + 8, &res, 4); memcpy(b + 12, &user, 8); queue(1101, call, b, 20);
}

// ---- ISteamUser ----------------------------------------------------------------------------------------------
static int u_hsteamuser(Obj *o) { return 1; }
static bool u_loggedon(Obj *o) { return true; }
static u64 u_steamid(Obj *o) { return 76561197960287930ull; }   // CSteamID: trivially copyable, returned in x0

// ---- ISteamFriends -------------------------------------------------------------------------------------------
static const char *f_persona(Obj *o) { return strstr(o->version, "015") ? "MockPlayer015" : "MockPlayer"; }
static int f_count(Obj *o, int flags) { return flags == 4 ? 3 : 0; }
static u64 f_byindex(Obj *o, int i, int flags) { return 76561197960287940ull + i; }
static const char *f_name(Obj *o, u64 id) { static char b[64]; snprintf(b, sizeof b, "Friend%llu", (unsigned long long)(id - 76561197960287940ull)); return b; }

// ---- ISteamApps ----------------------------------------------------------------------------------------------
static bool a_subscribed(Obj *o, uint32_t app) { return app == 480; }
static bool a_dlc(Obj *o, uint32_t app) { return app == 1001; }
static const char *a_lang(Obj *o) { return "english"; }
static int a_build(Obj *o) { return 1234; }

// ---- ISteamUtils ---------------------------------------------------------------------------------------------
static uint32_t ut_appid(Obj *o) { return 480; }
static const char *ut_country(Obj *o) { return "NL"; }
static bool ut_completed(Obj *o, u64 call, bool *failed) {
    for (int i = 0; i < ndone; i++) if (done[i].call == call) { if (failed) *failed = false; return true; }
    return false;
}
static bool ut_result(Obj *o, u64 call, void *buf, int cub, int id, bool *failed) {
    for (int i = 0; i < ndone; i++)
        if (done[i].call == call) {
            if (done[i].id != id || done[i].size != cub) { if (failed) *failed = true; return false; }   // size checked like Steam
            memcpy(buf, done[i].data, cub); if (failed) *failed = false; return true;
        }
    return false;
}
typedef void (*WarnHook)(int, const char *);
static void ut_hook(Obj *o, WarnHook h) { if (h) h(1, "mock warning"); }

// ---- ISteamUserStats -----------------------------------------------------------------------------------------
static int32_t stat_i = 7; static float stat_f = 2.5f; static double avg_sum; static char last_ach[64];
static bool s_request(Obj *o) { user_stats_received(0, 76561197960287930ull); return true; }
static bool s_get_i(Obj *o, const char *n, int32_t *v) { if (strcmp(n, "kills")) return false; *v = stat_i; return true; }
static bool s_get_f(Obj *o, const char *n, float *v) { if (strcmp(n, "dist")) return false; *v = stat_f; return true; }
static bool s_set_i(Obj *o, const char *n, int32_t v) { if (strcmp(n, "kills")) return false; stat_i = v; return true; }
static bool s_set_f(Obj *o, const char *n, float v) { if (strcmp(n, "dist")) return false; stat_f = v; return true; }
static bool s_avg(Obj *o, const char *n, float count, double secs) { avg_sum = count / secs; return !strcmp(n, "rate") && count == 30.0f && secs == 60.0; }
static bool s_get_ach(Obj *o, const char *n, bool *a) { *a = !strcmp(n, last_ach); return true; }
static bool s_set_ach(Obj *o, const char *n) { snprintf(last_ach, sizeof last_ach, "%s", n); return true; }
static bool s_store(Obj *o) { return true; }
static u64 s_req_user(Obj *o, u64 user) { user_stats_received(0x1111, user); return 0x1111; }
static u64 s_find_lb(Obj *o, const char *name) {   // LeaderboardFindResult_t (1104): uint64 handle; uint8 found
    uint8_t b[12] = {0}; u64 h = 0xABCDEF; memcpy(b, &h, 8); b[8] = !strcmp(name, "Best Times");
    queue(1104, 0x2222, b, 12); return 0x2222;
}
// LeaderboardEntry_t (macOS pack 4): CSteamID@0, rank@8, score@12, cDetails@16, UGCHandle_t@20
static bool s_lb_entry(Obj *o, u64 entries, int index, uint8_t *e, int32_t *details, int max) {
    u64 id = 76561197960287950ull + index, ugc = 0x5555000000000000ull + index; int32_t rank = index + 1, score = 1000 - index, nd = 2;
    memcpy(e, &id, 8); memcpy(e + 8, &rank, 4); memcpy(e + 12, &score, 4); memcpy(e + 16, &nd, 4); memcpy(e + 20, &ugc, 8);
    for (int i = 0; i < max && i < nd; i++) details[i] = 10 * (i + 1);
    return entries == 0xABCDEF;
}

// ---- ISteamRemoteStorage -------------------------------------------------------------------------------------
static char rs_name[64]; static uint8_t rs_data[256]; static int rs_len = -1;
static bool r_write(Obj *o, const char *n, const void *d, int32_t len) {
    if (len < 0 || len > 256) return false;
    snprintf(rs_name, sizeof rs_name, "%s", n); memcpy(rs_data, d, len); rs_len = len; return true;
}
static int32_t r_read(Obj *o, const char *n, void *d, int32_t len) {
    if (strcmp(n, rs_name) || rs_len < 0) return 0;
    int k = len < rs_len ? len : rs_len; memcpy(d, rs_data, k); return k;
}
static bool r_exists(Obj *o, const char *n) { return rs_len >= 0 && !strcmp(n, rs_name); }
static int32_t r_size(Obj *o, const char *n) { return r_exists(o, n) ? rs_len : 0; }
static bool r_quota(Obj *o, u64 *total, u64 *avail) { *total = 1000000; *avail = 1000000 - (rs_len > 0 ? rs_len : 0); return true; }

static void *make(const char *ver);
// ---- ISteamClient --------------------------------------------------------------------------------------------
static void *c_friends(Obj *o, int u, int p, const char *ver) { return u == 1 && p == 1 ? make(ver) : NULL; }
static void *c_utils(Obj *o, int p, const char *ver) { return p == 1 ? make(ver) : NULL; }
static void *c_stats(Obj *o, int u, int p, const char *ver) { return u == 1 && p == 1 ? make(ver) : NULL; }
static void c_hook(Obj *o, WarnHook h) { if (h) h(0, "mock client warning"); }

static void *make(const char *ver) {
    if (!initialized || !ver) return NULL;
    for (int i = 0; i < nobjs; i++) if (!strcmp(objs[i]->version, ver)) return objs[i];
    const SteamIface *ifc = find_iface(ver);
    if (!ifc) return NULL;
    void **vt = calloc(ifc->n + 2, sizeof(void *));
    for (int i = 0; i < ifc->n + 2; i++) vt[i] = (void *)unimpl;
    if (!strcmp(ifc->cls, "ISteamUser")) {
        set(vt, ifc, "GetHSteamUser()", u_hsteamuser); set(vt, ifc, "BLoggedOn()", u_loggedon); set(vt, ifc, "GetSteamID()", u_steamid);
    } else if (!strcmp(ifc->cls, "ISteamFriends")) {
        set(vt, ifc, "GetPersonaName()", f_persona); set(vt, ifc, "GetFriendCount(int)", f_count);
        set(vt, ifc, "GetFriendByIndex(int, int)", f_byindex); set(vt, ifc, "GetFriendPersonaName(CSteamID)", f_name);
    } else if (!strcmp(ifc->cls, "ISteamApps")) {
        set(vt, ifc, "BIsSubscribedApp(AppId_t)", a_subscribed); set(vt, ifc, "BIsDlcInstalled(AppId_t)", a_dlc);
        set(vt, ifc, "GetCurrentGameLanguage()", a_lang); set(vt, ifc, "GetAppBuildId()", a_build);
    } else if (!strcmp(ifc->cls, "ISteamUtils")) {
        set(vt, ifc, "GetAppID()", ut_appid); set(vt, ifc, "GetIPCountry()", ut_country);
        set(vt, ifc, "IsAPICallCompleted(SteamAPICall_t, bool *)", ut_completed);
        set(vt, ifc, "GetAPICallResult(SteamAPICall_t, void *, int, int, bool *)", ut_result);
        set(vt, ifc, "SetWarningMessageHook(SteamAPIWarningMessageHook_t)", ut_hook);
    } else if (!strcmp(ifc->cls, "ISteamUserStats")) {
        set(vt, ifc, "RequestCurrentStats()", s_request);
        set(vt, ifc, "GetStat(const char *, int32 *)", s_get_i); set(vt, ifc, "GetStat(const char *, float *)", s_get_f);
        set(vt, ifc, "SetStat(const char *, int32)", s_set_i); set(vt, ifc, "SetStat(const char *, float)", s_set_f);
        set(vt, ifc, "UpdateAvgRateStat(const char *, float, double)", s_avg);
        set(vt, ifc, "GetAchievement(const char *, bool *)", s_get_ach); set(vt, ifc, "SetAchievement(const char *)", s_set_ach);
        set(vt, ifc, "StoreStats()", s_store); set(vt, ifc, "RequestUserStats(CSteamID)", s_req_user);
        set(vt, ifc, "FindLeaderboard(const char *)", s_find_lb);
        set(vt, ifc, "GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t, int, LeaderboardEntry_t *, int32 *, int)", s_lb_entry);
    } else if (!strcmp(ifc->cls, "ISteamRemoteStorage")) {
        set(vt, ifc, "FileWrite(const char *, const void *, int32)", r_write); set(vt, ifc, "FileRead(const char *, void *, int32)", r_read);
        set(vt, ifc, "FileExists(const char *)", r_exists); set(vt, ifc, "GetFileSize(const char *)", r_size);
        set(vt, ifc, "GetQuota(uint64 *, uint64 *)", r_quota);
    } else if (!strcmp(ifc->cls, "ISteamClient")) {
        set(vt, ifc, "GetISteamFriends(HSteamUser, HSteamPipe, const char *)", c_friends);
        set(vt, ifc, "GetISteamUtils(HSteamPipe, const char *)", c_utils);
        set(vt, ifc, "GetISteamUserStats(HSteamUser, HSteamPipe, const char *)", c_stats);
        set(vt, ifc, "SetWarningMessageHook(SteamAPIWarningMessageHook_t)", c_hook);
    } else return NULL;
    Obj *o = calloc(1, sizeof *o); o->vtbl = vt; o->version = strdup(ver);
    objs[nobjs++] = o;
    return o;
}

// ---- plain exports -------------------------------------------------------------------------------------------
EXPORT bool SteamAPI_RestartAppIfNecessary(uint32_t app) { return false; }
EXPORT bool SteamAPI_Init(void) { initialized = 1; persona_state_change(); return true; }
EXPORT void SteamAPI_Shutdown(void) { initialized = 0; }
EXPORT bool SteamAPI_IsSteamRunning(void) { return true; }
EXPORT int SteamAPI_GetHSteamUser(void) { return initialized ? 1 : 0; }
EXPORT int SteamAPI_GetHSteamPipe(void) { return initialized ? 1 : 0; }
EXPORT void SteamAPI_ReleaseCurrentThreadMemory(void) {}
EXPORT void *SteamInternal_FindOrCreateUserInterface(int user, const char *ver) { return user == 1 ? make(ver) : NULL; }
EXPORT void *SteamInternal_CreateInterface(const char *ver) { return make(ver); }
EXPORT int mock_steam_unimplemented_calls(void) { return unimplemented_calls; }
