#pragma once
#include "game.h"
#include "../include/sw_mod_sdk.h"

namespace swml {
void InitializeEngineApi(const sw::Api& api);
const SwEngineApi* GetEngineApi();
void RegisterEngineApiOwner(const SwModHost* owner);
// Must be called BEFORE SwModUnload / FreeLibrary, also on failed initialization.
void ReleaseEngineApiOwner(const SwModHost* owner);
}
