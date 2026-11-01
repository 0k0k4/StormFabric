#pragma once
#include "engine_discovery.h"
namespace swml {
struct CustomDisplayLayout {
    uint32_t definition = 0, width = 0, height = 0, scale = 0;
    uint32_t video = 0, overlay = 0, definitionId = 0, archiveId = 0;
    uint32_t createSlot = 0, releaseSlot = 0;
    uint32_t minX = 0, maxX = 0, minZ = 0, maxZ = 0;
};
// Decodes operands only after full retained function/table contracts match.
// Unknown/unreviewed layouts fail closed; no version-specific offset fallback.
bool DiscoverCustomDisplayLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayLayout& out);
} // namespace swml
