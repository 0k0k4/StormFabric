# StormFabric v0.2.0

> Second alpha of StormFabric, the native C++ mod loader and plugin SDK for Stormworks: Build and Rescue.
> This release turns the loader into a gameplay modding platform: mods can now act on the world every tick,
> react to game events, extend addon scripts, and ship their own game content.

---

## ✨ What's new

### Gameplay modding (Stormworks v1.15.23 only)

* **Addon script libraries**: `lua->registerAddonLibrary` gives addon/mission scripts (`script.lua`) a native
  global table next to `server.*`.
* **Call the game from C++**: `extras->getGlobal` and `extras->call` run any Lua function, including the
  game's own `server.*` API, in protected mode. A Lua error comes back as `SW_LUA_CALL_FAILED` and never crashes the game.
* **Simulation tick**: `runtime->subscribeSimulationTick` calls a mod once per server update (about 60 per second),
  right after every addon's `onTick`, with a live addon Lua state.
* **Game events**: `runtime->subscribeGameEvent` delivers addon callbacks such as `onPlayerJoin`, `onVehicleSpawn`,
  `onCustomCommand` and `onChatMessage` to C++, once per event, with typed arguments.

### Content mods (no code)

* A mod can add or replace files under `rom/` (component definitions, meshes, audio, data) from its own
  `content/` folder, without touching game files. A mod can be content only (no DLL).
* The MODS menu shows `CONTENT` for an active content mod and `RESTART` when a change needs a restart.
  Conflicts between mods are reported in the log.

### SDK

* **Native primitives** (`host->api->native`): inline hooks, memory access and signature scanning for advanced mods.
* **Engine Lua binding surface**: 427 engine Lua entries exposed to mods.
* **Component physics attributes**: 120 fields exposed to mods.
* New capability bits: `SW_CAP_LUA_ADDON`, `SW_CAP_SIMULATION_TICK`, `SW_CAP_GAME_EVENTS`.
* **Backward compatible**: SDK version 2 and engine ABI 1 are unchanged. New members are appended,
  so mods built for v0.1.0 keep working.

### Docs and tooling

* **`AGENTS.md`**: a step-by-step guide to create a mod, with templates that compile as they are
  (content, tick, events, addon and microcontroller libraries).
* New docs: `docs/addon_lua.md` and `docs/content_mods.md`.
* New examples: `addon_lua_demo` (tick, events, addon library) and `content_demo` (adds a component).

### Fixes

* The debug terminal no longer freezes the game when text is selected in its window.
  Console output now goes through a dedicated writer thread.
* The test suite and offline generators are repaired; all 16 C++ suites pass.

---

## 📦 Package contents

```text
StormFabric-v0.2.0.zip
├── iphlpapi.dll                     (the StormFabric proxy loader)
└── How to install.txt
```

The loader only: no mod is included. The `mods/` folder is created on first launch.

---

## 🛠️ Installation

1. **Close Stormworks.**
2. Download **`StormFabric-v0.2.0.zip`** from the **Assets** section below.
3. Open your Stormworks folder (Steam: right-click **Stormworks** → **Manage** → **Browse local files**).
4. Copy `iphlpapi.dll` into that folder, next to `stormworks64.exe`. Replace the old one if you are
   upgrading from v0.1.0.
5. Launch the game normally through Steam. The **MODS** button appears on the main menu.
6. To add a mod, put its folder in `mods/` (for example `mods/my_mod/mod.json`), then enable it in the MODS menu.

## 🧹 Uninstallation

Delete `iphlpapi.dll` and the `mods/` folder. No game file is ever modified.

---

## ⚠️ Notes and requirements

* Windows 10/11 64-bit, Stormworks 64-bit (`stormworks64.exe`).
* Only two exact game builds are supported: v1.15.18 and v1.15.23 (SHA-256 checked). On any other build the
  loader installs no hook.
* Addon libraries, Lua calls, the simulation tick and game events require **v1.15.23**. Content mods and
  the rest of the SDK work on both builds.
* Not verified yet: multiplayer, pause and script reload for the tick and events; replacing existing game
  files, meshes and audio through content mods.
* Report issues on [GitHub Issues](https://github.com/0k0k4/StormFabric/issues).
