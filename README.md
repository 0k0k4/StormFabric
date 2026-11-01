# StormFabric

**English** | [Français](README_FR.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://github.com/0k0k4/StormFabric)

A native **C++ mod loader and plugin framework** for **Stormworks: Build and Rescue** on Windows x64.
Using an `iphlpapi.dll` proxy, StormFabric integrates a native **MODS** menu directly into the game, manages mod dependencies, and provides a clean, centralized engine SDK for mods.

**Native in-game rendering — no external overlays or Dear ImGui.** All engine addresses, vtable offsets, and hooks remain contained inside the loader. Mods built with the SDK require zero hardcoded game addresses.

> **Experimental Project.** Two exact executable builds currently have verified profiles; this is not universal across all game updates. World, entity, vehicle, player, and simulation tick services remain strictly disabled.

---

## Table of Contents

- [Features & Capabilities](#features--capabilities)
- [Compatibility & Safety](#compatibility--safety)
- [Installation](#installation)
- [Testing Example Mods](#testing-example-mods)
- [Creating a Mod](#creating-a-mod)
- [Building & Testing](#building--testing)
- [Architecture & Documentation](#architecture--documentation)
- [Community Mods](#community-mods)
- [Disclaimer & License](#disclaimer--license)

---

## Features & Capabilities

- **Native In-Game MODS Menu**: View loaded mods, toggle enable/disable states, inspect settings, and read logs.
- **Dependency Management**: Topological sorting ensures dependencies load first and unload last. Local DLL support via `native_deps` and `AddDllDirectory`.
- **SDK v2 / Engine ABI v1**: Extensible C/C++ API designed with forward-compatible structs (`structSize` validation).
- **Diagnostics**: Colorized console logging, in-menu log viewer, native notifications, and detailed logging to `StormworksModLoader.log`.
- **Runtime Mod Reloading**: Reload mods on the fly with automatic callback revocation before DLL unloading.

| API | Features | Current Status & Scope |
| --- | --- | --- |
| `api->ui` | Legacy native widgets: text, buttons, rectangles, checkboxes, containers, scrolling | Low-level native tree; requires matching engine object lifecycles |
| `api->nativeUi` | High-level HUD/editor widgets: roots, static windows, labels, buttons, visibility toggles, notifications | Editor buttons and HUD displays verified; FPS cursor mode not exposed |
| `api->runtime` | UI/Lua VM events, deferred tasks, timers, task cancellation | UI frame updates and monotonic time (**not** simulation ticks) |
| `api->input` | Keyboard actions, key modifiers, pressed/down/released states | Keybinds (e.g. F8) verified; raw mouse/controller input not exposed |
| `api->storage` | Per-mod configuration and binary data, atomic writes, sanitized keys | Up to 4 MiB per file; world-bound storage inactive |
| `api->lua` / `lua->extras` | Custom microcontroller libraries, 34 QoL math/string/table helpers | Microcontroller VM only; `raiseError` intentionally omitted |
| `api->video` | Native monitor video feed effects and asynchronous frame capture | Experimental; reads return latest completed source capture |
| `api->world` | Reserved types for future entity/vehicle/world interactions | **NULL**: world, entity, vehicle, and player APIs are disabled |

For detailed implementation status, see the [P0 SDK Specification](docs/p0_sdk.md).

---

## Compatibility & Safety

StormFabric currently validates against two **Windows x64** game builds:

| Profile | Abbreviated SHA-256 Identity |
| --- | --- |
| Stormworks v1.15.18 (Reference) | `55914301628a…` |
| Tested Newer Build | `f9206d85c82f…` |

Full hashes and validation signatures are documented in [Engine Discovery](docs/engine_discovery.md). The game's displayed version number alone is not sufficient to guarantee compatibility.

Before installing any hooks, StormFabric verifies the complete SHA-256 hash of `stormworks64.exe` and validates code witness signatures, vtables, and data layouts.
**Unknown builds remain in diagnostic read-only mode: no hooks are installed.**

> **Notice**: Native DLL mods run inside the game's process. StormFabric is **not a sandbox**. Only install mods from sources you trust, keep backups of your saves, and close the game before updating DLL files on disk.

---

## Installation

1. **Close Stormworks and its launcher.** Back up any existing `iphlpapi.dll` in your game directory.
2. Download a pre-built release or compile the project from source.
3. Place `build/iphlpapi.dll` into your Stormworks root folder (next to `stormworks64.exe`).
4. Place mod folders inside `<Stormworks>/mods/`, each with its `mod.json` and DLLs.
5. Launch the game normally via **Steam or Launcher.exe**, then click the **MODS** button on the main title screen.

```text
Stormworks/
├── stormworks64.exe
├── iphlpapi.dll
├── StormworksModLoader.log
└── mods/
    ├── _modloader.ini
    └── my_mod/
        ├── mod.json
        └── my_mod.dll
```

### Automated Installation Script

You can also use the included batch installer from the project folder:

```bat
install.bat "C:\Path\To\Stormworks"
```

To uninstall, delete `iphlpapi.dll` from your game folder (or run `uninstall.bat`).

---

## Testing Example Mods

Pre-compiled examples are generated in `build/examples/`:

| Example | Description |
| --- | --- |
| [`p0_demo`](examples/p0_demo/README.md) | Native HUD panel, F8 toggle, storage roundtrips, and SDK check suite |
| [`native_api_demo`](examples/native_api_demo) | Custom `sw_demo` Lua library exposed to microcontrollers |
| [`mc_lua_qol`](examples/mc_lua_qol) | Quality-of-life mathematical and utility functions for microcontroller Lua |
| [`video_fx`](docs/video_fx.md) | Native monitor post-processing shaders and frame capture |
| `sw_core_lib` / `radar_telemetry` | Inter-mod dependencies and local helper DLL loading |
| `simple_dllmain_mod` | Standard Win32 DLL without SDK exports |

---

## Creating a Mod

### Manifest: `mod.json`

Every mod requires a `mod.json` descriptor in its directory:

```json
{
  "id": "my_mod",
  "name": "My Custom Mod",
  "author": "0k0k4",
  "version": "1.0.0",
  "description": "Mod description displayed in the MODS menu.",
  "main": "my_mod.dll",
  "dependencies": [],
  "native_deps": [],
  "enabled_by_default": true
}
```

### C++ Plugin Example (No Game Addresses)

Include [`include/sw_mod_sdk.h`](include/sw_mod_sdk.h) and access the engine via `host->api`:

```cpp
#include "sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

static void OnGameThread(void*) {
    g_host->log(g_host->modId, "Task executed on UI thread via SDK!");
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION ||
        host->api->structSize < offsetof(SwEngineApi, runtime) + sizeof(host->api->runtime)) {
        return false;
    }

    const SwRuntimeApi* runtime = host->api->runtime;
    if (!runtime || runtime->structSize < sizeof(SwRuntimeApi)) return false;

    g_host = host;
    SwHandle task = 0;
    return runtime->runOnGameThread(host, &OnGameThread, nullptr, &task) == SW_OK;
}

SW_MOD_EXPORT void SwModUnload() {
    // The loader automatically revokes registered callbacks before calling unload.
    g_host = nullptr;
}
```

Key guidelines:
- Always check `structSize` before accessing fields added in later SDK versions.
- Lua VM operations are only valid within their respective registered callbacks; do not cache VM pointers.
- Timers increment with monotonic time during UI updates, not game simulation ticks.

---

## Building & Testing

### Prerequisites
- **Windows x64**
- **MSVC** compiler (Visual Studio 2022 or Build Tools with C++ workload)
- **Windows SDK**

### Build Commands

From a Command Prompt:

```bat
build.bat
examples\build_examples.bat
tests\run_tests.bat
```

- **Loader output**: `build/iphlpapi.dll`
- **Example mods**: `build/examples/`
- **Unit test suite**: `build/tests/` (13 C++ suites, C compilation tests)

The offline unit test suite uses mocked backends and does not require launching the game. Tests that verify executable profiles against `stormworks64.exe` will run if the game executable path is provided, or gracefully skip if unavailable.

---

## Architecture & Documentation

| Document | Topic |
| --- | --- |
| [Native API](docs/native_api.md) | ABI specifications, service lifetime, callback ownership |
| [P0 SDK Specification](docs/p0_sdk.md) | Implementation verification, boundaries, and limits |
| [Engine Discovery](docs/engine_discovery.md) | SHA-256 profiles, signature masks, unknown build diagnostics |
| [Lua Extensions](docs/p0_lua.md) | Type marshalling, custom tables, and VM hooks |
| [Mod Storage](docs/p0_storage.md) | Storage isolation, key validation, atomic I/O |
| [Video Monitors](docs/video_monitors.md) / [Video FX](docs/video_fx.md) | Framebuffer capture and monitor source shader injection |
| [UI Notifications](docs/ingame_notifications.md) | In-game notification pipeline and HUD lifecycle |
| [World Audit](docs/p0_world_evidence.md) | Analysis of simulation boundaries and inactive services |

---

## Community Mods

Developing a mod using StormFabric? Open an Issue or Pull Request to get your mod featured here!

---

## Disclaimer & License

- **Disclaimer**: StormFabric is an independent, open-source project and is **not affiliated with, endorsed by, or sponsored by Geometa**. "Stormworks: Build and Rescue" is a registered trademark of Geometa.
- **License**: Released under the **[MIT License](LICENSE)**. Copyright (c) 2026 0k0k4.
