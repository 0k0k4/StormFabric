#pragma once
#include "../include/sw_mod_sdk.h"

namespace swml {
bool EnableNativeLua(const SwModHost* host);
bool EnableNativeQolEditor(const SwModHost* host); // optional, independent of core Lua startup
void DisableNativeLua();
void SetNativeLuaEnabled(bool enabled);
void ReleaseNativeLuaOwner(const SwModHost* owner);
const SwLuaApi* NativeLuaPrimitives();
const SwLuaExtrasApi* NativeLuaExtras();
// True once the addon/mission script VM lifecycle hooks are installed (newer build only).
bool NativeAddonLuaInstalled();
// Simulation tick subscriptions. The call-site redirection is installed on the first
// subscription; it requires the central Lua service (EnableNativeLua) to be started.
SwResult SubscribeSimulationTick(const SwModHost* owner, SwSimulationTickCallback, void* userData, SwHandle* out);
SwResult UnsubscribeSimulationTick(const SwModHost* owner, SwHandle handle);
}
