#include "engine_bindings.h"
#include "engine_discovery.h"
#include "log.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <cstdio>
#include <vector>

namespace swml {
namespace {
struct Binding { uint32_t key, oldRva, newRva, displacement, size; const char *oldBytes, *newBytes; };
struct BindingSlot { uint32_t slot, key; };
struct BindingTable { uint32_t key, newRva; const BindingSlot* slots; size_t count; };
struct BindingWindow { uint32_t oldRva, newRva, size; const char *oldBytes, *newBytes; };
#include "engine_bindings.generated.h"
int Build(uint8_t* base) {
    if (!base) return 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
        if (nt->FileHeader.TimeDateStamp == 0x6a392154 && nt->OptionalHeader.SizeOfImage == 0xd89000) return 1;
        if (nt->FileHeader.TimeDateStamp == 0x6a9a9e20 && nt->OptionalHeader.SizeOfImage == 0xd8b000) return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
bool SameBytes(uint8_t* base, uint32_t start, const char* expected, size_t size) {
    __try { return memcmp(base + start, expected, size) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SlotMatches(uint8_t* base, uint32_t table, uint32_t slot, uint32_t target) {
    __try { return reinterpret_cast<void**>(base + table)[slot] == base + target; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool DiskHash(const wchar_t* path, const char* expected) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, written = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0) >= 0;
    std::vector<UCHAR> object(objectSize), buffer(1024 * 1024);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    UCHAR digest[32] = {};
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    char hex[65] = {};
    if (ok) { for (size_t i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", unsigned(digest[i])); ok = strcmp(hex, expected) == 0; }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}
}
bool IsSupportedEngineBuild(uint8_t* base) { return Build(base) != 0; }
uint32_t EngineRva(uint8_t* base, uint32_t key) {
    int build = Build(base);
    if (!build) return 0;
    for (const auto& b : kEngineBindings) if (b.key == key) return build == 1 ? b.oldRva : b.newRva;
    for (const auto& t : kEngineBindingTables) if (t.key == key) return build == 1 ? t.key : t.newRva;
    if (key == 0xa925f0) {
        discovery::Image image;
        if (!discovery::ParseImage(base, build == 1 ? 0xd89000 : 0xd8b000, image)) return 0;
        return discovery::FindImport(image, "OPENGL32.dll", "glDrawElements");
    }
    return 0; // Never reinterpret an unknown identity as an address.
}
bool ValidateEngineBinding(uint8_t* base, uint32_t key) {
    int build = Build(base);
    if (!build) return false;
    if (key == 0xa925f0) return EngineRva(base, key) != 0;
    for (const auto& b : kEngineBindings) if (b.key == key) {
        uint32_t rva = build == 1 ? b.oldRva : b.newRva;
        return rva >= b.displacement && SameBytes(base, rva - b.displacement, build == 1 ? b.oldBytes : b.newBytes, b.size);
    }
    for (const auto& t : kEngineBindingTables) if (t.key == key) {
        uint32_t table = build == 1 ? t.key : t.newRva;
        for (size_t i = 0; i < t.count; ++i) {
            uint32_t rva = EngineRva(base, t.slots[i].key);
            if (!rva || !SlotMatches(base, table, t.slots[i].slot, rva) || !ValidateEngineBinding(base, t.slots[i].key)) return false;
        }
        return true;
    }
    return false;
}
bool ValidateEngineLayouts(uint8_t* base) {
    int build = Build(base);
    if (!build) return false;
    for (const auto& w : kEngineLayoutWindows)
        if (!SameBytes(base, build == 1 ? w.oldRva : w.newRva, build == 1 ? w.oldBytes : w.newBytes, w.size)) return false;
    return true;
}
bool AuthenticateEngineExecutable(uint8_t* base) {
    int build = Build(base);
    if (!build || base != reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))) return false;
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path)) return false;
    LogDiscoveryProgress("Reviewed bindings", 0, 1, "authenticating executable SHA-256 (unknown files remain blocked)", false);
    const char* hash = build == 1 ? "55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3" :
        "f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699";
    if (!DiskHash(path, hash)) { LogError("Reviewed bindings: executable SHA-256 rejected; no hooks installed"); return false; }
    if (!ValidateReviewedEngineMemory(base)) return false;
    LogInfo("Engine bindings: authenticated reviewed build %d; all hook dependencies validated before writes", build);
    return true;
}
bool ValidateReviewedEngineMemory(uint8_t* base) {
    if (!Build(base)) return false;
    if (!ValidateEngineLayouts(base)) { LogError("Reviewed layout/ABI witness changed; no hooks installed"); return false; }
    size_t total = _countof(kEngineBindings) + _countof(kEngineBindingTables), n = 0;
    for (const auto& b : kEngineBindings) {
        if (!ValidateEngineBinding(base, b.key)) { LogError("Reviewed binding 0x%X changed in memory; no hooks installed", b.key); return false; }
        char detail[128]; snprintf(detail, sizeof(detail), "code identity 0x%X -> RVA 0x%X validated", b.key, EngineRva(base, b.key));
        LogDiscoveryProgress("Reviewed bindings", ++n, total, detail, false);
    }
    for (const auto& t : kEngineBindingTables) {
        if (!ValidateEngineBinding(base, t.key)) { LogError("Reviewed vtable 0x%X changed; no hooks installed", t.key); return false; }
        char detail[128]; snprintf(detail, sizeof(detail), "native vtable 0x%X validated", t.key);
        LogDiscoveryProgress("Reviewed bindings", ++n, total, detail, false);
    }
    return true;
}
}
