#include "engine_profile.h"
#include "engine_bindings.h"
#include <windows.h>
#include "log.h"

namespace swml {
namespace {
// All native API entries, builtin callbacks, and original hook targets used by
// native_lua.cpp. Fingerprints are selected centrally for the reviewed build;
// reference-build bytes must never be applied to a newer-build RVA.
constexpr uint32_t kLuaBindingKeys[] = {
    0x97920, 0x97a10, 0x98050, 0x982c0, 0x983d0, 0xabd60,
    0xba2e0, 0xba830, 0xbe4a0, 0x174f60, 0x17cb00, 0x1e3ee0,
    0x24b140, 0x24b650, 0x25e730, 0x25ed10, 0x2e3bc0, 0x2e3eb0,
    0x2e4140, 0x2e4460, 0x2e4620, 0x2e5ad0, 0x2e6050, 0x2e6b00,
    0x2e6f00, 0x2e70c0, 0x2e7280, 0x2e7440, 0x2e7600, 0x2e77c0,
    0x2e7980, 0x2e7b40, 0x2e7d00, 0x2e7ec0, 0x2e7f50, 0x2e7fe0,
    0x2e80c0, 0x2e8310, 0x2e86a0, 0x2e88b0, 0x2e8a80, 0x2e8c50,
    0x2e8e50, 0x2e9120, 0x2e9320, 0x2e95e0, 0x2e98c0, 0x2e99f0,
    0x2e9b20, 0x2e9c30, 0x63d980, 0x63f300, 0x63f420, 0x63f550,
    0x63fbc0, 0x9253e0, 0x957478, 0x9b15a0
};

bool Check(uint8_t* base) {
    __try {
        if (!IsSupportedEngineBuild(base)) return false;
        for (uint32_t key : kLuaBindingKeys) {
            if (!EngineRva(base, key) || !ValidateEngineBinding(base, key)) {
                LogError("Lua engine profile mismatch for key 0x%X; service disabled", key);
                return false;
            }
        }
        const uint32_t mcRva = EngineRva(base, 0xb6dac0);
        const uint32_t scriptRva = EngineRva(base, 0xb67728);
        const uint32_t helpRva = EngineRva(base, 0xae9a20);
        if (!mcRva || !scriptRva || !helpRva) return false;
        auto** mc = reinterpret_cast<void**>(base + mcRva);
        auto** script = reinterpret_cast<void**>(base + scriptRva);
        auto** help = reinterpret_cast<void**>(base + helpRva);
        // Preserve fail-closed conflict detection: never replace another hook.
        return mc[1] == base + EngineRva(base, 0x2e5ad0) &&
               mc[3] == base + EngineRva(base, 0x2e6050) &&
               script[1] == base + EngineRva(base, 0x63f550) &&
               script[3] == base + EngineRva(base, 0x63fbc0) &&
               help[1] == base + EngineRva(base, 0x63d980);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool ValidateLuaProfile(uint8_t* base) { return base && Check(base); }
}
