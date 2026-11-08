#include "native_symbols.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "engine_bindings.h"
#include "engine_discovery.h"
#include "log.h"

namespace swml {
namespace {
#include "native_symbols.generated.h"

// Roles already certified by the reviewed binding table (reference RVAs).
struct ServiceRole {
    const char* name;
    uint32_t referenceRva;
};
constexpr ServiceRole kServiceRoles[] = {
    {"engine.alloc", 0x957478},
    {"engine.string.reserve", 0x17cb00},
    {"engine.string.free", 0x9b15a0},
    {"engine.localized", 0x20bfd0},
};

// Roles located by a retained masked instruction contract.
struct ContractRole {
    const char* name;
    uint32_t referenceRva;
    const unsigned char* bytes;
    const unsigned char* mask;
    size_t size;
};
constexpr ContractRole kContractRoles[] = {
    {"engine.component.lookup", kNativeSymbol_lookup_rva, kNativeSymbol_lookup_bytes,
     kNativeSymbol_lookup_mask, kNativeSymbol_lookup_size},
    {"engine.component.clear", kNativeSymbol_clear_rva, kNativeSymbol_clear_bytes,
     kNativeSymbol_clear_mask, kNativeSymbol_clear_size},
    {"engine.component.grow", kNativeSymbol_grow_rva, kNativeSymbol_grow_bytes,
     kNativeSymbol_grow_mask, kNativeSymbol_grow_size},
    {"engine.localization.register", kNativeSymbol_localization_rva,
     kNativeSymbol_localization_bytes, kNativeSymbol_localization_mask,
     kNativeSymbol_localization_size},
};

const ContractRole* ContractByName(const char* name) {
    if (!name) return nullptr;
    for (const auto& role : kContractRoles)
        if (strcmp(role.name, name) == 0) return &role;
    return nullptr;
}
const ServiceRole* ServiceByName(const char* name) {
    if (!name) return nullptr;
    for (const auto& role : kServiceRoles)
        if (strcmp(role.name, name) == 0) return &role;
    return nullptr;
}

std::mutex g_mutex;
uint8_t* g_base = nullptr;
bool g_initialized = false;
// role name -> absolute address (nullptr = resolved and refused, fail-closed).
std::map<std::string, const void*> g_cache;

bool ImageOf(uint8_t* base, discovery::Image& image) {
    if (!base) return false;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.SizeOfImage > 512u * 1024u * 1024u) return false;
        return discovery::ParseImage(base, nt->OptionalHeader.SizeOfImage, image);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr uint32_t kRoleNotFound = 0u;

const void* ResolveRole(const char* role) {
    if (!role || !g_base) return nullptr;
    // 1. Reviewed binding table: reference RVA mapped per certified build.
    if (const ServiceRole* service = ServiceByName(role)) {
        const uint32_t rva = EngineRva(g_base, service->referenceRva);
        if (!rva || !ValidateEngineBinding(g_base, service->referenceRva)) return nullptr;
        return g_base + rva;
    }
    // 2. Retained masked contract over the executable sections.
    if (!ContractByName(role)) return nullptr;
    discovery::Image image;
    if (!ImageOf(g_base, image)) return nullptr;
    uint32_t rva = kRoleNotFound;
    if (!FindNativeSymbolInImage(image, role, rva) || !rva) return nullptr;
    return g_base + rva;
}
}  // namespace

bool FindNativeSymbolInImage(const discovery::Image& image, const char* role, uint32_t& rva) {
    rva = kRoleNotFound;
    const ContractRole* spec = ContractByName(role);
    if (!spec || !spec->size || !image.base) return false;
    discovery::Contract contract = {};
    contract.key = spec->referenceRva;
    contract.start = spec->referenceRva;
    contract.size = static_cast<uint32_t>(spec->size);
    contract.name = spec->name;
    contract.bytes = reinterpret_cast<const char*>(spec->bytes);
    contract.mask = reinterpret_cast<const char*>(spec->mask);
    // Always scan: a unique full-body match is required on BOTH certified builds.
    const discovery::Symbol symbol = discovery::Find(image, contract, /*reviewedFastPath=*/false);
    if (symbol.status != discovery::Status::Found) return false;
    rva = symbol.rva;
    return true;
}

void InitializeNativeSymbols(uint8_t* base) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
    g_base = base;
    g_initialized = base != nullptr;
    if (!g_initialized) return;
    if (!EngineBuild(base)) {
        LogWarn("Native symbols: engine build is not reviewed; every role stays unresolved");
        return;
    }
    size_t resolved = 0, total = 0;
    auto resolve = [&](const char* role) {
        ++total;
        const void* address = ResolveRole(role);
        g_cache[role] = address;
        if (address) ++resolved;
    };
    for (const auto& service : kServiceRoles) resolve(service.name);
    for (const auto& contract : kContractRoles) resolve(contract.name);
    LogInfo("Native symbols: %zu/%zu generic engine role(s) resolved and body-validated",
            resolved, total);
}

void ShutdownNativeSymbols() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
    g_base = nullptr;
    g_initialized = false;
}

const void* FindNativeSymbol(const char* role) {
    if (!role) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || !g_base) return nullptr;
    auto it = g_cache.find(role);
    if (it != g_cache.end()) return it->second;
    // Unknown role requested after initialization: resolve once and remember the
    // failure so a mod cannot hammer the scanner.
    const void* address = ResolveRole(role);
    g_cache[role] = address;
    return address;
}

const char* const* NativeSymbolRoles(size_t* count) {
    static const char* roles[] = {
        "engine.alloc",
        "engine.string.reserve",
        "engine.string.free",
        "engine.localized",
        "engine.component.lookup",
        "engine.component.clear",
        "engine.component.grow",
        "engine.localization.register",
    };
    if (count) *count = sizeof(roles) / sizeof(roles[0]);
    return roles;
}
}  // namespace swml
