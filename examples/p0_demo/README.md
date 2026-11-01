# P0 native SDK test mod

This mod tests the **implemented** P0 APIs, not the unavailable world/vehicle/player
bindings. No game addresses or external overlay. Native rendering only.

1. Enable **P0 native SDK demo** in MODS. Automatic checks write `PASS/FAIL/SKIP`
   lines under `[p0_demo]` in `StormworksModLoader.log`.
2. Load a world: a native test panel appears on the HUD. Open a microcontroller
   editor to test its separate panel. **Run SDK checks** repeats the storage/tasks
   checks; the timer executes three times and cancels itself.
3. **F8** toggles visibility and writes `config/hud_visible.bin`. Disable/re-enable
   the mod to verify persistence. Keyboard actions are suppressed while UI focus
   is captured or the game is not foreground; the key is not consumed.
4. Copy `demo.lua` into a microcontroller **after enabling the mod**, spawn/reload
   the vehicle and connect its video output to a powered monitor. It calls
   `p0_test.run()` once, displays the result and sets bool output 1 on success.
   Newly created VM lifecycle events appear in the log. Existing VMs need reload.
5. Disable the mod: native panels must disappear and tasks/callbacks must stop.
   Existing `p0_test.run` closures become inert. Re-enable/reload vehicle to retest.

Automatic checks: deferred game/UI-thread execution, timers/cancellation/stale
task handles, action registration/stale handles, binary/empty/config storage,
size queries/short-buffer nonmutation/path rejection/invalid owner; native
text/buttons/destruction/stale UI handles; Lua string/NUL/integer/nil/table/field/
type/pop/bounded copy. Notifications report runtime-check status.

Interactive/manual checks: F8 edges and focus suppression, actual rendering and
button hit testing, surface transitions, persistence across restart and unload.
PASS logs **do not** prove these manual checks, asynchronous GPU video capture,
all error paths, multimonitor rendering or GL-context recreation.

SKIP: real simulation ticks, world/entities/vehicles/players and world save storage.
`raiseError` is checked as explicitly unsupported, never used to throw through C++.
Self-test files are isolated as `config/p0_selftest.bin`, `data/p0_selftest.bin`
and `data/p0_empty.bin`; no user files are deleted.
