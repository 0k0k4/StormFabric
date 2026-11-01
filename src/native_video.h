#pragma once
#include "../include/sw_mod_sdk.h"
#include <cstdint>
namespace swml {
bool ValidateVideoProfile(uint8_t* base);
bool InstallNativeVideo(uint8_t* base);
const SwVideoApi* NativeVideoPrimitives();
void ReleaseNativeVideoOwner(const SwModHost* owner);
}
