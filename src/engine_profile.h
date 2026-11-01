#pragma once
#include <cstdint>

namespace swml {
// Fail-closed Lua API fingerprints and hook-conflict checks, using the central
// reviewed build map (including layout-bearing function bodies).
bool ValidateLuaProfile(uint8_t* base);
}
