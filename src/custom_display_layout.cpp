#include "custom_display_layout.h"
#include <cstring>

namespace swml {
// Instruction-relative operand locations below come from the retained Ghidra
// decoder exports. They are NOT object offsets or fallback executable RVAs.
// Only a complete corroborated discovery result may authorize their extraction.
bool DiscoverCustomDisplayLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayLayout& out) {
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(64) ||
        !result.GroupReady(128)) return false;
    // Revalidate bodies, relationships and table against the CURRENT image.
    // A caller cannot smuggle arbitrary addresses in a constructed Result.
    using namespace discovery;
    #include "engine_discovery.generated.h"
    for (const auto& c : kDiscoveryContracts) {
        if (!(c.groups & (64 | 128))) continue;
        uint32_t address = result.Address(c.key);
        if (!address || address < c.key - c.start ||
            !Match(image, c, address - (c.key - c.start))) return false;
        for (size_t i = 0; i < c.edgeCount; ++i) {
            uint32_t target = 0;
            if (!DecodeEdge(image, address - (c.key - c.start), c.edges[i], target)) return false;
            const uint32_t resolved = result.Address(c.edges[i].target);
            if (resolved && target != resolved) return false;
            // Unidentified data references are not used as layout evidence.
            if (c.edges[i].flow && !resolved) {
                const Contract* witness = nullptr;
                for (const auto& dependency : kDiscoveryContracts)
                    if (dependency.key == c.edges[i].target) witness = &dependency;
                if (!witness || target < witness->key - witness->start ||
                    !Match(image, *witness, target - (witness->key - witness->start))) return false;
            }
        }
    }
    const uint32_t table = result.Address(0xb70ea0);
    const Table* monitor = nullptr;
    for (const auto& t : kDiscoveryTables) if (t.key == 0xb70ea0) monitor = &t;
    if (!monitor || !table) return false;
    CustomDisplayLayout layout;
    bool createFound = false, releaseFound = false;
    for (size_t i = 0; i < monitor->count; ++i) {
        const auto& slot = monitor->slots[i];
        uint32_t target = 0;
        const uint64_t location = uint64_t(table) + uint64_t(slot.index) * sizeof(void*);
        if (location > UINT32_MAX || !image.Pointer(uint32_t(location), target) ||
            target != result.Address(slot.target)) return false;
        if (slot.target == 0x366010) { layout.createSlot = slot.index; createFound = true; }
        if (slot.target == 0x366100) { layout.releaseSlot = slot.index; releaseFound = true; }
    }
    if (!createFound || !releaseFound) return false;
    auto operand = [&](uint32_t key, uint32_t relative, size_t size, uint32_t& value) {
        uint64_t at = uint64_t(result.Address(key)) + relative;
        if (!result.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), size)) return false;
        value = 0;
        std::memcpy(&value, image.base + at, size);
        // Observation allocation bound, NOT an engine layout fallback.
        return value && value < 0x10000;
    };
    if (!operand(0x366010, 0x53, 1, layout.definition) ||
        !operand(0x366010, 0x85, 4, layout.width) ||
        !operand(0x366010, 0x8f, 4, layout.height) ||
        !operand(0x366010, 0x2b, 4, layout.scale) ||
        !operand(0x366010, 0x7f, 4, layout.video) ||
        !operand(0x366010, 0xbd, 4, layout.overlay) ||
        !operand(0x316770, 0x72, 4, layout.definitionId) ||
        !operand(0x316770, 0xfc, 4, layout.archiveId)) return false;
    if (!operand(0x366010, 0x6c, 4, layout.minX) ||
        !operand(0x366010, 0x63, 4, layout.maxX) ||
        !operand(0x366010, 0x56, 4, layout.minZ) ||
        !operand(0x366010, 0x5d, 4, layout.maxZ)) return false;
    // Cross-instruction/layout invariants, rather than assuming adjacent members.
    uint32_t releaseVideo = 0, releaseOverlay = 0, widthAgain = 0, heightAgain = 0;
    if (!operand(0x366100, 0x2b, 4, releaseVideo) ||
        !operand(0x366100, 0x6b, 4, releaseOverlay) ||
        !operand(0x366010, 0xb6, 4, heightAgain) ||
        !operand(0x366010, 0xc3, 4, widthAgain)) return false;
    if (layout.video != releaseVideo || layout.overlay != releaseOverlay ||
        layout.width != widthAgain || layout.height != heightAgain ||
        layout.height != layout.width + 4 || layout.scale != layout.height + 4 ||
        layout.video != layout.scale + 8 || layout.overlay != layout.video + 16 ||
        layout.definition % alignof(void*) || layout.video % alignof(void*) ||
        layout.overlay % alignof(void*) || layout.definitionId % alignof(void*) ||
        layout.archiveId % alignof(void*)) return false;
    out = layout;
    return true;
}
} // namespace swml
