// Steamworks bridge test (guest side). Built with mingw against an import library for steam_api64.dll
// (tools/steam_api64.def); at run time the bridge (runtime/steam.c) forwards to the mock libsteam_api.dylib
// (tests/mock/mock_steam_api.c) selected by WINTOMAC_STEAM_API. Expected output: tests/expected/steam_basic.out.
//
// No Steamworks SDK headers: the few declarations below are written from the public API documentation.
// C++ interfaces are called the way MSVC-built games call them: through vtable slots in MSVC order (overloads
// reversed), class return values through a hidden pointer after `this`, Windows struct packing (8).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t bool8;
#define IMP __declspec(dllimport)
typedef struct IFace { void **vt; } IFace;
typedef uint64_t SteamAPICall_t;

IMP bool8 SteamAPI_RestartAppIfNecessary(uint32_t);
IMP bool8 SteamAPI_Init(void);
IMP void SteamAPI_Shutdown(void);
IMP bool8 SteamAPI_IsSteamRunning(void);
IMP void SteamAPI_RunCallbacks(void);
IMP int SteamAPI_GetHSteamUser(void);
IMP int SteamAPI_GetHSteamPipe(void);
IMP void SteamAPI_RegisterCallback(void *cb, int id);
IMP void SteamAPI_UnregisterCallback(void *cb);
IMP void SteamAPI_RegisterCallResult(void *cb, SteamAPICall_t call);
IMP void SteamAPI_UnregisterCallResult(void *cb, SteamAPICall_t call);
IMP void *SteamInternal_ContextInit(void *data);
IMP void *SteamInternal_FindOrCreateUserInterface(int user, const char *version);
IMP void *SteamInternal_CreateInterface(const char *version);
IMP IFace *SteamUtils(void);   // pre-1.4x export
// flat API
IMP IFace *SteamAPI_SteamUser_v023(void);
IMP IFace *SteamAPI_SteamFriends_v017(void);
IMP IFace *SteamAPI_SteamApps_v008(void);
IMP IFace *SteamAPI_SteamUtils_v010(void);
IMP IFace *SteamAPI_SteamUserStats_v012(void);
IMP IFace *SteamAPI_SteamRemoteStorage_v016(void);
IMP uint64_t SteamAPI_ISteamUser_GetSteamID(IFace *);
IMP bool8 SteamAPI_ISteamUser_BLoggedOn(IFace *);
IMP const char *SteamAPI_ISteamFriends_GetPersonaName(IFace *);
IMP int SteamAPI_ISteamFriends_GetFriendCount(IFace *, int);
IMP uint64_t SteamAPI_ISteamFriends_GetFriendByIndex(IFace *, int, int);
IMP const char *SteamAPI_ISteamFriends_GetFriendPersonaName(IFace *, uint64_t);
IMP bool8 SteamAPI_ISteamApps_BIsSubscribedApp(IFace *, uint32_t);
IMP bool8 SteamAPI_ISteamApps_BIsDlcInstalled(IFace *, uint32_t);
IMP const char *SteamAPI_ISteamApps_GetCurrentGameLanguage(IFace *);
IMP uint32_t SteamAPI_ISteamUtils_GetAppID(IFace *);
IMP const char *SteamAPI_ISteamUtils_GetIPCountry(IFace *);
IMP bool8 SteamAPI_ISteamUserStats_GetStatInt32(IFace *, const char *, int32_t *);
IMP bool8 SteamAPI_ISteamUserStats_GetStatFloat(IFace *, const char *, float *);
IMP bool8 SteamAPI_ISteamUserStats_SetStatFloat(IFace *, const char *, float);
IMP bool8 SteamAPI_ISteamUserStats_SetAchievement(IFace *, const char *);
IMP bool8 SteamAPI_ISteamUserStats_GetAchievement(IFace *, const char *, bool8 *);
IMP bool8 SteamAPI_ISteamRemoteStorage_FileWrite(IFace *, const char *, const void *, int32_t);
IMP int32_t SteamAPI_ISteamRemoteStorage_FileRead(IFace *, const char *, void *, int32_t);
IMP bool8 SteamAPI_ISteamRemoteStorage_GetQuota(IFace *, uint64_t *, uint64_t *);

// ---- callback payloads, Windows layout (default packing = 8) ----
typedef struct { uint64_t m_ulSteamID; int m_nChangeFlags; } PersonaStateChange_t;                       // 304, 16 bytes
typedef struct { uint64_t m_nGameID; int m_eResult; uint32_t id_lo, id_hi; } UserStatsReceived_t;        // 1101, 24 (CSteamID is pack(1))
typedef struct { uint64_t m_hSteamLeaderboard; uint8_t m_bLeaderboardFound; } LeaderboardFindResult_t;   // 1104, 16
typedef struct { uint32_t id_lo, id_hi; int32_t rank, score, details; uint64_t ugc; } LeaderboardEntry_t; // 32

// ---- CCallbackBase as MSVC lays it out: vtable { Run(p, bIOFailure, hCall), Run(p), GetCallbackSizeBytes() } ----
typedef struct CB CB;
typedef struct { void (*run3)(CB *, void *, bool8, SteamAPICall_t); void (*run1)(CB *, void *); int (*size)(CB *); } CBVtbl;
struct CB { const CBVtbl *vt; uint8_t m_nCallbackFlags; int m_iCallback; const char *label; int size; };

static void on_run1(CB *self, void *p) {
    if (self->m_iCallback == 304) {
        PersonaStateChange_t *e = p;
        printf("callback %s: steamid=%llu flags=0x%x\n", self->label, (unsigned long long)e->m_ulSteamID, e->m_nChangeFlags);
    } else if (self->m_iCallback == 1101) {
        UserStatsReceived_t *e = p;
        printf("callback %s: game=%llu result=%d user=%llu\n", self->label, (unsigned long long)e->m_nGameID, e->m_eResult,
               (unsigned long long)e->id_lo | (unsigned long long)e->id_hi << 32);
    }
}
static void on_run3(CB *self, void *p, bool8 io_failure, SteamAPICall_t call) {
    printf("call result %s: call=0x%llx failed=%d\n", self->label, (unsigned long long)call, io_failure);
    if (!io_failure) on_run1(self, p);
}
static int cb_size(CB *self) { return self->size; }
static const CBVtbl cb_vtbl = { on_run3, on_run1, cb_size };

// ---- MSVC-convention interface calls ----
#define VCALL(obj, slot, type, ...) ((type)(obj)->vt[slot])((obj), ##__VA_ARGS__)

static void warn_hook(int severity, const char *text) { printf("warning hook: %d %s\n", severity, text); }

// SteamInternal_ContextInit data: { void (*init)(void *ctx), uintptr counter, ctx }
static IFace *g_user021;
static int init_calls;
static void init_ctx(void *ctx) {
    init_calls++;
    *(IFace **)ctx = SteamInternal_FindOrCreateUserInterface(SteamAPI_GetHSteamUser(), "SteamUser021");
}
static void *ctx_data[3] = { (void *)init_ctx, 0, 0 };
static IFace *SteamUser021(void) { return *(IFace **)SteamInternal_ContextInit(ctx_data); }

int main(void) {
    printf("restart=%d\n", SteamAPI_RestartAppIfNecessary(480));

    // CCallback members are usually constructed (and registered) before SteamAPI_Init.
    CB persona = { &cb_vtbl, 0, 0, "PersonaStateChange", sizeof(PersonaStateChange_t) };
    CB stats = { &cb_vtbl, 0, 0, "UserStatsReceived", sizeof(UserStatsReceived_t) };
    SteamAPI_RegisterCallback(&persona, 304);
    SteamAPI_RegisterCallback(&stats, 1101);
    printf("registered: flags=%d id=%d, flags=%d id=%d\n", persona.m_nCallbackFlags, persona.m_iCallback,
           stats.m_nCallbackFlags, stats.m_iCallback);
    printf("before init: user=%p\n", (void *)SteamUser021());

    int init = SteamAPI_Init(), running = SteamAPI_IsSteamRunning(), hsu = SteamAPI_GetHSteamUser(), hsp = SteamAPI_GetHSteamPipe();
    printf("init=%d running=%d user=%d pipe=%d\n", init, running, hsu, hsp);
    if (!init) {   // what a game sees without Steam (or without libsteam_api.dylib): no interfaces
        printf("no Steam: user=%p flat user=%p\n", (void *)SteamUser021(), (void *)SteamAPI_SteamUser_v023());
        return 1;
    }

    // ---- flat API ----
    IFace *user = SteamAPI_SteamUser_v023(), *friends = SteamAPI_SteamFriends_v017(), *apps = SteamAPI_SteamApps_v008();
    IFace *utils = SteamAPI_SteamUtils_v010(), *ustats = SteamAPI_SteamUserStats_v012(), *rs = SteamAPI_SteamRemoteStorage_v016();
    printf("flat: interfaces %d%d%d%d%d%d, same object again: %d\n", !!user, !!friends, !!apps, !!utils, !!ustats, !!rs,
           SteamAPI_SteamUser_v023() == user);
    printf("flat: steamid=%llu loggedon=%d\n", (unsigned long long)SteamAPI_ISteamUser_GetSteamID(user), SteamAPI_ISteamUser_BLoggedOn(user));
    printf("flat: persona=%s friends=%d\n", SteamAPI_ISteamFriends_GetPersonaName(friends), SteamAPI_ISteamFriends_GetFriendCount(friends, 4));
    uint64_t f2 = SteamAPI_ISteamFriends_GetFriendByIndex(friends, 2, 4);
    printf("flat: friend[2]=%llu name=%s\n", (unsigned long long)f2, SteamAPI_ISteamFriends_GetFriendPersonaName(friends, f2));
    printf("flat: subscribed(480)=%d subscribed(570)=%d dlc(1001)=%d lang=%s\n", SteamAPI_ISteamApps_BIsSubscribedApp(apps, 480),
           SteamAPI_ISteamApps_BIsSubscribedApp(apps, 570), SteamAPI_ISteamApps_BIsDlcInstalled(apps, 1001),
           SteamAPI_ISteamApps_GetCurrentGameLanguage(apps));
    printf("flat: appid=%u country=%s\n", SteamAPI_ISteamUtils_GetAppID(utils), SteamAPI_ISteamUtils_GetIPCountry(utils));
    int32_t kills = 0; float dist = 0;
    int ok1 = SteamAPI_ISteamUserStats_GetStatInt32(ustats, "kills", &kills), ok2 = SteamAPI_ISteamUserStats_GetStatFloat(ustats, "dist", &dist);
    printf("flat: kills=%d (%d) dist=%.2f (%d)\n", kills, ok1, dist, ok2);
    printf("flat: setstat=%d ", SteamAPI_ISteamUserStats_SetStatFloat(ustats, "dist", 9.75f));
    SteamAPI_ISteamUserStats_GetStatFloat(ustats, "dist", &dist);
    bool8 ach = 0; SteamAPI_ISteamUserStats_SetAchievement(ustats, "ACH_WIN");
    SteamAPI_ISteamUserStats_GetAchievement(ustats, "ACH_WIN", &ach);
    printf("dist=%.2f ach=%d\n", dist, ach);
    const char msg[] = "save game 1";
    char back[32] = {0}; uint64_t total = 0, avail = 0;
    int w = SteamAPI_ISteamRemoteStorage_FileWrite(rs, "save.dat", msg, sizeof msg);
    int r = SteamAPI_ISteamRemoteStorage_FileRead(rs, "save.dat", back, sizeof back);
    SteamAPI_ISteamRemoteStorage_GetQuota(rs, &total, &avail);
    printf("flat: cloud write=%d read=%d '%s' quota=%llu/%llu\n", w, r, back, (unsigned long long)avail, (unsigned long long)total);

    // ---- C++ interfaces (MSVC vtables) ----
    IFace *u21 = SteamUser021();
    printf("c++: SteamUser021 %s, init_calls=%d\n", u21 ? "ok" : "NULL", init_calls);
    SteamUser021();
    printf("c++: cached, init_calls=%d\n", init_calls);
    uint64_t sid = 0, *ret = VCALL(u21, 2, uint64_t *(*)(IFace *, uint64_t *), &sid);   // CSteamID GetSteamID(): hidden pointer
    printf("c++: GetSteamID=%llu (returned the hidden pointer: %d) BLoggedOn=%d\n", (unsigned long long)sid, ret == &sid,
           VCALL(u21, 1, bool8 (*)(IFace *)));

    IFace *st = SteamInternal_FindOrCreateUserInterface(SteamAPI_GetHSteamUser(), "STEAMUSERSTATS_INTERFACE_VERSION011");
    // MSVC slots: 0 RequestCurrentStats, 1 GetStat(float*), 2 GetStat(int32*), 3 SetStat(float), 4 SetStat(int32), 5 UpdateAvgRateStat
    float fv = 0; int32_t iv = 0;
    VCALL(st, 4, bool8 (*)(IFace *, const char *, int32_t), "kills", 42);
    VCALL(st, 1, bool8 (*)(IFace *, const char *, float *), "dist", &fv);
    VCALL(st, 2, bool8 (*)(IFace *, const char *, int32_t *), "kills", &iv);
    printf("c++: overloads: GetStat(float)=%.2f GetStat(int32)=%d\n", fv, iv);
    printf("c++: UpdateAvgRateStat(30.0f, 60.0)=%d\n", VCALL(st, 5, bool8 (*)(IFace *, const char *, float, double), "rate", 30.0f, 60.0));
    printf("c++: RequestCurrentStats=%d\n", VCALL(st, 0, bool8 (*)(IFace *)));

    // call result through CCallResult-style registration
    CB req = { &cb_vtbl, 0, 1101, "RequestUserStats", sizeof(UserStatsReceived_t) };
    SteamAPICall_t call = VCALL(st, 16, SteamAPICall_t (*)(IFace *, uint64_t), 76561197960287999ull);
    SteamAPI_RegisterCallResult(&req, call);
    SteamAPICall_t find = VCALL(st, 23, SteamAPICall_t (*)(IFace *, const char *), "Best Times");
    printf("c++: RequestUserStats call=0x%llx FindLeaderboard call=0x%llx\n", (unsigned long long)call, (unsigned long long)find);

    // ISteamClient: interfaces by version string, and a host->guest function pointer
    IFace *client = SteamInternal_CreateInterface("SteamClient017");
    int hu = SteamAPI_GetHSteamUser(), hp = SteamAPI_GetHSteamPipe();
    IFace *fr15 = VCALL(client, 8, IFace *(*)(IFace *, int, int, const char *), hu, hp, "SteamFriends015");
    IFace *ut9 = VCALL(client, 9, IFace *(*)(IFace *, int, const char *), hp, "SteamUtils009");
    printf("c++: client GetISteamFriends -> %s, GetISteamUtils -> appid %u\n", VCALL(fr15, 0, const char *(*)(IFace *)),
           VCALL(ut9, 9, uint32_t (*)(IFace *)));
    uint64_t fsid = 0; VCALL(fr15, 4, uint64_t *(*)(IFace *, uint64_t *, int, int), &fsid, 1, 4);
    printf("c++: GetFriendByIndex(1)=%llu\n", (unsigned long long)fsid);
    VCALL(client, 21, void (*)(IFace *, void (*)(int, const char *)), warn_hook);
    VCALL(ut9, 16, void (*)(IFace *, void (*)(int, const char *)), warn_hook);           // SteamUtils009::SetWarningMessageHook

    printf("-- RunCallbacks\n");
    SteamAPI_RunCallbacks();
    printf("-- RunCallbacks again\n");
    SteamAPI_RunCallbacks();

    // ISteamUtils::GetAPICallResult with the Windows-layout buffer
    bool8 failed = 1, completed = VCALL(ut9, 11, bool8 (*)(IFace *, SteamAPICall_t, bool8 *), find, &failed);
    LeaderboardFindResult_t lf; memset(&lf, 0xCC, sizeof lf);
    bool8 got = VCALL(ut9, 13, bool8 (*)(IFace *, SteamAPICall_t, void *, int, int, bool8 *), find, &lf, (int)sizeof lf, 1104, &failed);
    printf("c++: FindLeaderboard completed=%d got=%d failed=%d handle=0x%llx found=%d\n", completed, got, failed,
           (unsigned long long)lf.m_hSteamLeaderboard, lf.m_bLeaderboardFound);

    // struct out-parameter repacked (LeaderboardEntry_t is 28 bytes on macOS, 32 on Windows); 6 args: 2 on the stack
    LeaderboardEntry_t e; int32_t details[4] = {0}; memset(&e, 0xCC, sizeof e);
    bool8 eok = VCALL(st, 30, bool8 (*)(IFace *, uint64_t, int, LeaderboardEntry_t *, int32_t *, int), 0xABCDEF, 3, &e, details, 4);
    printf("c++: entry ok=%d user=%llu rank=%d score=%d details=%d [%d %d] ugc=0x%llx\n", eok,
           (unsigned long long)e.id_lo | (unsigned long long)e.id_hi << 32, e.rank, e.score, e.details, details[0], details[1],
           (unsigned long long)e.ugc);

    IFace *legacy = SteamUtils();
    // no steam_api64.dll next to the test, so the bridge picks the newest known version: SteamUtils011, GetAppID is slot 8
    printf("legacy SteamUtils(): appid=%u\n", legacy ? VCALL(legacy, 8, uint32_t (*)(IFace *)) : 0);

    SteamAPI_UnregisterCallback(&persona);
    printf("unregistered: flags=%d\n", persona.m_nCallbackFlags);
    SteamAPI_Shutdown();
    IFace *after = SteamUser021();
    printf("after shutdown: user=%p init_calls=%d\n", (void *)after, init_calls);
    return 0;
}
