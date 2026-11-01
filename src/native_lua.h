#pragma once
#include "../include/sw_mod_sdk.h"

namespace swml {
bool EnableNativeLua(const SwModHost* host);
void DisableNativeLua();
void SetNativeLuaEnabled(bool enabled);
void ReleaseNativeLuaOwner(const SwModHost* owner);
const SwLuaApi* NativeLuaPrimitives();
const SwLuaExtrasApi* NativeLuaExtras();
}
