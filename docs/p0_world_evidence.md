# P0 world/events/entities/vehicles/players — offline audit

## Decision for implementation

**Do not enable native world/events/entities/vehicles/players support from this audit alone.** Real native routines and layouts were recovered, but a validated acquisition/lifetime contract for their objects, a global simulation boundary, and thread affinity are still missing. Return unsupported, not successful empty/fabricated data. UI-frame callbacks must not be renamed simulation ticks.

This is ordinary Ghidra/static-file evidence, **not REA Evidence and not observed execution**. No game execution, attachment, installation or shared source/SDK changes were performed.

## Artifacts and reproducibility

* Reference: workspace `Stormworks/stormworks64.exe`, SHA-256 `55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3`.
* New: `<StormworksNewerPath>\stormworks64.exe`, SHA-256 `f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699`.
* Image base used in disassembly: `0x140000000`; addresses below are **RVAs**.
* Existing analyzed reference project was opened read-only once. After encountering a project lock, analysis continued on private snapshot `%TEMP%\opencode\P0WorldOffline_20261014\Stormworks.gpr`. No lock was removed.
* Newer selected-disassembly project was **copied**, not opened/saved in place: private `P0WorldNewOffline_20261014\NewerBindingsReview.gpr` under the same temp root. Headless used `-noanalysis -readOnly`; selected decoding changes were discarded.
* `tools/ghidra/p0_world_*.commands.txt` retain Ghidra commands; corresponding `.txt` / `.asm.txt` retain output.
* `python tools/p0_world_compare.py` reproduces masked candidate searches and writes `tools/ghidra/p0_world_candidates.json`. JSON contains exact windows/masks, hashes, ambiguity and PE `.pdata` regions. These are discovery witnesses, **not activation certificates**. Do not resolve an ambiguous window by choosing the first hit or falling back to an RVA.

## Recovered identities

| Routine | Reference RVA | New RVA | Evidence/status |
|---|---:|---:|---|
| Addon `onTick` dispatch | `0x59caf0` | `0x59c4e0` | Unique entry window, complete new disassembly, string witness |
| Lua `server.getPlayers` | `0x59ea50` | `0x59e440` | Unique entry window, complete new disassembly, string witness |
| Lua `server.getPlayerPos` | `0x59f1a0` | `0x59eb90` | Entry window has **6 hits in each build**; actual routine corroborated by string and player-ring/position operations |
| Lua `server.getVehiclePos` | `0x5a0900` | `0x5a0260` | Unique entry window, complete new disassembly, string witness |
| Addon `onCreate` dispatch | `0x5d1080` | `0x5d0a10` | Unique window, complete new disassembly, string witness |
| Addon `onDestroy` dispatch | `0x5d1210` | `0x5d0ba0` | Unique window, complete new disassembly, string witness |
| Native vehicle-position implementation | `0x729d30` | `0x72a620` | Unique 82-byte window, decoded body, vtable slot corroboration |
| Simulation/controller constructor | `0x73e390` | `0x73eb90` | Unique window; old decompile and new constructor prefix |

Primary sources: `p0_world_api.txt`, `p0_world_vehicle.txt`, `p0_world_new.asm.txt`, `p0_world_new_vehicle.asm.txt`, `p0_world_candidates.json`.

### Important correction: no proven `getVehicles` API

The substring search `getVehicles` actually found **`getVehiclesByName`**, reference wrapper `0x5a0fd0`, registered by `0x599fa0`. Its argument count is one, and it filters exact vehicle names. It is not an all-vehicles enumeration API. Sources: `p0_world_refs.txt:16-20`, `p0_world_api.txt:553-669`, `p0_world_vehicle.txt:84-149`.

## Players: real fields, with explicit base provenance

The Lua wrappers receive the real Lua state in **RCX** (Windows x64). Decompiler extra RDX/R8/R9 parameters are error-path register artifacts; do not expose them as a four-argument C++ API.

They resolve pseudo-index `-0xf4629` using native Lua index resolver `0x97920`: tag low-nibble 2 gives the stored pointer; tag 7 gives userdata payload `value+0x28`. Call the resulting pointer **C**. In the inspected wrappers `A = *(C+0)` is addon appdata, and `S = *(C+8)` is a script-operators interface. **C, A, S and the UI context are not interchangeable.** Registration passes `script+0x28` to `0x1e3ee0` for the `server` table (`p0_world_contract.txt:1060-1065`); full closure ownership/teardown was not traced.

Player collection observed in both binaries:

```
A+0x410 : pointer to array of player pointers
A+0x418 : uint32 capacity
A+0x41c : uint32 ring head
A+0x420 : uint32 count
P = array[(uint32(head+i) % capacity)]
P+0x80  : uint16 peer ID
P+0x88  : nullable char* name
P+0x98  : nullable controlled-object pointer
P+0xa2 / +0xa3 : byte admin/auth
P+0x140 : uint64 steam ID
controlled-object+0x98 : uint32 object ID
P+0x18 / +0x20 / +0x28 : three double position coordinates
```

Sources: reference `p0_world_api.txt:67-195,290-337`; new `p0_world_new.asm.txt:253-262,303-440,564-635`. `getPlayerPos` compares **16-bit** peer IDs, constructs an identity-oriented `double[16]` matrix and replaces elements 12/13/14 with those coordinates. This does **not** establish player orientation. Missing player returns a false success flag. `0x1e3960` serializes 16 doubles into a Lua table (`p0_world_loop.txt:4304-4349`), not a float matrix.

These are proven **relative accesses**, not a license to read `HUDContext+0x410`. Neither safe object capture nor thread-safe access nor retention past the native call has been established.

## Vehicles: direct native lookup and transform

`c_script_operators` vtable is reference RVA `0xb0f0e0`, new `0xb11a28`; slot **+0xc8** points respectively to `0x729d30` / `0x72a620`. Constructor stores this vtable at controller+8. Thus the interface pointer **S** points at that embedded subobject, not at the controller start. Source: `p0_world_interface.txt:27,157-159`, `p0_world_vehicle.txt:294-307`, new constructor `p0_world_new_vehicle.asm.txt:135-150`, and JSON qword witness.

Native vehicle-position calling convention established by wrapper call, body and disassembly:

```
RCX = S (script-operators interface)
EDX = int32 vehicle ID
R8  = output double[16] (0x80 bytes)
R9  = pointer to int32[3] optional voxel coordinates
fifth stack argument = byte enable-voxel-position
AL  = success (upper RAX bits are not a bool contract)
```

Native manager **M = *(S+0x10)**. Its vehicle ring: backing pointer `M+0x30`, capacity uint32 `+0x38`, head uint32 `+0x3c`, count uint32 `+0x40`; each slot is a vehicle-record pointer **V**. `V+4` is int32 vehicle ID; `V+0x10` is the 0x80-byte matrix copied for ordinary position requests. Voxel mode follows additional nullable `V+0xe0`/subobjects and can select a different matrix. Do not approximate that branch by adding voxel coordinates yourself. Sources: `p0_world_vehicle.txt:6-80`; both assembly dumps show the same relative accesses.

This ring is the strongest recovered all-vehicle enumeration **candidate**. It is not yet a safe supported enumerator: manager acquisition, loaded/unloaded record meaning, destruction/reuse, synchronization and world epoch remain unresolved. Generic entity enumeration has not been recovered. A numeric vehicle ID alone is not a generation handle.

## Events / scheduler: what is and is not proven

### HUD update is UI only

Reference `0x5c78b0` calls UI-child update `0x1dd440`, decrements HUD field `+0x90`, and invokes currency-display virtual `+0x128`. It does not establish a simulation tick. Reference HUD init `0x5c7020` allocates HUD children. Source: `p0_world_roots.txt:41-352`. Keep the previously reviewed new mappings `0x5c7240` / `0x5c69b0` distinct from world functions; they were not re-reviewed here.

### Addon onTick: not a global tick hook

Reference dispatcher `0x59caf0` (new `0x59c4e0`) takes RCX=script object, RDX=appstate used for error reporting, R8=script-name/string object, R9D=integer forwarded to the Lua `onTick` argument. It dispatches only when script+0x78 status is zero. The native caller is `0x73f090`, at callsite **`0x7400f1`**, within a loop over addon scripts at appdata+0x188/+0x190/+0x194/+0x198. Additional flags gate this loop. Source `p0_world_loop.txt:767-807`; dispatcher `p0_world_api.txt:673-782`.

The integer comes from script-operators virtual `+0x450`; reference target `0x7365b0` returns uint32 at `M+0xbaeb8` (`p0_world_context.txt:1-8`). Its semantic units and update/reset rules were **not** established. Do not relabel it a monotonically increasing simulation tick or assume fixed timestep.

`0x73f090` is called from reference `0x864c50` and `0x867710`; the latter can iterate up to ten queued update packets. Source `p0_world_contract.txt:1547-1618,2840-2901`. No single-call-per-physics-step or main-thread guarantee has been established. A hook on addon dispatch would execute once per eligible addon, and zero times with no eligible addons.

### onCreate/onDestroy are script lifecycle, not exact world events

Both dispatchers iterate an addon-script ring. `onDestroy` then calls script virtual `+8`, so captured script/Lua references cannot be assumed live after it returns. `onCreate` forwards a fifth stack byte to the Lua callback; its complete caller semantics were not reconstructed. They also occur inside `0x73f090` in an in-place destroy/reload/create sequence, not just process/world teardown: `p0_world_loop.txt:488-533`. `0x5e58d0` invokes onDestroy before broader cleanup (`p0_world_loop.txt:4039-4066`). Therefore forwarding every `onDestroy` as WorldUnloaded and every `onCreate` as WorldLoaded is incorrect.

## Remaining gates before runtime support

1. Establish a global simulation entry/callsite and its exact ABI for both builds, with all thread/worker/catch-up paths accounted for; independent of UI and addon count.
2. Establish the exact world ownership transition and ready/unloading phases, including reload, connection, failed-load and teardown paths.
3. Capture A/S/M from a proven argument/constructor association; validate lifetime and access phase. No arbitrary shared global or UI-state reinterpretation.
4. Audit destroy/removal and ID reuse. SDK handles should combine world epoch and per-slot generation; invalidate before native teardown and on reload.
5. Snapshot only in the proven owning phase; validate nonzero capacity, count<=capacity, overflow-safe indexes/byte spans, bounded total count/name length and finite transforms. Do not retain native pointers in SDK handles or across frames. These are safety design requirements, not behavior observed in this audit.
6. Promote only reviewed instruction/data windows into central bindings with edges/object provenance. Existing candidate prologs alone are insufficient. Leave capabilities false until these gates are met.

### Checklist outcome

* Player enumeration/position relative layouts: **answered statically**, unsupported acquisition/lifetime.
* Vehicle-ID lookup/matrix relative layouts: **answered statically**, unsupported acquisition/lifetime.
* All-vehicle ring: **partial**, meaning/lifetime not certified.
* Generic entities: **unresolved**.
* Reliable global simulation/main-thread tick: **unresolved**; addon/UI substitutes disproven.
* Exact world load/unload callbacks: **unresolved**; addon lifecycle substitutes disproven.
