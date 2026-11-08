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
}
