#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
namespace swml {
struct SlotPatch { void** slot; void* expected; void* replacement; void** original; };
// Acquire every vtable page before publishing a hook. A failed acquisition or
// changed slot leaves every original slot untouched. Callers install at startup.
inline bool PatchSlots(const SlotPatch* patches, size_t count, decltype(&VirtualProtect) protect = &VirtualProtect) {
    struct Page { void* address; DWORD protection; };
    SYSTEM_INFO info; GetSystemInfo(&info);
    std::vector<Page> pages;
    for (size_t i = 0; i < count; ++i) {
        const auto& p = patches[i];
        if (!p.slot || !p.replacement || !p.original || uintptr_t(p.slot) % alignof(void*) || *p.slot != p.expected) return false;
        void* address = reinterpret_cast<void*>(uintptr_t(p.slot) & ~(uintptr_t(info.dwPageSize) - 1));
        bool found = false; for (const auto& page : pages) if (page.address == address) found = true;
        if (!found) pages.push_back({address, 0});
    }
    size_t acquired = 0;
    for (; acquired < pages.size(); ++acquired)
        if (!protect(pages[acquired].address, info.dwPageSize, PAGE_READWRITE, &pages[acquired].protection)) break;
    bool ok = acquired == pages.size();
    if (ok) for (size_t i = 0; i < count; ++i) if (*patches[i].slot != patches[i].expected) ok = false;
    if (ok) {
        for (size_t i = 0; i < count; ++i) *patches[i].original = patches[i].expected;
        for (size_t i = 0; i < count; ++i) InterlockedExchangePointer(patches[i].slot, patches[i].replacement);
    }
    while (acquired) {
        const auto& page = pages[--acquired]; DWORD ignored;
        protect(page.address, info.dwPageSize, page.protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), page.address, info.dwPageSize);
    }
    return ok;
}
}
