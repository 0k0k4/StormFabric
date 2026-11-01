# StormFabric v0.1.0-alpha

> **First Public Alpha Release** of StormFabric — the native C++ mod loader and plugin SDK for Stormworks: Build and Rescue.

---

## ⚡ What is StormFabric?

StormFabric is a non-invasive, high-performance C++ mod loader for Stormworks:
* **Zero Modified Game Files:** Loads via a standard Windows `iphlpapi.dll` proxy.
* **In-Game Menu:** Adds a native "MODS" button directly to the Stormworks main menu to view and toggle plugins.
* **Stable C++ SDK (ABI v1):** Provides safe engine abstractions so mods don't rely on fragile hardcoded memory offsets.

---

## 🚀 Featured Showcase Mod: `mc_lua_qol`

Included in this release is **`mc_lua_qol`** (Microcontroller Lua QoL & Unlocker), demonstrating the power of native engine hooks:

* **128 KB Microcontroller Scripts:** Completely removes the vanilla 4 KB character ceiling, allowing massive codebases inside vehicle microcontrollers.
* **CPU Timeout & Instruction Unlocks:** Lifts execution limits so complex navigation, radar, and physics algorithms never hit unexpected timeout crashes.
* **Native Helper Functions:** Injects 25+ fast native C++ helpers directly into Lua:
  * Hardware-accelerated PID controllers.
  * Touchscreen button hitboxes & input handlers.
  * Hex color decoding & drawing utilities.
  * High-performance math and telemetry utilities.

---

## 📦 Package Contents

```text
StormFabric-v0.1.0-alpha.zip
├── iphlpapi.dll                     (The StormFabric proxy loader)
└── mods/
    └── mc_lua_qol/
        ├── mc_lua_qol.dll           (Compiled 64-bit native mod)
        └── mod.json                 (Mod manifest and metadata)
```

---

## 🛠️ Installation

1. Download **`StormFabric-v0.1.0-alpha.zip`** from the **Assets** section below.
2. Open your Stormworks installation folder:
   * Right-click **Stormworks** in your Steam Library &rarr; **Manage** &rarr; **Browse local files**.
   * (Usually `C:\Program Files (x86)\Steam\steamapps\common\Stormworks`).
3. Extract the contents of the ZIP archive directly into that folder so that:
   * `iphlpapi.dll` is in the same folder as `stormworks64.exe`.
   * The `mods/` folder is placed in the root directory.
4. Launch Stormworks normally through Steam. A new **MODS** button will appear on the main menu!

---

## 🧹 Uninstallation

To completely uninstall StormFabric:
1. Delete `iphlpapi.dll` from your Stormworks directory.
2. Delete the `mods/` folder (or just `mods/mc_lua_qol`).
3. Your game is immediately 100% back to vanilla. No original files were ever modified.

---

## ⚠️ Notes & Requirements

* Requires Windows 10/11 64-bit and Stormworks 64-bit (`stormworks64.exe`).
* Designed and reverse-engineered against Stormworks v1.15.x.
* If you encounter issues, please report them on [GitHub Issues](https://github.com/0k0k4/StormFabric/issues).
