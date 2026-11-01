# Engine discovery: implementation progress

## Current delivery (2026-10-14)

P0 SDK additions and their explicit limitations are tracked in [p0_sdk.md](p0_sdk.md).
The regenerated central profile also retains the new hash-pinned Lua extension
bodies and callee witnesses. World/player/vehicle audit candidates are **not**
activated; see [p0_world_evidence.md](p0_world_evidence.md). No gameplay verification
or installed-DLL replacement has been performed for the P0 additions.

**Two exact executable builds now have reviewed activation profiles. Generic
unknown-build activation remains disabled.**

Known builds select a central binding profile, authenticate the executable's full
SHA-256, and validate every compiled code/vtable/layout witness before hook writes.
UI, Lua, notifications and video use the same reference-key-to-current-RVA mapping.
Unknown builds still run read-only discovery and install no hooks. This is targeted
support for the user's newer executable, not automatic support for future versions.

An unknown build still installs **no hooks**: matching code alone does not certify
the layouts the loader directly accesses. No setting bypasses this safety gate.
The three remaining ambiguous targets also demonstrate why a generic signature
scan would not be sufficient.

| Approved stage | Status |
| --- | --- |
| Dependency inventory / shared resolver | Shared scanner plus compiled reviewed bindings consumed by all native services |
| PE section scanning / address operands / RTTI / imports | Implemented; x64 instruction boundaries and operand offsets exported offline by Ghidra |
| Terminal progress / persistent diagnostics | Implemented and console-buffer tested |
| Native layout and data-reference certification | Direct loader-accessed layout witnesses reviewed for these two files; unknown builds remain diagnostic-only |
| Core Lua vs QoL/editor capability separation | Pending; discovery groups exist, runtime Lua startup is not yet split |
| Migrate Lua/video/notification hook consumers | Implemented central two-build bindings; legacy-hook conflict checks retained |
| Atomic installation and dependency checks | All retained code/callee/slot/layout witnesses checked before startup writes; multi-vtable transactions acquire all pages before writes; video retains rollback |
| Multiple actual versions + in-game validation | Both files pass offline ASLR/profile tests; newer native menu and both plugin startups verified; map/render tests remain |

## Progress visibility

When the existing debug terminal is enabled at startup, it shows an in-place
ASCII bar, percentage/count, phase and detail. Symbol results and phase changes
also remain in terminal scrollback; scanner checkpoints do not flood scrollback.
All useful checkpoints are written to `Stormworks/StormworksModLoader.log`.

Example:

```text
[Discovery] [##########----------] 50% 46/92 RUNNING | Functions | native_2e7600: FOUND ...
[Discovery] [####################] 100% 9/9 PHASE FINISHED | RTTI/vtables | ...
```

`100%` means **that phase's work has finished**, not that all targets are safe.
`FOUND`, `MISSING`, `AMBIGUOUS` and `INVALID` distinguish discovery results.
An ambiguous scan result can be corroborated later by a unique RTTI class and its
required vtable slots; consult the final Relationships results, not just the first
scan result. The final summary explicitly reports unknown-layout rejection.

The logger is synchronized with normal log output. It does not force-open a
disabled terminal. Its existing `cfg.open_terminal_startup` setting is respected.

## Evidence and algorithm

Reference: Stormworks 1.15.18 x64, SHA-256
`55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3`.

- `tools/discovery_inventory.py` inventories loader function expressions, required
  vtable slots and the two native video call windows.
- `tools/ghidra/Discovery.java` exports full function bodies and bounded hook
  windows from the existing Ghidra project, **read-only, without analysis**. It
  checks the project's executable SHA-256 before exporting.
- Only decoded external relative/RIP-relative address operands are masked.
  Internal branches, field displacements and other instruction bytes remain fixed.
  Computed-call analysis references are not treated as encoded operands.
- `tools/discovery_contracts.json` retains 92 primary contracts, 114 direct-callee
  witnesses and 9 RTTI/vtable contracts.
- `tools/gen_discovery_profile.py` emits `src/engine_discovery.generated.h`.
  Profiles are compiled into the loader, not loaded from arbitrary runtime JSON.
- `tools/discovery_data_witnesses.json` retains reviewed, typed image-base-relative
  jump-table operands. Generation hash-checks the reference executable, instruction
  bytes and all table entries. Runtime accepts moved tables only when the base
  producer still resolves to image base and all local branch targets are preserved.
- `src/engine_discovery.cpp` checks x64 PE bounds, uses a reviewed-RVA fast path or
  scans executable sections using the longest literal anchor, then checks the
  whole masked body. Multiple matches are not resolved by picking the first.
- RTTI requires a bounded MSVC x64 COL, exact class name, and all required slot
  bodies. TypeDescriptors can live in `.data`; COL/vtables must be read-only.
- Calls are decoded using the retained instruction operand/end offsets, then
  checked against discovered functions or full direct-callee witnesses.
- `glDrawElements` is located by PE import identity (`OPENGL32.dll` + name), never
  by the current IAT address. Missing original thunk/name metadata is rejected.
- Runtime scans exclude discardable sections and verify committed/readable
  coverage of the sections used. They do not dereference arbitrary outside pointers.

**Limits:** this is deliberately conservative. Full function bodies can change
under compiler optimization even when behavior does not. Conversely, masked data
references, indirect-call targets, deeper callee dependencies and native layouts
are not fully certified yet. `GroupReady()` is a discovery result, **not** permission
to activate an unknown build. The reviewed build's existing safety gate is retained.

## Validation

`cmd /c StormworksModLoader\tests\run_tests.bat` includes eight C++ suites plus C SDK
compilation. Both actual files are checked under simulated ASLR, including the
complete reviewed memory witness set, UI API resolution and Lua/video/notifications.

The reference mapped under simulated ASLR yields **101/101** targets. Changing
only its timestamp forces scanning: **98/101** final targets; UI, core Lua,
notifications and video discovery groups resolve, while QoL/editor contain three
unresolved duplicate bodies (`0x24b140`, `0x2e88b0`, `0x2e8a80`). This is **not** a
test against a different game version.

Tests cover moved synthetic functions, changed rel32 displacements, duplicate
patterns, non-executable/cross-section rejection, invalid PE bounds, named OpenGL
imports, missing original import thunks, altered native call destinations, and
overwritten vtable slots. Unknown-build discovery leaves the image untouched.
The test executable never executes game code or loads the executable as a program.

For standalone, read-only reference validation after building:

```powershell
& '.\build\tests\engine_discovery_tests.exe' '.\Stormworks\stormworks64.exe'
```

For a different executable, use `--diagnose` to avoid reference-specific assertions:

```powershell
& '.\build\tests\engine_discovery_tests.exe' '<StormworksNewerPath>\stormworks64.exe' --diagnose
```

### Initial newer-build diagnostics (historical)

The user supplied a discovery log for the build running from
`<StormworksNewerPath>\stormworks64.exe`, not the workspace's reference
copy. SHA-256 is `f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699`,
PE timestamp `0x6a9a9e20`, image size `0xd8b000`. No release number is inferred.

`tools/newer_offline_comparison.json` and the reproducible read-only script
`tools/compare_newer_offline.py` retain **static file evidence**, not REA or runtime
rendering evidence. They distinguish two rejection causes:

1. Three video contracts contain changed object-field displacements:
   monitor draw `0xe50 -> 0xe68`, GPU caller `0x2830 -> 0x2898`, and update accesses
   `0x246c/0x2468 -> 0x292c/0x2928`. These remain fixed, not wildcarded.
   The first two candidates are corroborated by the matching monitor call window
   or native GPU body; the update candidate is supported by whole-body similarity
   and its ten-part unwind partition. Field meanings/layout compatibility remain
   uncertified.
2. UI constructors match, but their common helper `0x99bc00 -> 0x99d3b0` uses
   indexed image-base-relative jump tables, which the initial contract left fixed.
   All four tables moved by `0x2000`, preserving all 43 checked local branch targets.
   Typed table validation now handles this relocation without ignoring target data
   or masking structure offsets.

The updated offline diagnostic finds **84/101** final targets (the old terminal
log found 70; the new checker also propagates invalid method dependencies to their
vtables). Remaining dependency mismatches and ambiguous bodies are still rejected,
and **unknown-build activation is still disabled**.

Rejection logs now identify the specific dependency, decoded target, first changed
fixed byte or table-validation failure. The Functions progress bar counts completed
symbols monotonically; local scan percentage is detail-only. Scanner checkpoints
do not flood terminal scrollback, and completed phases are not printed twice.

Regression tests cover moved tables, modified entries/base rejection, monotonic
function-phase progress and single terminal phase-completion output. The updated
DLL is built in `build/iphlpapi.dll` and, after Stormworks exited, installed at the
newer root `<StormworksNewerPath>\iphlpapi.dll`. The prior loader/log
were preserved in `build/backups/newer-discovery-diagnostics-20261014-145153/`.
Installed SHA-256: `8e793d677a9245febaac9338e3e0c87955e5b3d0302ee04a857d69a4da804b11`.
This is a **diagnostic improvement**, not newer-build activation. Do not replace
an installed loader while the game is running.

## Next implementation checkpoints

1. Verify native menu, map loading/notifications, editor, video effects/capture,
   overlays/tactile, multi-monitor rendering and GL lifecycle on the newer build.
2. Split core custom libraries from optional QoL/editor hooks and expose independent
   availability. Runtime Lua startup remains coupled to optional editor targets.
3. Expand typed data-reference/layout witnesses to allow safely certified unknown
   builds. Whole-function contracts and compiler-generated indirect tables remain
   conservative; matching bodies alone still cannot activate another executable.

## Reviewed newer-build activation

`src/engine_bindings.{h,cpp}` owns all reference identities, build selection,
full-file SHA-256 authentication, code fingerprints, slot checks and layout windows.
`tools/gen_engine_bindings.py` verifies both file hashes and derives mappings from
unique full-body matches, exact RTTI classes/slots and decoded caller relationships.
It cross-checks independent Lua/video evidence, including the formerly ambiguous
`0x24b140`, `0x2e88b0` and `0x2e8a80`. No unknown-build original-RVA fallback exists.
Compiled reviewed bindings are retained in `src/engine_bindings.generated.h`;
their derivation is retained in `tools/reviewed_engine_bindings.json`.

Static layout/ABI evidence is retained in `tools/newer_lua_bindings.json`,
`tools/newer_video_bindings.json` and `tools/ghidra/review_ui_{old,new}.asm.txt`:

- All Lua primitive, builtin I/O/screen callback and editor helper bodies match
  their instruction-bounded masks, preserving field displacements and immediate
  allocation sizes. This supports unchanged Lua value/state and script I/O layouts;
  it is static evidence, not an executed Lua regression test.
- UI widget constructors, lifecycle methods and menu hooks preserve their fixed
  instruction bytes, including loader-accessed fields and widget sizes. Rejected
  composite-render helpers include moved RIP-relative **indirect jump** operands
  that the first exporter did not mask (e.g. `0x1bb470+0x76 JMP [0x140ce3268]`).
  The newer profile validates its own complete bytes rather than blindly masking
  those differences. Editor dispatch tables likewise moved; no UI field offset is
  inferred from the rejected byte alone.
- Changed monitor update fields and native draw queues remain owned by the native
  game function. The loader delegates those accesses instead of using the old
  displacements. Its direct monitor signal `+0x1a8`, deferred resource object `+8`,
  texture ID `+0x48`, script Lua state `+8` and drawing permission `+0x304` are
  corroborated by matching native bodies and explicit code windows. The independent
  378-byte monitor constructor comparison preserves all 334 non-address bytes.
- Monitor draw argument 8 is an **integer flag**, not a float, in both binaries.
  The loader hook prototype and fake-backend test now preserve that ABI correctly.
  Other hook float/int stack arguments retain their reviewed calling conventions.
- `OPENGL32.dll!glDrawElements` is resolved by import identity, not a guessed IAT
  offset, and the current imported target is checked before installation.
- Native notification init/teardown context handling is unchanged; both notification
  vtables are acquired/installed as one transaction. Lua's five slots and menu's
  three slots likewise acquire all required pages before publishing any hook.

The terminal now reports `Reviewed bindings` progress for these known files,
rather than displaying the old reference-only scanner's `84/101 UNKNOWN` summary.
Full file hash + memory validation must succeed before the activation path.
This does not certify unrelated builds or validate live rendering/capture behavior.

### Deployment and observed startup (2026-10-14, 15:27)

Installed at `<StormworksNewerPath>\iphlpapi.dll`, SHA-256
`47ac9ea1659df3d3b8871fe7509b8f1626fb7b21b86c5860abe6010cfe0fde5d`.
Previous DLL/log: `build/backups/newer-build-activation-20261014-152509/`.
The newer root's `mods/` folder was empty; the rebuilt address-free `mc_lua_qol`
and `video_fx` plugins, manifests and video demo were installed there. Terminal
progress and both plugins are enabled in `mods/_modloader.ini`.

Launching the game executable directly exited before the menu; use this root's
**`Launcher.exe`**, whose `ColdClientLoader.ini` selects `stormworks64.exe`.
Startup through that launcher was observed, with a responding Stormworks window
and the following entries in the game's `StormworksModLoader.log`:

```text
[15:27:20.692] Engine bindings: authenticated reviewed build 2; all hook dependencies validated before writes
[15:27:20.692] Engine API v1 initialized (UI available, Lua validated, video validated)
[15:27:20.693] Native notifications: HUD and editor lifecycle hooks installed (UI context from init)
[15:27:20.693] initialisation terminee
[15:27:27.790] bouton MODS ajoute
[15:27:32.531] mod 'Microcontroller Lua QoL & Unlocker' ... charge ... [entree: SwModInit]
[15:27:32.551] Video: loader-owned native monitor hooks installed
[15:27:32.553] mod 'Native Video FX (experimental)' ... charge ... [entree: SwModInit]
[15:27:35.378] ouverture vue MODS : 2 mod(s) detecte(s), 2 charge(s)
```

These are **runtime startup/menu observations**, not proof of map notification
lifecycle, executed QoL Lua functions, camera shader effects, pixel orientation,
capture timing, performance, multimonitor behavior or GL context recreation.
Those gameplay checks remain pending. Leave the installed DLL alone while this
game process is running.
