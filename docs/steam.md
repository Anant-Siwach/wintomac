# Steamworks (steam_api64.dll) on macOS

Games that use the Steamworks API import `steam_api64.dll`. wintomac replaces that DLL with
`runtime/steam.c`, a bridge to Valve's **macOS** Steamworks runtime, `libsteam_api.dylib`. That library in turn
talks to the Steam client the user is running. This follows the idea of Proton's `lsteamclient` (Windows Steam
calls are bridged to the native client) but at a different layer; see "Design" below.

## Ground rules: no DRM circumvention

* There is no Steam emulator here. Ownership, subscriptions, DLC, licences, login, achievements and stats all come
  from the real Steam client through Valve's own `libsteam_api.dylib`. wintomac never answers those questions.
* If Steam isn't running, the user isn't logged in, or the account doesn't own the app, `SteamAPI_Init` fails
  inside Valve's library and the game sees exactly that.
* If no `libsteam_api.dylib` can be found, the bridge logs once and behaves like `steam_api` with no Steam
  available: `SteamAPI_Init`/`InitSafe` return false, `SteamAPI_InitFlat`/`SteamAPI_InitEx` return
  `k_ESteamAPIInitResult_NoSteamClient` with a message, `SteamAPI_IsSteamRunning` returns false, and every interface
  accessor returns NULL.
* SteamStub, CEG and other DRM wrappers are not touched. A SteamStub-wrapped exe is out of scope.

## How a native Mac game talks to Steam

1. The game ships `libsteam_api.dylib`, the Steamworks "redistributable" (usually in
   `Game.app/Contents/Frameworks` or `Contents/MacOS`). Recent SDKs ship a universal (x86_64 + arm64) build.
2. Roughly (this is Valve's closed code; the outline matches the SDK documentation and Proton's design):
   `SteamAPI_Init()` reads the app id (from the environment when Steam launched the game, otherwise from
   `steam_appid.txt` in the working directory). It then finds the running Steam client and loads that client's
   `steamclient.dylib` (inside `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/`). It
   calls `CreateInterface("SteamClientNNN")`, opens a pipe with `CreateSteamPipe`/`ConnectToGlobalUser`, and
   asks the client to check ownership.
3. Interfaces (`ISteamUser`, `ISteamUserStats`, ...) are C++ objects owned by steamclient. Each one is
   requested by an exact version string (`"SteamUser023"`), so one client serves every SDK version.
4. Callbacks are pulled from the client by `SteamAPI_RunCallbacks` (or manual dispatch) and delivered to the
   game's `CCallback`/`CCallResult` objects by calling their virtual `Run()`.

## What Proton's lsteamclient does

Under Proton, the game's own Windows `steam_api64.dll` runs unchanged inside Wine and loads
`steamclient64.dll`. Proton replaces that DLL with `lsteamclient`, a Wine DLL that loads the native
`steamclient.so` (or `steamclient.dylib` for CrossOver on macOS) and exports `CreateInterface`,
`Steam_BGetCallback` and the rest. For every interface version it ships generated wrapper classes
(`cppISteamUser_SteamUser023.cpp`, `winISteamUser.c`, made by `gen_wrapper.py` with libclang from the vendored
per-SDK headers). Those wrappers turn MSVC `thiscall`/x64 vtable calls into native calls. They also convert
every struct whose layout differs: callback structs are packed to 8 on Windows and to 4 elsewhere, which is
`steamclient_structs_generated.h`. Host-to-guest function pointers (warning hooks, networking debug output,
matchmaking response objects) are queued and run on the Windows side. Hand-written code covers what can't be
generated: matchmaking server-list responses, networking messages, and Steam Input.

## Design: bridge at the steam_api level

wintomac shims `steam_api64.dll` itself and forwards to a host `libsteam_api.dylib`:

```
game.exe (recompiled) --Win64 ABI--> runtime/steam.c --arm64 ABI--> libsteam_api.dylib --IPC--> Steam client
```

Why `steam_api` and not `steamclient` as in Proton:

* **There is no Wine underneath.** Running the game's `steam_api64.dll` would mean recompiling it and emulating
  what it expects from Windows (the Steam registry keys, `ActiveProcess` pid checks, process handles,
  `steamclient64.dll` lookup). That is a lot of fragile Win32 behaviour for no gain.
* **It is Valve's supported path.** The macOS `libsteam_api.dylib` is the official runtime for exactly this job:
  a game process talking to the Steam client. Every ownership or licence decision stays in Valve's code, and
  we talk to Steam only "through the API provided by the SDK Redistributables". `steamclient`'s exports
  (`Steam_BGetCallback`, `Steam_GetAPICallResult` and friends) are internal.
* **Version strings pass straight through.** When the game asks for `"SteamUser019"`, the host object we get
  back *is* a SteamUser019, so slot `i` of the guest's vtable maps to one known slot of the host's vtable. We
  need ABI facts only, never behaviour.
* **Cost:** the user has to supply a `libsteam_api.dylib`. It is redistributable: any Mac Steamworks game they own
  has one (see the test plan below). It must contain an arm64 slice.

### How the bridge works (`runtime/steam.c`)

* **Plain exports** (`SteamAPI_Init`, `InitSafe`, `InitFlat`, `SteamInternal_SteamAPI_Init` (= `InitEx`),
  `Shutdown`, `RestartAppIfNecessary`, `IsSteamRunning`, `RunCallbacks`, `GetHSteamUser/Pipe`,
  `ReleaseCurrentThreadMemory`, the game-server entry points, `SteamInternal_GameServer_Init(_V2)`, ...) are
  forwarded. Windows-only ones (`WriteMiniDump`, `UseBreakpadCrashHandler`, `SetMiniDumpComment`) are no-ops.
* **Interface accessors.** `SteamInternal_FindOrCreateUserInterface`, `...GameServerInterface` and
  `SteamInternal_CreateInterface` call the host with the guest's version string. `SteamInternal_ContextInit` (the
  cached accessors behind `SteamUser()` and friends in SDK 1.4x+) is implemented here. It re-runs the guest's
  init function whenever Init or Shutdown bumps the counter. Pre-1.4x SDKs exported `SteamUser()`,
  `SteamFriends()`, ...; the interface version those returned was compiled into the game's own
  `steam_api64.dll`, so the bridge looks for the version strings in that file (next to the exe) and falls back
  to the newest version it knows, with a log line.
* **Interface objects.** The guest gets a wrapper `{vtbl, host object, interface description}`. Its vtable is
  thunks `__wintomac_steam_slot_N`. Each call is translated using `runtime/gen/steam_ifaces.h`:
  * the MSVC slot maps to the Itanium slot (MSVC puts overloads such as `GetStat(int32*)` and `GetStat(float*)`
    in reverse order);
  * Win64 positional arguments (RCX, RDX, R8, R9, XMM0-3, then the stack) map to arm64 (x0-x7 and v0-v7 counted
    separately, then Apple's packed stack slots);
  * `bool` and small ints are zero- or sign-extended;
  * methods returning a class (`CSteamID GetSteamID()`) take MSVC's hidden return pointer after `this`;
  * pointers to structs whose layout differs between the targets (`LeaderboardEntry_t`, `SteamUGCDetails_t`, ...)
    are repacked in and out;
  * `ISteamClient::GetISteam*(…, version)` results are wrapped again with that version;
  * `SteamAPIWarningMessageHook_t` arguments get a host trampoline that calls back into the guest.
  * Unknown interface versions fall back to 1:1 slots with integer arguments and a log line.
* **Flat API.** `SteamAPI_ISteamX_Method(self, ...)` (the API used by SDK 1.48+, Unity and Unreal plugins,
  Steamworks.NET) is dispatched on the wrapper's own interface version, matched by method name and signature.
  So it works even when the host dylib comes from a different SDK. Flat accessors (`SteamAPI_SteamUser_v023()`)
  map to the version string. Flat functions that aren't virtual methods (helpers such as
  `SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32` or `SteamAPI_SteamNetworkingIdentity_ToString`) are
  forwarded to the host symbol of the same name, with interface pointers unwrapped.
* **Callbacks and call results.** `SteamAPI_RegisterCallback`/`RegisterCallResult` register a host proxy.
  The proxy is laid out like an Itanium `CCallbackBase`: `{vptr, uint8 m_nCallbackFlags, int m_iCallback}`.
  `libsteam_api` calls the proxy's `Run()`. The proxy converts the payload to the Windows layout and calls the
  guest object through its MSVC vtable: slot 0 `Run(p, bIOFailure, hSteamAPICall)`, slot 1 `Run(p)`, slot 2
  `GetCallbackSizeBytes()`. The registered flag and id are mirrored into the guest object, as `steam_api` does.
  The callback's struct version is picked by the guest's `GetCallbackSizeBytes()`, and the proxy reports the
  matching macOS size to the host. `ISteamUtils::GetAPICallResult`, the manual-dispatch API
  (`SteamAPI_ManualDispatch_*`) and `SteamAPICallCompleted_t::m_cubParam` are converted the same way.

### The ABI tables (`tools/gen_steam.py` → `runtime/gen/steam_ifaces.h`)

`make steam-gen` (after `tools/fetch_proton_steam.sh`) reads the per-SDK interface headers that Proton vendors in
`lsteamclient/steamworks_sdk_*` (SDK 1.16 to 1.65, 66 header sets) and has clang measure everything for both
targets: `x86_64-pc-windows-msvc` (guest) and `arm64-apple-macos` (host). Stub libc headers in
`tools/steam_stub_include` stand in for both platform SDKs.

* vtable slot order: `-fdump-vtable-layouts` on a derived class with an out-of-line constructor;
* struct layouts: `-fdump-record-layouts` for every callback struct and every struct passed by pointer;
* argument classes and sizes, and `k_iCallback` values: constexpr probes compiled to assembly.

The result: **149 interface versions** (5303 methods), **214 callback ids** with 123 distinct struct layouts,
**1022 flat methods**, **87 flat accessors** and **187 forwarded flat helpers**. The header holds only ABI facts:
version strings, method names, slot numbers, argument classes, offsets and callback ids. It is checked in, so a
normal build doesn't need Proton's files.

Interfaces covered include every version in those SDKs. For the core ones:

| interface | versions |
|---|---|
| ISteamClient | SteamClient011–021, 023 |
| ISteamUser | SteamUser016–023 |
| ISteamUserStats | STEAMUSERSTATS_INTERFACE_VERSION010–013 |
| ISteamApps | STEAMAPPS_INTERFACE_VERSION004–009 |
| ISteamFriends | SteamFriends009–015, 017, 018 |
| ISteamUtils | SteamUtils005–011 |
| ISteamRemoteStorage | STEAMREMOTESTORAGE_INTERFACE_VERSION004–014, 016 |

The tables also cover Matchmaking, Networking, NetworkingSockets/Messages/Utils, UGC, HTTP, Inventory,
Controller/Input, Screenshots, HTMLSurface, GameServer(Stats), Music, Video, ParentalSettings, RemotePlay,
Parties, Timeline and AppList.

### Licence of the headers

Proton's own code is BSD-3-Clause (© Valve). The `lsteamclient/` directory, which contains the vendored
`steamworks_sdk_*` headers, carries the **Steamworks SDK licence** (`lsteamclient/LICENSE`). That licence grants
use "solely to develop the Licensee Software", says it takes effect when the SDK is downloaded, and restricts
redistribution to the `redistributable_bin` part. wintomac therefore:

* never downloads the SDK from Valve and never accepts its licence on the user's behalf;
* fetches Proton's public repository only into `third_party/proton-lsteamclient/` (local, not redistributed)
  and reads the headers as data with `python3 -I` and `clang -fsyntax-only`;
* compiles nothing from them into wintomac and copies no header text. The generated table contains only
  interoperability facts: names, slot numbers, sizes and offsets.
* does not ship Valve binaries: `libsteam_api.dylib` comes from the user's own games.

If that reading is too generous for your use, delete `third_party/proton-lsteamclient` and keep the checked-in
`runtime/gen/steam_ifaces.h`, or regenerate the table from a Steamworks SDK you have licensed yourself (point
`tools/gen_steam.py` at a directory containing `steamworks_sdk_<ver>/` folders of its headers).

## Tests (headless)

`tests/src/steam_basic.c` (mingw, imports `steam_api64.dll` through `tools/steam_api64.def`) runs against
`tests/mock/mock_steam_api.c`. The mock is a host-native `libsteam_api.dylib` with canned values. It builds
real Itanium vtables for each requested version from the generated table, keeps a callback queue, and checks
callback sizes the way Steam does. `make check` passes `WINTOMAC_STEAM_API=build/libmock_steam_api.dylib`.
The test covers:

* `RestartAppIfNecessary`, `Init`, `IsSteamRunning`, `Shutdown`;
* the flat API on User/Friends/Apps/Utils/UserStats/RemoteStorage;
* `SteamInternal_ContextInit` caching and re-init after Init and Shutdown;
* MSVC overload order (`GetStat`/`SetStat` int vs float);
* a hidden-pointer `CSteamID` return;
* float and double arguments (`UpdateAvgRateStat`);
* `ISteamClient::GetISteamFriends/GetISteamUtils` by version string;
* warning hooks called back into the guest;
* callbacks registered before Init (`PersonaStateChange_t`: 16 bytes on Windows, 12 on macOS);
* a call result (`UserStatsReceived_t`: 24 vs 20 bytes);
* `GetAPICallResult` (`LeaderboardFindResult_t`: 16 vs 12 bytes);
* a repacked out-struct with stack arguments (`GetDownloadedLeaderboardEntry`, `LeaderboardEntry_t` 32 vs 28);
* a pre-1.4x exported accessor (`SteamUtils()`).

Without a library (`WINTOMAC_STEAM_API=/nonexistent`), the same test prints `init=0 running=0` and gets NULL
interfaces.

## Testing with a real Steam game (for the user)

wintomac's maintainers can't do this: it needs your Steam account. Nothing here logs in or launches Steam for you.

**You need:**

1. Steam for Mac, running and logged in with an account that **owns** the game.
2. The game's **Windows** files. Steam for Mac only installs Mac depots, so fetch them one of these ways:
   * from a Windows PC or a CrossOver/Whisky bottle where it is installed; or
   * with Steam's console (`steam://open/console`): `download_depot <appid> <windows depot id>`. The files land
     under `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/`.
3. A `libsteam_api.dylib` **with an arm64 slice**, from any Mac Steamworks game you have installed:
   ```sh
   find ~/Library/Application\ Support/Steam/steamapps/common -name libsteam_api.dylib -exec lipo -archs {} \; -print
   ```
   Pick one that lists `arm64`; newer is better. x86_64-only copies (older SDKs) can't be loaded by the arm64
   game.

**Build and run** (from the wintomac checkout; `Game` stands for your exe's name):

```sh
make recomp EXE=/path/to/Game/Game.exe          # recompile only the exe (and its own DLLs), NOT steam_api64.dll
cp /path/to/libsteam_api.dylib build/            # or: export WINTOMAC_STEAM_API=/path/to/libsteam_api.dylib
cd /path/to/Game                                  # keep steam_api64.dll here: its interface versions are read from it
echo <appid> > steam_appid.txt                    # Valve's developer mechanism; Steam still checks ownership
WINTOMAC_STEAM_TRACE=1 /path/to/wintomac/build/Game.bin Game.exe 2> steam.log
```

The bridge finds `libsteam_api.dylib` via `WINTOMAC_STEAM_API`, next to `Game.exe`, next to `Game.bin`, or in
`../Frameworks` of either. `steam_appid.txt` (or `export SteamAppId=<appid>`) is needed because the game isn't
launched by Steam. Without it, `SteamAPI_RestartAppIfNecessary` asks Steam to relaunch the game, which Steam for
Mac can't do for a Windows title, so the game exits; the log says so.

**What success looks like:**

* no `[rt] steam: SteamAPI_Init failed` line; with tracing, `steam.log` shows interface versions being wrapped
  (`steam: SteamUser021 -> host ..., guest ...`) and calls returning sensible values;
* your Steam friends list shows you **playing the game**;
* achievements unlock with Steam's notification, and stats and cloud saves sync (check the game's page in
  Steam afterwards);
* the game shows your Steam name and avatar, and DLC you own is detected.

**If it fails:**

* `SteamAPI_Init failed`: check that Steam is running and logged in, that the appid in `steam_appid.txt` is
  right, and that this account owns the game.
* `no libsteam_api.dylib`: check the path, or that `lipo -archs` includes arm64.
* `interface X is not in the generated table` or `guest callback objects ... not bridged`: please report the
  log.

Optional smoke test without a game: `tests/src/steam_basic.c` can run against the real library with Valve's
public test app Spacewar (appid 480), which every account can use. Its output won't match the mock's expected
file, and it calls stats/achievement setters (names that Spacewar doesn't have) and writes a 12-byte
`save.dat` to Spacewar's Steam Cloud space:
`cd build && echo 480 > steam_appid.txt && WINTOMAC_STEAM_API=/path/to/libsteam_api.dylib ./steam_basic.bin steam_basic.exe`.

## Gaps

* **Guest callback interfaces** passed *into* Steam are not bridged: `ISteamMatchmakingServerListResponse`,
  `...PingResponse`, `...PlayersResponse`, `...RulesResponse`. Calls that take one are skipped and return 0
  (logged). Server browsers won't work.
* **Function-pointer arguments** other than the warning hook are passed as NULL (logged). Examples:
  `SteamNetworkingUtils` global callbacks, `FnSteamNetConnectionStatusChanged` in config values, and
  `SetGlobalCallback_*` helpers.
* **Arrays of repacked structs** (pointer plus count) are converted for the first element only. Most array
  parameters (`SteamItemDetails_t`, `SteamUGCDetails_t` via index) have identical layouts or single elements.
  `SteamPartyBeaconLocation_t` passed **by value** (ISteamParties) is copied, not repacked.
* Structs nested in callback payloads that themselves contain pointers to repacked structs aren't followed.
  `SteamNetworkingMessage_t` objects are host objects passed through as pointers. Their `m_pfnRelease` and
  `m_pfnFreeData` are host functions, so the SDK's inline `SteamNetworkingMessage_t::Release()`, which calls
  `m_pfnRelease` straight from game code, will fault. `SteamAPI_SteamNetworkingMessage_t_Release` (flat) is
  forwarded and works.
* MSVC is assumed for the game's vtable layout. A game whose C++ is built with MinGW (Itanium order on Windows)
  would get the overload order wrong for the few overloaded methods. The flat API is unaffected.
* Steam Overlay, Steam Input's controller configuration UI and Big Picture integration don't apply to a process
  Steam didn't launch.
* The manual-dispatch path and the game-server entry points are implemented but not covered by the mock test.
* Not yet tried against a live Steam client, because that needs the user's account (see the test plan above).
