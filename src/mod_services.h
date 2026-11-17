#pragma once
#include "../include/sw_mod_sdk.h"
#include <set>
#include <string>
namespace swml {
const SwServiceApi* ServicePrimitives();
// Called by the mod loader before SwModInit: the mod's key and the keys of every mod it
// depends on, directly or transitively (mod.json "dependencies").
void SetServiceOwnerIdentity(const SwModHost* owner, const std::string& key, const std::set<std::string>& dependencies);
// Withdraws the owner's services and forgets its identity (called before its DLL is freed).
void ReleaseServiceOwner(const SwModHost* owner);
}
