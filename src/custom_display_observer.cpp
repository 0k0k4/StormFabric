#include "custom_display_observer.h"
#include "custom_display_registry.h"
#include "engine_bindings.h"
#include "hook_transaction.h"
#include "custom_display_targets.h"
#include "log.h"
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace swml {
namespace {
using CreateFn = void (*)(void*, void*, int32_t);
using ReleaseFn = void (*)(void*, void*);
CreateFn g_create = nullptr;
ReleaseFn g_release = nullptr;
CustomDisplayLayout g_layout;
std::atomic<uint32_t> g_events{0};
bool g_installed = false;
uintptr_t g_base = 0;
size_t g_size = 0;
constexpr uint32_t kMaxEvents = 4096;
CustomDisplayQueueTargetFn g_request = nullptr;
void* g_monitorTable = nullptr;

bool Copy(const void* base, size_t offset, void* out, size_t bytes) {
    uintptr_t at = reinterpret_cast<uintptr_t>(base);
    if (!at || offset > UINTPTR_MAX - at || bytes > UINTPTR_MAX - (at + offset)) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at + offset),
                             out, bytes, &read) && read == bytes;
}
template<class T> bool Field(const void* base, size_t offset, T& out) {
    return Copy(base, offset, &out, sizeof(out));
}
template<size_t N> bool String(const void* base, uint32_t offset, char (&out)[N]) {
    const char* text = nullptr;
    uint32_t length = 0;
    if (!Field(base, offset, text) || !Field(base, size_t(offset) + sizeof(void*), length) ||
        length >= N || (length && !text)) return false;
    if (!length) { out[0] = '\0'; return true; }
    if (!Copy(text, 0, out, length)) return false;
    for (uint32_t i = 0; i < length; ++i)
        if (out[i] < 0x20 || out[i] > 0x7e) return false;
    out[length] = '\0';
    return true;
}

bool ApplyCustomDisplayResolution(void* self, void* queue, int32_t quality) {
    if (!self || !queue || !g_request) return false;

    void* def = nullptr;
    if (!Field(self, g_layout.definition ? g_layout.definition : 0x58, def) || !def)
        return false;

    char defId[64] = {};
    if (!String(def, g_layout.definitionId ? g_layout.definitionId : 0x288, defId))
        return false;

    if (strncmp(defId, "swml_cd_", 8) != 0) {
        return false; // Stock monitor
    }

    int px = 0, py = 0;
    if (!GetCustomDisplayResolution(defId, px, py) || px <= 0 || py <= 0) {
        int w = 1, h = 1;
        if (sscanf_s(defId, "swml_cd_%dx%d", &w, &h) == 2) {
            px = w * 32;
            py = h * 32;
        } else {
            px = 32;
            py = 32;
        }
    }

    const uint32_t scale = (quality == 1) ? 1u : (quality == 3) ? 4u : 2u;
    auto* bytes = static_cast<uint8_t*>(self);

    uint32_t widthOff = g_layout.width ? g_layout.width : 0x4b8;
    uint32_t heightOff = g_layout.height ? g_layout.height : 0x4bc;
    uint32_t scaleOff = g_layout.scale ? g_layout.scale : 0x4c0;
    uint32_t videoOff = g_layout.video ? g_layout.video : 0x4c8;
    uint32_t overlayOff = g_layout.overlay ? g_layout.overlay : 0x4d8;

    *reinterpret_cast<uint32_t*>(bytes + widthOff) = static_cast<uint32_t>(px);
    *reinterpret_cast<uint32_t*>(bytes + heightOff) = static_cast<uint32_t>(py);
    *reinterpret_cast<uint32_t*>(bytes + scaleOff) = scale;

    CustomDisplayResourceHandle* videoHandle = reinterpret_cast<CustomDisplayResourceHandle*>(bytes + videoOff);
    CustomDisplayResourceHandle* overlayHandle = reinterpret_cast<CustomDisplayResourceHandle*>(bytes + overlayOff);

    // Queue video target: px * scale by py * scale, format 5, depth 1
    g_request(queue, px * static_cast<int32_t>(scale), py * static_cast<int32_t>(scale), 5, 1, videoHandle);
    // Queue overlay target: px by py, format 5, depth 1
    g_request(queue, px, py, 5, 1, overlayHandle);

    LogInfo("CDRES: Applied custom monitor %s (%dx%d, scale %ux) at monitor %p",
            defId, px, py, scale, self);
    return true;
}

void Write(uint32_t event, const char* phase, void* monitor, void* queue,
           int32_t quality, const CustomDisplaySnapshot& s, void* caller) {
    LogInfo("CDOBS event=%u phase=%s thread=%lu monitor=%p queue=%p quality=%d caller=%p valid=%d def=%p id=%s archive=%s pixels=%ux%u scale=%u video=%p/%p overlay=%p/%p",
        event, phase, GetCurrentThreadId(), monitor, queue, quality, caller, s.valid,
        s.definition, s.valid ? s.definitionId : "<unreadable>",
        s.valid ? s.archiveId : "<unreadable>", s.width, s.height, s.scale,
        s.video, s.videoManager, s.overlay, s.overlayManager);
}
// Diagnostics must not prevent/duplicate a native call, even on logging errors.
void Observe(uint32_t event, const char* phase, void* self, void* queue,
             int32_t quality, void* caller) noexcept {
    try {
        CustomDisplaySnapshot snapshot;
        ReadCustomDisplaySnapshot(self, g_layout, snapshot);
        Write(event, phase, self, queue, quality, snapshot, caller);
    } catch (...) { /* The original call remains unconditional. */ }
}
uint32_t Begin(void* caller) noexcept {
    uint32_t event = g_events.load(std::memory_order_relaxed);
    do {
        if (event >= kMaxEvents) return UINT32_MAX;
    } while (!g_events.compare_exchange_weak(event, event + 1, std::memory_order_relaxed));
    try {
        void* frames[12] = {};
        USHORT count = CaptureStackBackTrace(1, _countof(frames), frames, nullptr);
        char text[512] = {}; size_t used = 0;
        for (USHORT i = 0; i < count && used + 24 < sizeof(text); ++i) {
            uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
            if (address >= g_base && address - g_base < g_size) {
                int n = sprintf_s(text + used, sizeof(text) - used, "%s+0x%llx", used ? "," : "",
                                  static_cast<unsigned long long>(address - g_base));
                if (n > 0) used += size_t(n);
            }
        }
        LogInfo("CDOBS event=%u native_stack=%s caller=%p", event, text, caller);
    } catch (...) {}
    return event;
}
void Create(void* self, void* queue, int32_t quality) {
    void* caller = _ReturnAddress();
    uint32_t event = Begin(caller);
    if (event != UINT32_MAX) Observe(event, "create.before", self, queue, quality, caller);
    if (!ApplyCustomDisplayResolution(self, queue, quality))
        g_create(self, queue, quality); // Default stock path
    if (event != UINT32_MAX) Observe(event, "create.after", self, queue, quality, caller);
}
void Release(void* self, void* queue) {
    void* caller = _ReturnAddress();
    uint32_t event = Begin(caller);
    if (event != UINT32_MAX) Observe(event, "release.before", self, queue, 0, caller);
    g_release(self, queue); // Does not destroy the monitor; no wrapper read after.
    if (event != UINT32_MAX) Observe(event, "release.after", self, queue, 0, caller);
}
}

bool ReadCustomDisplaySnapshot(const void* monitor, const CustomDisplayLayout& l,
                               CustomDisplaySnapshot& out) {
    CustomDisplaySnapshot s;
    // Defensive bounds also protect use by offline tests and future callers.
    for (auto offset : {l.definition, l.width, l.height, l.scale, l.video, l.overlay,
                       l.definitionId, l.archiveId})
        if (!offset || offset >= 0x10000) return false;
    if (!Field(monitor, l.definition, s.definition) || !s.definition ||
        !Field(monitor, l.width, s.width) || !Field(monitor, l.height, s.height) ||
        !Field(monitor, l.scale, s.scale) ||
        !Field(monitor, l.video, s.video) ||
        !Field(monitor, size_t(l.video) + sizeof(void*), s.videoManager) ||
        !Field(monitor, l.overlay, s.overlay) ||
        !Field(monitor, size_t(l.overlay) + sizeof(void*), s.overlayManager) ||
        !String(s.definition, l.definitionId, s.definitionId) ||
        !String(s.definition, l.archiveId, s.archiveId)) return false;
    s.valid = true; out = s;
    return true;
}

bool InstallCustomDisplayObserver(uint8_t* base) {
    if (g_installed) return true;
    if (!IsSupportedEngineBuild(base)) return false;
    const auto result = discovery::DiscoverLoadedEngine(base);
    discovery::Image image;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!discovery::ParseImage(base, nt->OptionalHeader.SizeOfImage, image) ||
        !DiscoverCustomDisplayLayout(image, result, g_layout)) {
        LogError("CDOBS: complete layout/slot contracts unavailable; no observation hooks installed");
        return false;
    }
    InitializeCustomDisplayRegistry();
    auto** table = reinterpret_cast<void**>(base + result.Address(0xb70ea0));
    g_request = reinterpret_cast<CustomDisplayQueueTargetFn>(base + result.Address(0x20da70));
    g_monitorTable = table;

    SlotPatch patches[] = {
        {&table[g_layout.createSlot], base + result.Address(0x366010), reinterpret_cast<void*>(&Create), reinterpret_cast<void**>(&g_create)},
        {&table[g_layout.releaseSlot], base + result.Address(0x366100), reinterpret_cast<void*>(&Release), reinterpret_cast<void**>(&g_release)}
    };
    g_base = reinterpret_cast<uintptr_t>(base); g_size = image.size;
    if (!PatchSlots(patches, _countof(patches))) {
        LogError("CDOBS: slot conflict/protection failure; native slots left untouched");
        return false;
    }
    g_installed = true;
    LogInfo("CDOBS: Custom display resolution hooks active (createSlot=%u, releaseSlot=%u)",
        g_layout.createSlot, g_layout.releaseSlot);
    return true;
}

void UninstallCustomDisplayObserver() {
    if (!g_installed || !g_monitorTable || !g_create || !g_release) return;
    SlotPatch patches[] = {
        {reinterpret_cast<void**>(&reinterpret_cast<void**>(g_monitorTable)[g_layout.createSlot]), reinterpret_cast<void*>(&Create), reinterpret_cast<void*>(g_create), reinterpret_cast<void**>(&g_create)},
        {reinterpret_cast<void**>(&reinterpret_cast<void**>(g_monitorTable)[g_layout.releaseSlot]), reinterpret_cast<void*>(&Release), reinterpret_cast<void*>(g_release), reinterpret_cast<void**>(&g_release)}
    };
    PatchSlots(patches, _countof(patches));
    g_installed = false;
    LogInfo("CDOBS: Custom display resolution hooks uninstalled");
}
} // namespace swml
