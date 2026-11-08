// Tests for the generic native primitives added to the SDK:
//   * role resolution from a retained masked contract (found / absent / ambiguous
//     / non-conforming body) — the exact fail-closed contract of the loader;
//   * bounded memory access refused on unvalidated addresses;
//   * inline hook + trampoline, idempotent restore, byte-identical prologue;
//   * refusal when the expected prologue bytes do not match.
// The test never executes game code: every hook target is a locally allocated
// executable page.
#include "../src/engine_discovery.h"
#include "../src/native_prims.h"
#include "../src/native_symbols.h"
#include "../include/sw_mod_sdk.h"

#include <windows.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace swml {
void LogError(const char*, ...) {}
void LogWarn(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
}  // namespace swml

namespace {
#include "../src/native_symbols.generated.h"

SwModHost g_host = {};

// Minimal but structurally valid x64 image: DOS + NT headers, one executable
// section. Enough for discovery::ParseImage and discovery::Find.
class SyntheticImage {
public:
    explicit SyntheticImage(size_t imageSize = 0x4000, size_t sectionSize = 0x1000)
        : data(imageSize, 0), size(imageSize), section(0x1000), sectionBytes(sectionSize) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(data.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(data.data() + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = static_cast<uint32_t>(size);
        nt->OptionalHeader.SizeOfHeaders = 0x400;
        nt->OptionalHeader.ImageBase = 0x140000000ull;
        auto* sectionHeader = IMAGE_FIRST_SECTION(nt);
        memcpy(sectionHeader->Name, ".text", 5);
        sectionHeader->Misc.VirtualSize = static_cast<uint32_t>(sectionBytes);
        sectionHeader->VirtualAddress = static_cast<uint32_t>(section);
        sectionHeader->SizeOfRawData = static_cast<uint32_t>(sectionBytes);
        sectionHeader->Characteristics = 0x60000020;  // CODE | EXECUTE | READ
    }
    uint8_t* at(uint32_t rva) { return data.data() + rva; }
    bool parsed(swml::discovery::Image& image) {
        return swml::discovery::ParseImage(data.data(), size, image);
    }

private:
    std::vector<uint8_t> data;
    size_t size;
    uint32_t section;
    size_t sectionBytes;
};

void PlaceContract(SyntheticImage& image, uint32_t offset,
                   const unsigned char* bytes, size_t size) {
    memcpy(image.at(0x1000 + offset), bytes, size);
}

void TestRoleContract() {
    SyntheticImage image;
    PlaceContract(image, 0x100, kNativeSymbol_lookup_bytes, kNativeSymbol_lookup_size);
    swml::discovery::Image parsed;
    assert(image.parsed(parsed));
    uint32_t rva = 0;
    assert(swml::FindNativeSymbolInImage(parsed, "engine.component.lookup", rva));
    assert(rva == 0x1100);
    // Unknown role and absent contract both fail closed.
    assert(!swml::FindNativeSymbolInImage(parsed, "engine.does.not.exist", rva));
    assert(!swml::FindNativeSymbolInImage(parsed, "engine.component.clear", rva));

    // A single changed fixed byte invalidates the whole contract.
    SyntheticImage altered;
    PlaceContract(altered, 0x100, kNativeSymbol_lookup_bytes, kNativeSymbol_lookup_size);
    altered.at(0x1100 + 2)[0] ^= 0xFF;
    swml::discovery::Image alteredImage;
    assert(altered.parsed(alteredImage));
    assert(!swml::FindNativeSymbolInImage(alteredImage, "engine.component.lookup", rva));

    // Two full-body matches are ambiguous: resolution is refused, never guessed.
    SyntheticImage duplicated;
    PlaceContract(duplicated, 0x100, kNativeSymbol_lookup_bytes, kNativeSymbol_lookup_size);
    PlaceContract(duplicated, 0x300, kNativeSymbol_lookup_bytes, kNativeSymbol_lookup_size);
    swml::discovery::Image duplicatedImage;
    assert(duplicated.parsed(duplicatedImage));
    assert(!swml::FindNativeSymbolInImage(duplicatedImage, "engine.component.lookup", rva));
    puts("PASS: role contract resolved / absent / altered / ambiguous (fail-closed)");
}

uint8_t* AllocateExecutable(size_t size, const uint8_t* code, size_t codeSize) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE));
    assert(page);
    memset(page, 0x90, size);
    if (code && codeSize) memcpy(page, code, codeSize);
    FlushInstructionCache(GetCurrentProcess(), page, size);
    return page;
}

using Fn = int (*)(void);

void TestMemoryValidation() {
    uint32_t value = 0x11223344;
    uint32_t read = 0;
    assert(swml::native::NativePrimitives()->readMemory(&g_host, &value, &read, sizeof(read)) == SW_OK);
    assert(read == value);
    const uint32_t written = 0xAABBCCDD;
    assert(swml::native::NativePrimitives()->writeMemory(&g_host, &value, &written, sizeof(written)) == SW_OK);
    assert(value == written);

    // Uncommitted / invalid addresses are refused before any write.
    const uint32_t patch = 0xDEADBEEF;
    assert(swml::native::NativePrimitives()->writeMemory(&g_host, reinterpret_cast<void*>(0x10), &patch,
                                                         sizeof(patch)) == SW_INVALID_ARGUMENT);
    assert(swml::native::NativePrimitives()->readMemory(&g_host, reinterpret_cast<void*>(0x10), &read,
                                                        sizeof(read)) == SW_INVALID_ARGUMENT);
    // Zero size and null pointers are refused.
    assert(swml::native::NativePrimitives()->writeMemory(&g_host, &value, &patch, 0) == SW_INVALID_ARGUMENT);
    assert(swml::native::NativePrimitives()->readMemory(&g_host, nullptr, &read, 4) == SW_INVALID_ARGUMENT);
    puts("PASS: bounded read/write, unvalidated address refused");
}

void TestHookAndRestore() {
    // target: mov eax,1 ; ret  (padded with NOPs so a 16-byte prologue covers
    // whole instructions).
    const uint8_t targetCode[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    const uint8_t detourCode[] = {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
    uint8_t* target = AllocateExecutable(64, targetCode, sizeof(targetCode));
    uint8_t* detour = AllocateExecutable(64, detourCode, sizeof(detourCode));
    uint8_t original[16];
    memcpy(original, target, sizeof(original));

    volatile Fn call = reinterpret_cast<Fn>(target);
    volatile Fn callDetour = reinterpret_cast<Fn>(detour);
    assert(call() == 1);
    assert(callDetour() == 2);

    void* trampoline = nullptr;
    SwHandle handle = 0;
    // Fail-closed: mismatching expected bytes are refused and nothing changes.
    uint8_t wrong[16];
    memcpy(wrong, original, sizeof(wrong));
    wrong[1] ^= 0xFF;
    assert(swml::native::NativePrimitives()->hookInline(&g_host, target, detour, wrong,
                                                        sizeof(wrong), &trampoline, &handle) == SW_CONFLICT);
    assert(memcmp(target, original, sizeof(original)) == 0);
    assert(handle == 0 && trampoline == nullptr);

    // Data pages are never patched.
    uint32_t dataPage = 0;
    assert(swml::native::NativePrimitives()->hookInline(&g_host, &dataPage, detour, nullptr, 16,
                                                        &trampoline, &handle) == SW_UNAVAILABLE);

    assert(swml::native::NativePrimitives()->hookInline(&g_host, target, detour, original,
                                                        sizeof(original), &trampoline, &handle) == SW_OK);
    assert(handle != 0 && trampoline != nullptr);
    assert(memcmp(target, original, sizeof(original)) != 0);
    assert(call() == 2);   // the hook redirects to the detour

    // Idempotent restore, byte-identical prologue.
    assert(swml::native::NativePrimitives()->hookRestore(&g_host, handle) == SW_OK);
    assert(memcmp(target, original, sizeof(original)) == 0);
    assert(call() == 1);
    assert(swml::native::NativePrimitives()->hookRestore(&g_host, handle) == SW_OK);
    assert(memcmp(target, original, sizeof(original)) == 0);

    // Unknown handle fails closed.
    assert(swml::native::NativePrimitives()->hookRestore(&g_host, 0x12345678) == SW_NOT_FOUND);

    // Forwarding through the trampoline reproduces the original prologue.
    trampoline = nullptr;
    handle = 0;
    assert(swml::native::NativePrimitives()->hookInline(&g_host, target, detour, original,
                                                        sizeof(original), &trampoline, &handle) == SW_OK);
    auto callTrampoline = reinterpret_cast<Fn>(trampoline);
    // The copied prologue ends with `ret`, so the trampoline returns 1 without
    // reaching its jump back.
    assert(callTrampoline() == 1);
    assert(swml::native::NativePrimitives()->hookRestore(&g_host, handle) == SW_OK);

    VirtualFree(target, 0, MEM_RELEASE);
    VirtualFree(detour, 0, MEM_RELEASE);
    puts("PASS: inline hook, trampoline, contract refusal, idempotent byte-identical restore");
}

void TestCodeAllocation() {
    // Scratch executable memory near a target is reachable by a rel32 jump.
    auto* page = AllocateExecutable(64, nullptr, 0);
    void* block = swml::native::NativePrimitives()->codeAllocNear(&g_host, page, 32);
    assert(block != nullptr);
    const uint8_t pattern[] = {0x90, 0x90, 0xC3};
    assert(swml::native::NativePrimitives()->writeMemory(&g_host, block, pattern, sizeof(pattern)) == SW_OK);
    swml::native::NativePrimitives()->codeFree(&g_host, block);
    VirtualFree(page, 0, MEM_RELEASE);
    puts("PASS: executable scratch allocation near a target + release");
}
}  // namespace

int main() {
    // No owner registry in this executable: accept any non-null owner.
    swml::native::SetNativeOwnerCheck(nullptr);
    swml::native::InitializeNativePrimitives(nullptr);
    g_host.sdkVersion = SW_MOD_SDK_VERSION;
    g_host.modId = "native_prims_tests";

    TestRoleContract();
    TestMemoryValidation();
    TestHookAndRestore();
    TestCodeAllocation();
    puts("PASS: native primitive suites");
    return 0;
}
