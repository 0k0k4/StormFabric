#pragma once
#include "../include/sw_mod_sdk.h"
#include <cstdint>
namespace swml {
bool ValidateVideoProfile(uint8_t* base);
bool InstallNativeVideo(uint8_t* base);
const SwVideoApi* NativeVideoPrimitives();
void ReleaseNativeVideoOwner(const SwModHost* owner);
// Render thread, current GL context: texture of an image owned by `owner`, pixels uploaded,
// in the same colour encoding as the monitor's native video texture (srgb). Installed by the
// engine API (mod_image); NULL leaves monitor image sources inert.
using VideoImageTextureFn = bool (*)(const SwModHost* owner, uint64_t image, bool srgb, unsigned* texture);
void SetVideoImageResolver(VideoImageTextureFn resolver);
}
