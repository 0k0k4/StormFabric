#include "native_prims.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine_bindings.h"
#include "engine_discovery.h"
#include "log.h"
#include "native_symbols.h"

namespace swml {
namespace native {
namespace {

// ---------------------------------------------------------------------------
// Engine entry points resolved through the discovery profile (never hardcoded).
// ---------------------------------------------------------------------------
using EngineNewFn = void* (*)(size_t);
using EngineReserveFn = void (*)(SwEngineString*, uint32_t);
using EngineFreeFn = void (*)(void*);

std::mutex g_mutex;
uint8_t* g_base = nullptr;
bool g_shutdown = false;
EngineNewFn g_engineNew = nullptr;
EngineReserveFn g_engineReserve = nullptr;
EngineFreeFn g_engineFree = nullptr;
bool (*g_ownerCheck)(const SwModHost*) = nullptr;

constexpr size_t kMaxTransfer = 256u * 1024u * 1024u;
constexpr size_t kMaxPattern = 512u;
constexpr size_t kMaxPrologue = 32u;
constexpr size_t kCodeBlockBytes = 96u;

// SEH boundary kept in a function without C++ objects (C2712).
bool SafeMemcpy(void* destination, const void* source, size_t size) {
    __try {
        memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ValidOwner(const SwModHost* owner) {
    if (!owner) return false;
    return g_ownerCheck ? g_ownerCheck(owner) : true;
}

// ---------------------------------------------------------------------------
// Bounded, page-validated memory access.
// ---------------------------------------------------------------------------
struct PageRun {
    void* base;
    size_t size;
    DWORD protection;
    bool executable;
};

DWORD PageSize() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize ? info.dwPageSize : 4096;
}

// Splits [address, address+size) into committed runs of identical protection.
// Returns false when any page is not committed, is PAGE_NOACCESS/PAGE_GUARD, or
// is not readable (and not writable for writes). Nothing is written here.
bool InspectRange(const void* address, size_t size, bool forWrite, std::vector<PageRun>& runs) {
    runs.clear();
    if (!address || !size || size > kMaxTransfer) return false;
    const uintptr_t pageSize = PageSize();
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = start + size;
    if (end < start) return false;
    uintptr_t cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT) return false;
        const DWORD protection = info.Protect;
        if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        const bool writable = (protection & (PAGE_READWRITE | PAGE_WRITECOPY |
                                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (forWrite && !writable && !(protection & (PAGE_READONLY | PAGE_EXECUTE_READ |
                                                     PAGE_EXECUTE))) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= cursor) return false;
        uintptr_t chunkEnd = (std::min)(end, regionEnd);
        // The caller may have passed an unaligned address; only whole pages are
        // queried, so runs are tracked per page.
        const uintptr_t pageStart = cursor & ~(pageSize - 1);
        const uintptr_t pageEnd = (chunkEnd + pageSize - 1) & ~(pageSize - 1);
        const size_t runSize = static_cast<size_t>((std::min)(pageEnd, end) - pageStart);
        PageRun run{reinterpret_cast<void*>(pageStart), runSize, protection,
                    (protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                   PAGE_EXECUTE_WRITECOPY)) != 0};
        if (!runs.empty() && runs.back().base == run.base) {
            runs.back().size = (std::max)(runs.back().size, run.size);
        } else {
            runs.push_back(run);
        }
        cursor = pageStart + runSize;
    }
    return !runs.empty();
}

enum class WriteOutcome { kOk = 0, kInvalid, kIoError };

WriteOutcome WriteCodeRange(void* address, const void* data, size_t size) {
    if (!data) return WriteOutcome::kInvalid;
    std::vector<PageRun> runs;
    if (!InspectRange(address, size, /*forWrite=*/true, runs)) return WriteOutcome::kInvalid;
    const uintptr_t rangeStart = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = rangeStart + size;
    bool restored = true;
    for (const PageRun& run : runs) {
        uintptr_t runStart = (std::max)(reinterpret_cast<uintptr_t>(run.base), rangeStart);
        uintptr_t runEnd = (std::min)(reinterpret_cast<uintptr_t>(run.base) + run.size, end);
        if (runEnd <= runStart) continue;
        DWORD old = 0;
        const DWORD target = run.executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        if (!VirtualProtect(reinterpret_cast<void*>(runStart), runEnd - runStart, target, &old))
            return WriteOutcome::kIoError;
        if (!SafeMemcpy(reinterpret_cast<void*>(runStart),
                        static_cast<const uint8_t*>(data) + (runStart - rangeStart),
                        runEnd - runStart)) {
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(runStart), runEnd - runStart, old, &ignored);
            return WriteOutcome::kIoError;
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(runStart),
                              runEnd - runStart);
        DWORD ignored = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(runStart), runEnd - runStart, old, &ignored)) {
            restored = false;
            LogError("Native primitives: protection restore failed at %p", (void*)runStart);
        }
    }
    return restored ? WriteOutcome::kOk : WriteOutcome::kIoError;
}

// ---------------------------------------------------------------------------
// Inline hooks.
// ---------------------------------------------------------------------------
struct HookRecord {
    SwHandle handle = 0;
    const SwModHost* owner = nullptr;
    uint8_t* target = nullptr;
    uint8_t* trampoline = nullptr;
    size_t prologue = 0;
    uint8_t saved[kMaxPrologue] = {};
    bool active = false;
};

// Insertion order is the restore order (reverse).
std::vector<std::unique_ptr<HookRecord>> g_hooks;
SwHandle g_nextHandle = 0x5300000000000001ull;

struct CodeBlock {
    const SwModHost* owner;
    void* block;
};
std::vector<CodeBlock> g_codeBlocks;

size_t BuildAbsoluteJump(uint8_t* out, size_t capacity, const void* target) {
    if (!out || capacity < 14 || !target) return 0;
    memset(out, 0x90, capacity);
    out[0] = 0xFF;
    out[1] = 0x25;
    *reinterpret_cast<int32_t*>(out + 2) = 0;
    *reinterpret_cast<uint64_t*>(out + 6) = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(target));
    return 14;
}

// Builds the jump written at the hook target. Prefers the 14-byte RIP-indirect
// form (works across the whole 64-bit address space); falls back to a 5-byte
// relative jump only when the caller's prologue is shorter and the detour is in
// range.
size_t BuildHookPatch(uint8_t* out, size_t prologue, const void* target, const void* detour) {
    if (!out || !target || !detour) return 0;
    if (prologue >= 14) return BuildAbsoluteJump(out, prologue, detour);
    if (prologue < 5) return 0;
    const int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(detour)) -
                          static_cast<int64_t>(reinterpret_cast<uintptr_t>(target) + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    memset(out, 0x90, prologue);
    out[0] = 0xE9;
    const int32_t displacement = static_cast<int32_t>(delta);
    memcpy(out + 1, &displacement, 4);
    return 5;
}

bool PrologueMatches(const uint8_t* target, const void* expected, size_t size) {
    if (!expected) return true;
    __try {
        return memcmp(target, expected, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HookRecord* FindHook(SwHandle handle) {
    for (auto& record : g_hooks)
        if (record && record->handle == handle) return record.get();
    return nullptr;
}

void ReleaseCodeBlocks(const SwModHost* owner) {
    for (auto it = g_codeBlocks.begin(); it != g_codeBlocks.end();) {
        if (it->owner != owner) {
            ++it;
            continue;
        }
        VirtualFree(it->block, 0, MEM_RELEASE);
        it = g_codeBlocks.erase(it);
    }
}

void RestoreRecords(std::vector<HookRecord*>& records) {
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        HookRecord* record = *it;
        if (!record || !record->active) continue;
        if (WriteCodeRange(record->target, record->saved, record->prologue) != WriteOutcome::kOk) {
            LogError("Native primitives: prologue restore failed at %p (handle 0x%llx)",
                     (void*)record->target, (unsigned long long)record->handle);
            continue;
        }
        record->active = false;
    }
}

// ---------------------------------------------------------------------------
// Signature scanning (shared discovery::Find mechanism).
// ---------------------------------------------------------------------------
discovery::Image g_image;
bool g_imageValid = false;

bool EnsureImage() {
    if (g_imageValid) return true;
    if (!g_base) return false;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.SizeOfImage > 512u * 1024u * 1024u) return false;
        g_imageValid = discovery::ParseImage(g_base, nt->OptionalHeader.SizeOfImage, g_image);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_imageValid = false;
    }
    return g_imageValid;
}

// ---------------------------------------------------------------------------
// SDK entry points.
// ---------------------------------------------------------------------------
const void* FindSymbolPrim(const SwModHost* owner, const char* role) {
    if (!ValidOwner(owner)) return nullptr;
    return FindNativeSymbol(role);
}

SwResult FindSignaturePrim(const SwModHost* owner, const void* bytes, const void* mask,
                           size_t size, const void** address) {
    if (!ValidOwner(owner) || !bytes || !mask || !address) return SW_INVALID_ARGUMENT;
    if (size < 8 || size > kMaxPattern) return SW_INVALID_ARGUMENT;
    *address = nullptr;
    const auto* pattern = static_cast<const uint8_t*>(bytes);
    const auto* wildcards = static_cast<const uint8_t*>(mask);
    for (size_t i = 0; i < size; ++i)
        if (wildcards[i] != 0x00 && wildcards[i] != 0xFF) return SW_INVALID_ARGUMENT;
    if (!EnsureImage()) return SW_UNAVAILABLE;
    discovery::Contract contract = {};
    contract.key = 0;
    contract.start = 0;
    contract.size = static_cast<uint32_t>(size);
    contract.name = "mod signature";
    contract.bytes = reinterpret_cast<const char*>(pattern);
    contract.mask = reinterpret_cast<const char*>(wildcards);
    const discovery::Symbol symbol = discovery::Find(g_image, contract, /*reviewedFastPath=*/false);
    if (symbol.status == discovery::Status::Found) {
        *address = g_base + symbol.rva;
        return SW_OK;
    }
    return symbol.status == discovery::Status::Ambiguous ? SW_CONFLICT : SW_NOT_FOUND;
}

SwResult ReadMemoryPrim(const SwModHost* owner, const void* address, void* out, size_t size) {
    if (!ValidOwner(owner) || !address || !out || !size) return SW_INVALID_ARGUMENT;
    std::vector<PageRun> runs;
    if (!InspectRange(address, size, /*forWrite=*/false, runs)) return SW_INVALID_ARGUMENT;
    return SafeMemcpy(out, address, size) ? SW_OK : SW_IO_ERROR;
}

SwResult WriteMemoryPrim(const SwModHost* owner, void* address, const void* data, size_t size) {
    if (!ValidOwner(owner) || !address || !data || !size) return SW_INVALID_ARGUMENT;
    const WriteOutcome outcome = WriteCodeRange(address, data, size);
    return outcome == WriteOutcome::kOk ? SW_OK : (outcome == WriteOutcome::kInvalid ? SW_INVALID_ARGUMENT : SW_IO_ERROR);
}

SwResult HookInlinePrim(const SwModHost* owner, void* target, void* detour,
                        const void* expectedBytes, size_t prologueBytes,
                        void** trampoline, SwHandle* handle) {
    if (!ValidOwner(owner) || !target || !detour) return SW_INVALID_ARGUMENT;
    if (prologueBytes < 5 || prologueBytes > kMaxPrologue) return SW_INVALID_ARGUMENT;
    if (handle) *handle = 0;
    if (trampoline) *trampoline = nullptr;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutdown) return SW_UNAVAILABLE;

    uint8_t* site = static_cast<uint8_t*>(target);
    std::vector<PageRun> runs;
    if (!InspectRange(site, prologueBytes, /*forWrite=*/true, runs)) return SW_INVALID_ARGUMENT;
    for (const PageRun& run : runs)
        if (!run.executable) return SW_UNAVAILABLE;   // never patch data pages
    // Fail-closed contract: the caller's validated bytes must still be there.
    if (!PrologueMatches(site, expectedBytes, prologueBytes)) return SW_CONFLICT;

    auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCodeBlockBytes,
                                                     MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE));
    if (!block) return SW_UNAVAILABLE;
    memcpy(block, site, prologueBytes);
    if (!BuildAbsoluteJump(block + prologueBytes, kCodeBlockBytes - prologueBytes,
                           site + prologueBytes)) {
        VirtualFree(block, 0, MEM_RELEASE);
        return SW_UNAVAILABLE;
    }
    FlushInstructionCache(GetCurrentProcess(), block, kCodeBlockBytes);

    uint8_t patch[kMaxPrologue] = {};
    const size_t patchSize = BuildHookPatch(patch, prologueBytes, site, detour);
    if (!patchSize) {
        VirtualFree(block, 0, MEM_RELEASE);
        return SW_UNAVAILABLE;
    }

    auto record = std::make_unique<HookRecord>();
    record->handle = g_nextHandle++;
    record->owner = owner;
    record->target = site;
    record->trampoline = block;
    record->prologue = prologueBytes;
    memcpy(record->saved, site, prologueBytes);

    // Publish the written instruction tail first, then the jump opcode, so a
    // concurrently running thread can never decode a half-written jump.
    if (patchSize >= 14) {
        if (WriteCodeRange(site + 2, patch + 2, patchSize - 2) != WriteOutcome::kOk ||
            WriteCodeRange(site, patch, 2) != WriteOutcome::kOk) {
            VirtualFree(block, 0, MEM_RELEASE);
            return SW_IO_ERROR;
        }
    } else if (WriteCodeRange(site, patch, patchSize) != WriteOutcome::kOk) {
        VirtualFree(block, 0, MEM_RELEASE);
        return SW_IO_ERROR;
    }
    record->active = true;
    if (trampoline) *trampoline = block;
    if (handle) *handle = record->handle;
    g_hooks.push_back(std::move(record));
    LogInfo("Native primitives: inline hook at %p -> %p (%zu-byte prologue, trampoline %p)",
            (void*)site, detour, prologueBytes, (void*)block);
    return SW_OK;
}

SwResult HookRestorePrim(const SwModHost* owner, SwHandle handle) {
    if (!ValidOwner(owner) || !handle) return SW_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    HookRecord* record = FindHook(handle);
    if (!record) return SW_NOT_FOUND;
    if (record->owner != owner) return SW_INVALID_OWNER;
    if (!record->active) return SW_OK;   // idempotent
    if (WriteCodeRange(record->target, record->saved, record->prologue) != WriteOutcome::kOk)
        return SW_IO_ERROR;
    record->active = false;
    LogInfo("Native primitives: inline hook at %p restored (handle 0x%llx)",
            (void*)record->target, (unsigned long long)handle);
    return SW_OK;
}

void* GameAllocPrim(const SwModHost* owner, size_t size) {
    if (!ValidOwner(owner) || !g_engineNew || !size) return nullptr;
    if (size > kMaxTransfer) return nullptr;
    return g_engineNew(size);
}

void GameFreePrim(const SwModHost* owner, void* block) {
    if (!ValidOwner(owner) || !g_engineFree || !block) return;
    g_engineFree(block);
}

SwResult StringMakePrim(const SwModHost* owner, SwEngineString* out, const char* utf8) {
    if (!ValidOwner(owner) || !out || !utf8) return SW_INVALID_ARGUMENT;
    if (!g_engineReserve) return SW_UNAVAILABLE;
    const size_t length = strlen(utf8);
    if (length > 1024u * 1024u) return SW_INVALID_ARGUMENT;
    out->data = nullptr;
    out->len = static_cast<uint32_t>(length);
    out->cap = 0;
    g_engineReserve(out, static_cast<uint32_t>(length + 1));
    if (!out->data) return SW_IO_ERROR;
    memcpy(out->data, utf8, length + 1);
    return SW_OK;
}

void StringFreePrim(const SwModHost* owner, SwEngineString* string) {
    if (!ValidOwner(owner) || !string) return;
    if (string->data && g_engineFree) g_engineFree(string->data);
    string->data = nullptr;
    string->len = 0;
    string->cap = 0;
}

void* CodeAllocNearPrim(const SwModHost* owner, const void* target, size_t size) {
    if (!ValidOwner(owner) || !target || !size) return nullptr;
    if (size > 1024u * 1024u) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutdown) return nullptr;
    const uintptr_t granularity = 0x10000ull;
    const uintptr_t base = reinterpret_cast<uintptr_t>(target) & ~(granularity - 1);
    void* block = nullptr;
    for (int step = 1; step <= 64 && !block; ++step) {
        for (int sign : {-1, 1}) {
            const uintptr_t candidate = base + static_cast<uintptr_t>(sign) * step * 0x400000ull;
            if (candidate < granularity) continue;
            block = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (block) break;
        }
    }
    if (!block) block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_EXECUTE_READWRITE);
    if (!block) return nullptr;
    g_codeBlocks.push_back({owner, block});
    return block;
}

void CodeFreePrim(const SwModHost* owner, void* block) {
    if (!ValidOwner(owner) || !block) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_codeBlocks.begin(); it != g_codeBlocks.end(); ++it) {
        if (it->block != block) continue;
        if (it->owner != owner) return;   // never free another owner's block
        VirtualFree(block, 0, MEM_RELEASE);
        g_codeBlocks.erase(it);
        return;
    }
}

uint32_t EngineBuildPrim(const SwModHost* owner) {
    if (!ValidOwner(owner) || !g_base) return 0;
    return EngineBuild(g_base);
}

const SwNativeApi g_api = {
    sizeof(SwNativeApi),
    SW_NATIVE_API_VERSION,
    &FindSymbolPrim,
    &FindSignaturePrim,
    &ReadMemoryPrim,
    &WriteMemoryPrim,
    &HookInlinePrim,
    &HookRestorePrim,
    &GameAllocPrim,
    &GameFreePrim,
    &StringMakePrim,
    &StringFreePrim,
    &CodeAllocNearPrim,
    &CodeFreePrim,
    &EngineBuildPrim,
};

}  // namespace

void InitializeNativePrimitives(uint8_t* base) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_base = base;
    g_shutdown = false;
    g_imageValid = false;
    g_engineNew = nullptr;
    g_engineReserve = nullptr;
    g_engineFree = nullptr;
    InitializeNativeSymbols(base);
    if (!base) return;
    g_engineNew = reinterpret_cast<EngineNewFn>(const_cast<void*>(FindNativeSymbol("engine.alloc")));
    g_engineReserve = reinterpret_cast<EngineReserveFn>(
        const_cast<void*>(FindNativeSymbol("engine.string.reserve")));
    g_engineFree = reinterpret_cast<EngineFreeFn>(
        const_cast<void*>(FindNativeSymbol("engine.string.free")));
    LogInfo("Native primitives v%u initialized (allocator %s, strings %s)",
            SW_NATIVE_API_VERSION, g_engineNew ? "validated" : "unavailable",
            (g_engineReserve && g_engineFree) ? "validated" : "unavailable");
}

const SwNativeApi* NativePrimitives() { return &g_api; }

void ReleaseNativeOwner(const SwModHost* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<HookRecord*> mine;
    for (auto& record : g_hooks)
        if (record && record->owner == owner && record->active) mine.push_back(record.get());
    if (!mine.empty())
        LogInfo("Native primitives: restoring %zu hook(s) for an unloading mod", mine.size());
    RestoreRecords(mine);
    ReleaseCodeBlocks(owner);
    // Keep records (trampolines stay mapped for threads still inside them) but
    // drop the owner identity so a reused host address cannot resurrect them.
    for (auto& record : g_hooks)
        if (record && record->owner == owner) record->owner = nullptr;
}

void ShutdownNativePrimitives() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutdown) return;
    g_shutdown = true;
    std::vector<HookRecord*> all;
    for (auto& record : g_hooks)
        if (record && record->active) all.push_back(record.get());
    // Reverse installation order: mod hooks first, then anything installed later.
    RestoreRecords(all);
    for (auto& block : g_codeBlocks) VirtualFree(block.block, 0, MEM_RELEASE);
    g_codeBlocks.clear();
    g_hooks.clear();
    ShutdownNativeSymbols();
    g_base = nullptr;
    g_imageValid = false;
}

void SetNativeOwnerCheck(bool (*check)(const SwModHost*)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ownerCheck = check;
}

}  // namespace native
}  // namespace swml
