#pragma once
#include <cstddef>
#include <cstdint>

#include "../include/sw_mod_sdk.h"

namespace swml {
namespace native {

// Installs the native primitive service for one process image. Must be called
// once, before any mod is loaded. Idempotent; nullptr resets it (tests).
void InitializeNativePrimitives(uint8_t* base);
// The API table published through SwEngineApi::native (never NULL after init).
const SwNativeApi* NativePrimitives();
// Restores every hook owned by an unloading mod and frees its code blocks, in
// reverse installation order. Must run BEFORE the mod DLL is freed.
void ReleaseNativeOwner(const SwModHost* owner);
// Process-detach safety net: restores any still-active prologue (reverse order).
void ShutdownNativePrimitives();
// Owner validation is delegated to the engine API registry in production. When
// unset (unit tests), any non-null owner is accepted.
void SetNativeOwnerCheck(bool (*check)(const SwModHost*));

}  // namespace native
}  // namespace swml
