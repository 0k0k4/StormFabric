#pragma once
#include "../include/sw_mod_sdk.h"
#include "../include/sw_sdk_storage.h"
#include <string>

namespace swml {
// Supplied by engine_api.cpp; must not call back into storage or dereference owner.
bool IsEngineApiOwner(const SwModHost* owner);
const SwStorageApi* GetStorageApi();
// Call after engine-owner registration. Snapshots modDir; never rereads it on I/O.
SwStorageStatus RegisterModStorageOwner(const SwModHost* owner);
// Call before SwModUnload / engine-owner removal. Does not delete persistent files.
void ReleaseModStorageOwner(const SwModHost* owner);
// Empty disables world storage. A nonempty ID must be the actual stable world ID,
// restricted to the same safe alphabet/length as keys. Invalid IDs disable storage.
SwStorageStatus SetStorageWorld(const std::string& worldId);
}
