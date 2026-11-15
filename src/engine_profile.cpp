#include "engine_profile.h"
#include "engine_bindings.h"
#include <windows.h>
#include <cstring>
#include "log.h"

namespace swml {
namespace {
// Core custom-library service keys: Lua primitives, string/allocator helpers,
// table extras and the microcontroller VM lifecycle originals used by native_lua.cpp
// regardless of the optional QoL state. Fingerprints are selected centrally for the
// reviewed build; reference-build bytes must never be applied to a newer-build RVA.
constexpr uint32_t kCoreLuaKeys[] = {
    0x17cb00, 0x957478, 0x9b15a0,             // EStr reserve/free and op_new (ui+lua shared)
    0x097920, 0x097a10, 0x098050, 0x0982c0,   // index2addr, checkstack, pushcclosure, rawgeti
    0x0983d0, 0x0ba830, 0x0ba2e0, 0x0be4a0,   // setfield, newtable, resizearray, tonumber
    0x1e3ee0,                                 // register-library helper for the server table
    0x2e5ad0, 0x2e6050,                       // mc Lua vtable slot originals (VM lifecycle)
    0xb6dac0                                  // c_microprocessor_lua_script vtable
};

// Optional QoL/editor keys: consumed only by the editor vtable hooks, the enriched
// input/output/screen/math tables and the timeout unlock. A missing or ambiguous
// editor target disables QoL but leaves the core Lua service available.
constexpr uint32_t kQolEditorKeys[] = {
    0x0abd60, 0x174f60, 0x2e6b00,             // luaopen_math, monotonic ms, timeout hook
    0x2e3bc0, 0x2e3eb0, 0x2e4140, 0x2e4460,   // native draw helpers used by QoL screen.*
    0x2e4620,
    0x2e6f00, 0x2e70c0, 0x2e7280, 0x2e7440,   // screen.set/map colors
    0x2e7600, 0x2e77c0, 0x2e7980, 0x2e7b40,
    0x2e7d00, 0x2e7ec0, 0x2e7f50, 0x2e7fe0,
    0x2e80c0, 0x2e8310, 0x2e86a0, 0x2e88b0,   // drawText, drawTextBox, drawLine, circles
    0x2e8a80, 0x2e8c50, 0x2e8e50, 0x2e9120,
    0x2e9320, 0x2e95e0, 0x2e98c0, 0x2e99f0,   // drawTriangle(F), output.setBool/Number
    0x2e9b20, 0x2e9c30,                       // input.getBool/getNumber
    0x24b140, 0x24b650, 0x25e730, 0x25ed10,   // editor scroll/editbox helpers
    0x63f300, 0x63f420, 0x63f550, 0x63fbc0,   // editor help/heading/desc and slot originals
    0x63d980, 0x9253e0,                       // help tab slot original, scroll add (ui+editor)
    0xb67728, 0xae9a20                        // script/help tab vtables
};

bool CheckKeys(uint8_t* base, const uint32_t* keys, size_t count) {
    __try {
        if (!IsSupportedEngineBuild(base)) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!EngineRva(base, keys[i]) || !ValidateEngineBinding(base, keys[i])) {
                LogError("Lua engine profile mismatch for key 0x%X; service disabled", keys[i]);
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Check(uint8_t* base) {
    if (!CheckKeys(base, kCoreLuaKeys, _countof(kCoreLuaKeys))) return false;
    __try {
        const uint32_t mcRva = EngineRva(base, 0xb6dac0);
        if (!mcRva) return false;
        auto** mc = reinterpret_cast<void**>(base + mcRva);
        // Preserve fail-closed conflict detection: never replace another hook.
        return mc[1] == base + EngineRva(base, 0x2e5ad0) &&
               mc[3] == base + EngineRva(base, 0x2e6050);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CheckQolEditor(uint8_t* base) {
    if (!CheckKeys(base, kQolEditorKeys, _countof(kQolEditorKeys))) return false;
    __try {
        const uint32_t scriptRva = EngineRva(base, 0xb67728);
        const uint32_t helpRva = EngineRva(base, 0xae9a20);
        if (!scriptRva || !helpRva) return false;
        auto** script = reinterpret_cast<void**>(base + scriptRva);
        auto** help = reinterpret_cast<void**>(base + helpRva);
        // Preserve fail-closed conflict detection: never replace another hook.
        return script[1] == base + EngineRva(base, 0x63f550) &&
               script[3] == base + EngineRva(base, 0x63fbc0) &&
               help[1] == base + EngineRva(base, 0x63d980);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

namespace {
#include "addon_lua_profile.generated.h"
}
bool ResolveAddonLuaProfile(uint8_t* base, AddonLuaProfile* out) {
    if (!base || !out) return false;
    __try {
        if (EngineBuild(base) != 2) return false; // newer reviewed build only
        if (memcmp(base + kAddonLuaClose, kAddonLuaCloseBytes, sizeof(kAddonLuaCloseBytes)) != 0 ||
            memcmp(base + kAddonLuaInit, kAddonLuaInitBytes, sizeof(kAddonLuaInitBytes)) != 0) return false;
        auto** table = reinterpret_cast<void**>(base + kAddonLuaVtable);
        // Fail-closed conflict detection: never replace another hook.
        if (table[1] != base + kAddonLuaClose || table[3] != base + kAddonLuaInit) return false;
        *out = {table, base + kAddonLuaClose, base + kAddonLuaInit};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* ResolveLuaPcallk(uint8_t* base) {
    if (!base) return nullptr;
    __try {
        if (EngineBuild(base) != 2) return nullptr; // newer reviewed build only
        return memcmp(base + kLuaPcallk, kLuaPcallkBytes, sizeof(kLuaPcallkBytes)) == 0 ? base + kLuaPcallk : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool ValidateAddonLuaProfile(uint8_t* base) { AddonLuaProfile ignored; return ResolveAddonLuaProfile(base, &ignored); }
bool ValidateLuaProfile(uint8_t* base) { return base && Check(base); }
bool ValidateQolEditorProfile(uint8_t* base) { return base && CheckQolEditor(base); }
}
