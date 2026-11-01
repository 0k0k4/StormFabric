#pragma once
#include "custom_display_layout.h"

namespace swml {
struct CustomDisplaySnapshot {
    bool valid = false;
    void* definition = nullptr;
    uint32_t width = 0, height = 0, scale = 0;
    void* video = nullptr; void* videoManager = nullptr;
    void* overlay = nullptr; void* overlayManager = nullptr;
    char definitionId[128] = {}, archiveId[64] = {};
};
// Copies only; never retains a reference into an engine allocation. Bad memory
// or malformed strings fail the snapshot, not the original native call.
bool ReadCustomDisplaySnapshot(const void* monitor, const CustomDisplayLayout& layout,
                               CustomDisplaySnapshot& out);
// Explicit SWML_CD_OBSERVE=1 opt-in, authenticated executable, corroborated
// discovery and atomic slot publication required. No target overrides by default.
// A SECOND explicit opt-in enables only the fixed local 24x16 experiment, with
// an exact pinned archive and per-attachment sizes <= the stock 1x1 baseline.
bool InstallCustomDisplayObserver(uint8_t* base);
void UninstallCustomDisplayObserver();
} // namespace swml
