#pragma once
#include <cstdint>
namespace swml {
// Stable reference keys are loader-internal identities, not addresses for plugins.
uint32_t EngineRva(uint8_t* base, uint32_t referenceRva);
bool IsSupportedEngineBuild(uint8_t* base);
bool ValidateEngineBinding(uint8_t* base, uint32_t referenceRva);
bool ValidateEngineLayouts(uint8_t* base);
bool ValidateReviewedEngineMemory(uint8_t* base);
// Production activation also authenticates the on-disk executable by SHA-256.
bool AuthenticateEngineExecutable(uint8_t* base);
}
