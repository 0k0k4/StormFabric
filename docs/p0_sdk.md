# P0 SDK — implementation status (2026-10-14)

**This is a partial P0 implementation, not completion of all nine proposed services.**
Native game rendering is retained; mods contain no addresses. Existing SDK v2 /
engine ABI v1 prefixes remain unchanged. New members are appended; check
`api->structSize` and the member table's `structSize` before using them.

## Available implementation

| Service | Delivery | Evidence |
|---|---|---|
| Runtime | Owner-scoped subscriptions, UI events, monotonic clock, deferred game-thread tasks, cancellable/repeating timers | Actual backend unit tests; native UI hook integration compiled, not gameplay-tested |
| Input | Named actions, virtual-key bindings/modifiers, conflict detection, pressed/down/released snapshots | Backend tests; Windows keyboard polling, not Stormworks remapping or controller input |
| Native HUD/editor UI | Opaque owner-scoped handles; roots, static windows, text, buttons, visibility, recursive destruction and native notifications | Fake native-tree tests + reviewed existing HUD/editor lifecycle bindings; not gameplay-tested |
| Configuration/mod data | Bounded binary reads, size queries, atomic writes, safe keys, reparse/path checks | Actual filesystem tests; see `p0_storage.md` |
| Lua values/tables | Appended `lua->extras`: strings including NUL, nil, integer, tables, fields, types, pop, string copy | Retained two-build static bindings + backend tests; see `p0_lua.md` |

Additional native UI static evidence: `tools/ghidra/p0_ui.txt` records that
`remove_child` invokes native teardown/destruction, and the rectangle-button
constructor initializes its composite base (labels may be native child elements).
The existing central per-build constructor/remove bindings remain the activation
gate; these decompilations are not new gameplay proof.

`examples/p0_demo/` demonstrates a native HUD, F8 action and persisted visibility.
Build with `examples/build_examples.bat`; artifacts remain in `build/examples/`.
The expanded demo includes automatic PASS/FAIL/SKIP checks and `demo.lua` for new
microcontroller VMs. Instructions: `examples/p0_demo/README.md`.
Follow-up deployment: the P0 loader and expanded demo were installed in the actual
newer game root while stopped, with backups and SHA-256 copy verification. Demo
enabled as `p0_demo=1`; existing mod settings preserved. No game was launched and
no gameplay validation is claimed.

## Explicitly unavailable

- `api->world == NULL`: world, entity, vehicle and player snapshots are not activated.
  Types are reserved in the append-only SDK, **not usable bindings**.
- Simulation/world/entity/player subscriptions return `SW_UNAVAILABLE`.
- Simulation time returns `SW_UNAVAILABLE`, without changing the output.
- World-scoped storage returns `SW_STORAGE_UNAVAILABLE`: no validated stable world ID.
- UI roots for the main menu are unavailable: its full teardown contract is not
  hooked by the new opaque-handle service. The existing raw legacy UI API is unchanged.
- No teleport/spawn/despawn/control/authority writes, native game-action interception,
  mouse/controller binding or drag/resize/editor undo is advertised.
- Lua `raiseError` is intentionally unsupported; addon VMs are not supported.

`p0_world_evidence.md` records recovered routines and remaining blockers. An addon
`onTick` is not a global simulation tick; addon creation/destruction can happen on
reload and must not be mistaken for world load/unload.

## Runtime semantics and lifetimes

- Scheduling runs on the thread observed by the reviewed native UI update hook.
  It is always deferred, even if called on that thread; there is no execution under
  loader lock and no worker-thread access to native objects.
- Timers use monotonic wall time, **not simulation time**. They only run when a UI
  update is pumped; pausing or absence of a hooked surface can delay execution.
- Every UI surface pulse has its own sequence. Simultaneous HUD/editor pulses are
  not deduplicated into a game frame. `uiDeltaSeconds` is between pulses.
- At most 256 tasks execute per pulse; repeat timers skip missed intervals rather
  than running an unbounded catch-up loop. New tasks wait for the next pulse.
- Per owner: 256 subscriptions, 256 tasks, 64 actions, 512 registered UI handles.
  Retained click tokens are capped conservatively at 8192 process-wide; further
  button creation returns `SW_LIMIT_REACHED` until the loader process restarts.
  IDs increase monotonically and are never reused. Cancelled/destroyed handles and
  handles from another owner return `SW_NOT_FOUND`.
- Runtime registration/scheduling can be used from worker threads. Callback execution
  and release are serialized. Callbacks must not join a worker blocked on runtime APIs.
- Native UI operations require the recorded UI thread. A surface becoming ready does
  not imply a world load. Subscribe to UI updates too if enabled after a ready event.
- Microcontroller Lua create/destroy events are queued and delivered on the next UI
  pulse with `SW_SURFACE_NONE` and an opaque VM generation, never a native pointer.
  Subscribing starts the centrally validated Lua service. Queue limit: 4096; overflow
  is logged and drops further events. A create event is historical, not a guarantee
  the VM remains alive at delivery. These events do not permit stack access.
- Native teardown invalidates UI handles **before** native destruction. Loader release
  revokes subscriptions/tasks/actions/Lua callbacks and removes mod roots before DLL
  unload. `SwModUnload` receives an already-revoked owner; persist while still active.
- Keyboard actions are suppressed if Stormworks is not foreground or its UI context
  has a focused element. This is conservative, not a certified text-input classifier.
  Keys are not consumed: native game controls can still receive the same key.
- Callback exceptions are contained and the offending callback disabled. This does
  not sandbox arbitrary native plugin memory access or guarantee a callback time limit.

## Remaining validation

### First gameplay feedback: oversized text

The supplied gameplay screenshot showed giant P0 glyphs. The wrapper incorrectly
used layout-sized `.028/.03` values as native constructor font scalars. Body/title/
button defaults are now `.00044/.00050/.00048`, with `.8` line spacing, matching
existing native menu/notification usage. Regression tests inspect all three native
text-constructor calls. This fixes the parameter mismatch in source; corrected
appearance still requires a fresh-game check after installing the rebuilt loader.
The corrected loader was installed at 16:28 after both game and launcher exited;
SHA-256 `6527afdc62c9f9b5bebc5755a0da6e6e43d0e85c7c87431ccafd25dd85d47a97`.
Prior DLL and runtime log are retained in
`build/backups/p0-font-fix-20261014-162833/`. All 13 C++ suites passed, including
font-scale regression assertions. No post-fix gameplay screenshot is available.

The 16:26 runtime log confirms demo registration, task/timer/cancellation/storage
checks, native widget creation/destruction and three queued Lua creation events.
It does **not** establish visual correctness, actual button hit testing or execution
of `p0_test.run()`; the screenshot specifically contradicts visual correctness of
the initial font defaults.

Second gameplay screenshot (16:29): title/status font scale is now readable,
`PASS 25 / FAIL 0` and three Lua VM creation events are visible. Button caption
still wrapped vertically because its native text box used negative fill bounds.
Caption bounds now explicitly match the positive button width/height, centred
at `(0,0)`; nonpositive button dimensions are rejected. Regression tests inspect
the actual text-call bounds. This second layout fix still needs gameplay confirmation.
Installed with the game/launcher stopped at 16:31; SHA-256
`38cf7a7857f2482f65b4cf51fc2bc9acc8109c9b5f84fc6e35d3f694b4745842`;
prior DLL/log retained in `build/backups/p0-button-label-fix-20261014-163158/`.
All 13 C++ suites passed before deployment. No post-button-fix gameplay proof yet.

### Subsequent gameplay verification (16:35)

The user's subsequent screenshot shows the corrected native panel/caption,
`PASS 100 / FAIL 0`, and a vehicle monitor displaying `P0 LUA CHECKS PASSED`.
The actual runtime log corroborates native button click events at 16:33:28,
16:33:32 and 16:33:35, a Lua VM destroy/reinit notification, and at 16:35:08:
binary string push/copy, string/integer/nil types, table allocation, field roundtrip/
bounded copy and stack restoration. The unsafe error API remains explicitly
unsupported, as intended. Task/timer/storage checks also passed in this session.

The 100 count is cumulative across reruns, not 100 distinct services. This verifies
these exercised paths and corrected HUD/button appearance on the installed build;
it does not establish unavailable world/simulation bindings, F8/focus behaviour,
restart persistence, DLL-unload safety or the independent video-effects/capture
backend. Those checks remain separate.

User follow-up: F8 toggling and visibility persistence are reported working, and
the button works in the microcontroller editor. Clicking the HUD button during
ordinary first-person gameplay is not established: normal mouse-look does not
provide the editor's interactive UI cursor. No certified SDK cursor/input-mode
switch is currently exposed; do not advertise the gameplay button as interactable
without a validated native input route. DLL-unload/focus edge cases remain pending.

The P0 DLL and expanded demo are now installed (game stopped during replacement).
Gameplay validation is still required: load a world, verify native HUD/F8 and config persistence,
change HUD/editor surfaces, destroy parent roots, disable/re-enable the mod and
confirm callbacks/resources do not survive unload. Do not overwrite the installed
loader while Stormworks is running.
