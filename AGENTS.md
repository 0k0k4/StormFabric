# AGENTS.md — creating a StormFabric mod

Guide for an agent (or a human) who has to **create a mod** for Stormworks with
StormFabric. It says what to produce, in which order, and which rules must never be broken.
The templates below compile as they are.

To change the **loader** itself (not a mod), read `README.md` and `docs/` first.

---

## 1. Hard rules

1. **Never put a game address in a mod.** No `gameBase + 0x…`, no hardcoded structure
   offset. Everything goes through `host->api` (the SDK). Addresses live in the loader,
   which validates them before every use.
2. **Check before use.** Every SDK table has a `structSize`; recently appended members must
   be tested (`structSize >= offsetof(...) + sizeof(...)`) and the matching capability
   (`runtime->capabilities()`) checked. When something is missing, the mod disables itself
   cleanly (`return false`) or does without that feature.
3. **Never modify or redistribute game files.** A content mod ships its own files in
   `content/`. To start from a game file, write a script that derives it from the user's
   own copy (see `examples/content_demo/make_content.py`).
4. **Respect threads.** The tick and game events run on the **simulation thread**; native
   UI requires the **UI thread**. To hand work over: `runtime->runOnGameThread`.
5. **Keep nothing from Lua.** A `void* luaState` is only valid during the callback that
   provides it. Do not store it and do not use it from another thread.
6. **The game must be closed** to replace a mod DLL or the loader.
7. **Never name a mod DLL like a Windows DLL.** Windows binds a DLL request by base name to a
   module already loaded in the process, so a mod DLL called `websocket.dll`, `winhttp.dll`,
   `version.dll`… replaces the system DLL for the game and everything in it. Prefix the name
   (`sf_websocket.dll`) and check that `C:\Windows\System32\<name>` does not exist.

## 2. Which kind of mod?

| Goal | Kind | Section |
|---|---|---|
| Add/replace components, meshes, audio, data | Content only (no C++) | §4 |
| Act on the world every tick (`server.*`) | DLL + tick | §6 |
| React to an event (chat command, player, vehicle…) | DLL + game events | §6 |
| Give new functions to addon scripts (`script.lua`) | DLL + addon library | §7 |
| Give new functions to microcontrollers | DLL + microcontroller library | §8 |
| Window/HUD, keyboard shortcut, storage | DLL + `nativeUi` / `input` / `storage` | §9 |
| Draw your own pixels over the game (web view, video) | DLL + `image` | §9 |
| Offer functions to other mods / use another mod's functions | DLL + `services` | §9 |

One mod can combine a DLL **and** content.

**Availability**: addon libraries, Lua calls (`extras->call`), the tick and game events only
exist on game build **1.15.23** (`SW_CAP_LUA_ADDON`, `SW_CAP_SIMULATION_TICK`,
`SW_CAP_GAME_EVENTS`). Everything else works on both reviewed builds.

## 3. Mod layout

```text
<Stormworks>/mods/my_mod/
├── mod.json          required
├── my_mod.dll        if the mod has code
└── content/          if the mod has content (same tree as rom/)
```

```json
{
  "id": "my_mod",
  "name": "My Mod",
  "author": "author",
  "version": "1.0.0",
  "description": "Shown in the MODS menu.",
  "main": "my_mod.dll",
  "content": "content",
  "dependencies": [],
  "native_deps": [],
  "enabled_by_default": true
}
```

- `id`: lowercase, unique. `main`: omitted for a content-only mod; never the name of a
  System32 DLL (rule 7).
- `content`: folder overlaid on `rom/` (defaults to `content`, used when it exists).
- `dependencies`: `id`s of other mods loaded before this one. `native_deps`: local DLLs.
- The enabled/disabled state is stored in `mods/_modloader.ini` (`id=1` or `id=0`).

## 4. Content mod (no code)

Put the files in `content/`, reproducing the path they would have under `rom/`:

```text
mods/my_mod/content/data/definitions/my_component.xml   → rom/data/definitions/my_component.xml
mods/my_mod/content/meshes/my_mesh.mesh                 → rom/meshes/my_mesh.mesh
```

- New file name = **added**. Same name as a game file = **replaced**.
- Applied **at game startup** only (the MODS menu shows `RESTART` otherwise).
- Conflict between two mods: the first in alphabetical order wins (written to the log).
- A component definition is made by copying a similar component **from the user's copy**
  of the game and changing `name="…"` (and anything else needed).

Details: `docs/content_mods.md`.

## 5. Native mod skeleton

Always start from this skeleton. `SwModInit` is called on load (main menu, outside the
loader lock); `SwModUnload` right before unloading, after the loader has already revoked
the mod's callbacks.

```cpp
// file: templates/minimal.cpp
#include "sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION) return false;
    g_host = host;
    host->log(host->modId, "loaded");   // -> StormworksModLoader.log
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
```

## 6. Acting on the world: tick, game events, `server.*`

Complete template: a message every minute, a greeting whenever a player joins, and a
`?hello` command handled in C++. This is the base of any gameplay mod.

```cpp
// file: templates/gameplay.cpp
#include "sw_mod_sdk.h"
#include <stdio.h>
#include <string.h>

static const SwModHost* g_host = nullptr;
static uint64_t g_ticks = 0;

// server.announce(title, text) via the game's own Lua API. Only valid inside a tick or
// event callback, with the luaState that callback received.
static bool Announce(void* L, const char* title, const char* text) {
    const SwLuaApi* lua = g_host->api->lua;
    const SwLuaExtrasApi* x = lua->extras;
    char error[256];
    if (!L || !lua->checkStack(L, 6)) return false;
    if (x->getGlobal(g_host, L, "server") != SW_LUA_OK) return false;     // pushes server
    if (x->getField(g_host, L, -1, "announce") != SW_LUA_OK) return false; // pushes server.announce
    x->pushString(g_host, L, title, strlen(title));
    x->pushString(g_host, L, text, strlen(text));
    if (x->call(g_host, L, 2, 0, error, sizeof(error)) != SW_LUA_OK) {    // consumes fn + 2 args
        g_host->log(g_host->modId, error);
        return false;
    }
    return true; // the loader drops `server` (and anything left) after the callback
}

static void OnTick(const SwSimulationTick* tick, void*) {
    if (++g_ticks % (60 * 60) == 0)   // ~60 ticks per second
        Announce(tick->luaState, "My Mod", "one more minute");
}

static void OnEvent(const SwGameEvent* event, void*) {
    const SwGameValue* a = event->arguments;
    char text[160];
    // onPlayerJoin(steam_id, name, peer_id, is_admin, is_auth)
    if (!strcmp(event->name, "onPlayerJoin") && event->argumentCount >= 2 && a[1].type == SW_GAME_VALUE_STRING) {
        snprintf(text, sizeof(text), "welcome %s", a[1].string);
        Announce(event->luaState, "My Mod", text);
    }
    // onCustomCommand(full_message, peer_id, is_admin, is_auth, command, ...)
    if (!strcmp(event->name, "onCustomCommand") && event->argumentCount >= 5 &&
        a[4].type == SW_GAME_VALUE_STRING && !strcmp(a[4].string, "?hello")) {
        Announce(event->luaState, "My Mod", "hello from C++");
    }
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->lua || !host->api->runtime) return false;
    const SwLuaApi* lua = host->api->lua;
    const SwRuntimeApi* runtime = host->api->runtime;
    // Appended members: check sizes, then capabilities.
    if (lua->structSize < offsetof(SwLuaApi, extras) + sizeof(lua->extras) || !lua->extras ||
        lua->extras->structSize < offsetof(SwLuaExtrasApi, call) + sizeof(lua->extras->call) ||
        runtime->structSize < offsetof(SwRuntimeApi, unsubscribeGameEvent) + sizeof(runtime->unsubscribeGameEvent)) return false;
    const uint64_t caps = runtime->capabilities();
    if (!(caps & SW_CAP_SIMULATION_TICK) || !(caps & SW_CAP_GAME_EVENTS)) {
        host->log(host->modId, "tick/events unavailable on this game build");
        return false;
    }
    g_host = host;
    SwHandle tick = 0, events = 0;
    if (runtime->subscribeSimulationTick(host, &OnTick, nullptr, &tick) != SW_OK) return false;
    // NULL = every event; or one name, e.g. "onPlayerJoin". onTick is never an event.
    if (runtime->subscribeGameEvent(host, nullptr, &OnEvent, nullptr, &events) != SW_OK) return false;
    return true; // subscriptions are revoked automatically when the mod unloads
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
```

Tick and event rules:
- **Simulation thread**, about 60 ticks per second; only while a world runs (single player
  or host), never in the main menu.
- `tick->luaState` can be `NULL` (no live addon VM): test it before calling.
- An event is delivered **once**, before the first addon runs its own callback. Event names
  and arguments are those of the addon Lua callbacks (`onVehicleSpawn`, `onPlayerSit`,
  `onChatMessage`, `onToggleMap`…); see the game's Lua help in the script editor or
  `stormworks-modding-kit/lua-api/api-reference.md`.
- String arguments are copies, valid during the callback only.
- Lua stack handling: always `checkStack` before pushing; `call` consumes the function and
  its arguments; a Lua error comes back as `SW_LUA_CALL_FAILED` (never an exception). Never
  call `raiseError` (unsupported).
- Reading through `server.*`: `call(..., nargs, 1, ...)`, then read the result and `pop`.
  A string: `extras->copyString(..., -1, ...)`. A number or boolean:
  `argNumber(L, getTop(L), 0)` / `argBool(L, getTop(L), false)`. Example:
  `examples/addon_lua_demo` (`HostName`).
- **Index sign**: `argNumber`, `argBool` and `argString` accept **positive indices only**; a
  negative index silently returns the default. The `extras` functions (`getType`,
  `copyString`, `getField`, `setField`, `pop`…) accept both. To read the top of the stack
  with an `arg*` function, use `getTop(L)`.
- **Passing a stack value to `call`**: `call` wants the function *below* its arguments, so a
  value already on the stack (an argument received by a library function) must be copied
  on top with `extras->pushValue(host, L, index)` (appended member: check `structSize`).
  Example: `getGlobal("next")`, `pushValue(1)`, `pushNil` → `call(2, 2)` iterates a table
  passed by the script. There is no `lua_remove`: values pushed stay until you `pop` them.
- **Objects tied to a script**: `extras->vmId(host, L, &id)` (appended member: check
  `structSize`) gives the id of the VM the callback runs in, equal to the `handle` of
  `SW_EVENT_LUA_DESTROYED` / `SW_EVENT_ADDON_LUA_DESTROYED` (`runtime->subscribe`). Store it
  with whatever the script creates and free those objects when that VM is destroyed
  (vehicle despawned, script reloaded).

## 7. Library for addon scripts

The mod provides a global table to `script.lua` (next to `server.*`).

```cpp
// file: templates/addon_library.cpp
#include "sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

// my_mod.double(x) -> 2 * x
static int Double(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    const double x = lua->argNumber(L, 1, 0);
    if (!lua->checkStack(L, 1)) return 0;
    lua->pushNumber(L, 2 * x);
    return 1; // number of results
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->lua || !host->api->runtime) return false;
    const SwLuaApi* lua = host->api->lua;
    if (lua->structSize < offsetof(SwLuaApi, registerAddonLibrary) + sizeof(lua->registerAddonLibrary) ||
        !(host->api->runtime->capabilities() & SW_CAP_LUA_ADDON)) return false;
    g_host = host;
    SwLuaFunction functions[] = {{"double", &Double, nullptr}};
    return lua->registerAddonLibrary(host, "my_mod", functions, 1);
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
```

In the addon, always test the table so the addon keeps working without the mod:

```lua
function onCreate(is_world_create)
    if my_mod then server.announce("my_mod", tostring(my_mod.double(21))) end
end
```

- Installed in **newly created** addon VMs (on world load or script reload).
- Refused names: game tables (`server`, `matrix`, `property`, `g_savedata`…), standard
  libraries, base functions, and any `onXxx` name.
- A callback returns the **number of results** it pushed (0 on error).

## 8. Library for microcontrollers

Same idea with `registerLibrary` (works on both builds). Reserved names: `input`, `output`,
`screen`, `math`, `debug`, `qol`, `_G`, `string`, `table`.

```cpp
// file: templates/mc_library.cpp
#include "sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

// my_mc.sum(a, b, ...) -> sum of the arguments
static int Sum(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    double sum = 0;
    for (int i = 1; i <= lua->getTop(L); ++i) sum += lua->argNumber(L, i, 0);
    if (!lua->checkStack(L, 1)) return 0;
    lua->pushNumber(L, sum);
    return 1;
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->lua) return false;
    g_host = host;
    SwLuaFunction functions[] = {{"sum", &Sum, nullptr}};
    return host->api->lua->registerLibrary(host, "my_mc", functions, 1);
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
```

## 9. Other services

- **Storage** (`host->api->storage`, any thread): `readConfig` / `writeConfig` by key
  (`[A-Za-z0-9._-]`, at most 64 characters and 4 MiB, atomic writes). No storage tied to a
  world save (`SW_STORAGE_UNAVAILABLE`).
- **Keyboard shortcuts** (`host->api->input`): `registerAction`, then `bindKey(vk, modifiers)`;
  presses arrive as `SW_EVENT_ACTION` events (`runtime->subscribe`).
- **HUD / windows** (`host->api->nativeUi`, UI thread only): see `examples/p0_demo`.
- **Pixel images** (`host->api->image`, **any thread**, `SW_CAP_NATIVE_IMAGE`; check
  `api->structSize` covers `image`): `create` a BGRA/RGBA image for a surface, `update` it
  with pixels (whole or a region), place it with `setRect` in window pixels. Drawn over the
  whole frame before present; the base for web views or video. `setInputCallback` makes it
  interactive (mouse over it and keys while it has focus go to the mod instead of the game;
  callback on the window thread, keep it short). See `examples/image_demo` and
  `docs/native_image.md`.
- **Services between mods** (`host->api->services`; check `api->structSize` covers
  `services`): a provider `publish`es a `static const` function table under a name and a
  version (`SW_SERVICE_VERSION(major, minor)`); a consumer `get`s it, and the loader only
  allows that when the provider is in the consumer's mod.json `dependencies` (so the table
  outlives the consumer). Ship the table's struct in a header; first member `structSize`,
  append only. A provider that keeps per-consumer objects or callbacks must register
  `setOwnerReleaseCallback` and drop them there. See `examples/service_demo` and
  `docs/services.md`.
- **Vehicle monitors**: `video->setSourceImage(host, L, image)` (appended member: check
  `structSize`), called in a monitor's `onDraw`, shows one of your `api->image` images on
  that monitor instead of its video signal for this frame; `screen.*` drawings stay on top.
  The monitor's touch output reaches the microcontroller as usual. See `docs/video_fx.md`.
- **Tasks** (`runtime->runOnGameThread`, `runtime->schedule`): run on the UI thread, can be
  requested from the simulation thread.
- **Low level** (`host->api->native`: inline hooks, memory): last resort, only with
  validated signatures (`findSignature`); never a hardcoded address.

References: `include/sw_mod_sdk.h`, `include/sw_sdk_p0.h`, `include/sw_sdk_lua.h`,
`include/sw_sdk_storage.h`, `include/sw_sdk_image.h`, and `docs/addon_lua.md`,
`docs/native_api.md`, `docs/p0_sdk.md`, `docs/native_image.md`, `docs/services.md`
(`include/sw_sdk_services.h`).

## 10. Building

MSVC x64 (Visual Studio 2022 or Build Tools), in a prompt where `vcvars64.bat` has been
called. `<repo>` is the StormFabric folder:

```bat
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /LD /I"<repo>\include" my_mod.cpp ^
   /Fe:my_mod.dll /link /DLL /MANIFEST:NO /DEBUG:NONE
```

- `/MT`: static CRT, no runtime DLL to ship.
- The SDK also compiles as C (`SW_MOD_EXPORT` handles both).
- Automation example: `examples/build_examples.bat`.

## 11. Installing, launching, checking

1. **Close Stormworks.**
2. Copy `mod.json`, the DLL and/or `content/` to `<Stormworks>/mods/my_mod/`.
3. Enable it: the in-game **MODS** menu, or a `my_mod=1` line in `mods/_modloader.ini`.
4. Launch the game through **`Launcher.exe`** (not `stormworks64.exe` directly on an
   install without Steam).
5. Read `<Stormworks>/StormworksModLoader.log`: mod loading, `host->log` messages, loader
   errors.

Quick troubleshooting:

| Symptom in the log or the menu | Likely cause |
|---|---|
| `NO DLL` / `MISSING DLL` | wrong `main`, DLL missing or misnamed |
| `MISSING: x` / `NEED: x` | dependency missing or disabled |
| `SwModInit` returned `false` | a version/capability check failed (log why) |
| tick/events never received | no world loaded, or a game build other than 1.15.23 |
| addon library missing | `script.lua` loaded before the mod: reload the world |
| content ignored | game not restarted, wrong path under `content/`, conflict (see log) |
| `plugin … callback crashed; disabled` | exception or invalid access in a mod callback |
| a Windows API fails only in game (e.g. WinHTTP error 8) | a mod DLL has the name of a System32 DLL (rule 7) |

## 12. Checklist before shipping a mod

- [ ] No game address or offset in the code.
- [ ] Every appended SDK member and every capability checked before use.
- [ ] No `luaState` kept; no native UI from the tick or an event.
- [ ] `checkStack` before every Lua push; the right number of results returned.
- [ ] No game file in the mod package.
- [ ] Valid `mod.json`, unique lowercase `id`.
- [ ] DLL name absent from `C:\Windows\System32`.
- [ ] Tested in game: loading visible in the log and the behaviour observed.
