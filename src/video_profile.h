#pragma once
#include <cstdint>
#include <cstddef>
// Reference-build binding keys, resolved by engine_bindings for each supported
// executable. Never add these keys directly to a module base. Plugins do not see them.
namespace swml { namespace video_profile {
constexpr uint32_t kMonitorDrawCall = 0x74a538;
constexpr uint32_t kGpuDrawCall = 0x6d87d0;
constexpr uint32_t kMonitorDraw = 0x74aa50;
constexpr uint32_t kGpuDraw = 0x688ec0;
constexpr uint32_t kMonitorDestroy = 0x367c80;
constexpr uint32_t kMonitorVtable = 0xb70ea0;
constexpr uint32_t kDrawElementsIat = 0xa925f0;
// Direct loader access offsets verified unchanged in both supported binaries:
// tools/newer_video_bindings.json. Engine-owned e50/e68 and 2830/2898 fields
// are intentionally NOT accessed by this loader.
constexpr size_t kMonitorSignal = 0x1a8;
constexpr size_t kResourceObject = 8;
constexpr size_t kTargetTexture = 0x48;
} }
