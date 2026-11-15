#pragma once
#include <cstdint>

namespace swml {
// Core custom-library service: Lua primitives, table/extra helpers and the
// microcontroller VM lifecycle slots. Fail-closed fingerprints use the central
// reviewed build map (including layout-bearing function bodies).
bool ValidateLuaProfile(uint8_t* base);
// Optional QoL/editor hooks: editor vtables, editor helpers, QoL screen/input/
// output enrichment and the timeout unlock. Independent of the core service so
// runtime Lua startup never depends on optional editor targets.
bool ValidateQolEditorProfile(uint8_t* base);
// Optional addon/mission script VM lifecycle (c_playlist_lua_script vtable slots
// 1 and 3). Certified for the newer reviewed build only: every other build,
// a changed method body or an already replaced slot leaves it unavailable.
// The state pointer lives at self+8, exactly as for microcontroller scripts.
struct AddonLuaProfile { void** vtable; void* close; void* init; };
bool ResolveAddonLuaProfile(uint8_t* base, AddonLuaProfile* out);
bool ValidateAddonLuaProfile(uint8_t* base);
// Optional protected call: lua_pcallk(L, nargs, nresults, errfunc, ctx, k).
// Newer reviewed build only; NULL when the complete retained body differs.
void* ResolveLuaPcallk(uint8_t* base);
}
