// Tests for the generated Lua binding table and the three introspection
// primitives exposed through SwNativeApi:
//   * total / per-table / typed counts match the reviewed kit;
//   * luaBindingFind resolves a known binding to its exact RVA;
//   * unknown names and out-of-bounds indexes fail closed (NULL);
//   * table names are consistent and their sum equals the total;
//   * duplicate names are exactly the ones documented by the kit (the kit is
//     NOT globally name-unique: 427 entries, 391 distinct names).
#include "../src/lua_bindings.generated.h"
#include "../src/native_prims.h"
#include "../include/sw_mod_sdk.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace swml {
void LogError(const char*, ...) {}
void LogWarn(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
}  // namespace swml

namespace {

SwModHost g_host = {};

const SwNativeApi* Api() { return swml::native::NativePrimitives(); }

void TestCounts() {
    // Values verified by hand against lua_bindings.json.
    assert(kLuaBindingCount == 427);
    assert(kLuaBindingCountBase == 30);
    assert(kLuaBindingCountMc == 30);
    assert(kLuaBindingCountMission == 189);
    assert(kLuaBindingCountOther == 50);
    assert(kLuaBindingCountStdlib == 128);
    assert(kLuaBindingTypedCount == 46);

    assert(Api()->luaBindingCount(&g_host) == 427);
    // An invalid owner is fail-closed.
    assert(Api()->luaBindingCount(nullptr) == 0);
    puts("PASS: total / per-table / typed counts match the kit");
}

void TestFindKnown() {
    const SwLuaBindingInfo* entry = Api()->luaBindingFind(&g_host, "getVehiclePos");
    assert(entry != nullptr);
    assert(strcmp(entry->name, "getVehiclePos") == 0);
    assert(strcmp(entry->table, "mission") == 0);
    assert(entry->rva == 0x5A0260u);
    assert(entry->arity == 1);  // annotated "1 | 4" -> smallest accepted count
    puts("PASS: luaBindingFind(getVehiclePos) -> mission RVA 0x5A0260, arity 1");
}

void TestFindUnknown() {
    assert(Api()->luaBindingFind(&g_host, "thisBindingDoesNotExist") == nullptr);
    assert(Api()->luaBindingFind(&g_host, "") == nullptr);
    assert(Api()->luaBindingFind(&g_host, nullptr) == nullptr);
    assert(Api()->luaBindingFind(nullptr, "getVehiclePos") == nullptr);
    puts("PASS: unknown name / null name / invalid owner -> NULL");
}

void TestIndexBounds() {
    const SwLuaBindingInfo* first = Api()->luaBindingAt(&g_host, 0);
    assert(first != nullptr);
    assert(strcmp(first->name, "ipairs") == 0);
    assert(Api()->luaBindingAt(&g_host, 426) != nullptr);
    assert(Api()->luaBindingAt(&g_host, 427) == nullptr);
    assert(Api()->luaBindingAt(&g_host, UINT32_MAX) == nullptr);
    assert(Api()->luaBindingAt(nullptr, 0) == nullptr);
    puts("PASS: luaBindingAt bounds are fail-closed");
}

void TestTableConsistency() {
    size_t base = 0, mc = 0, mission = 0, other = 0, stdlib = 0;
    for (uint32_t i = 0; i < kLuaBindingCount; ++i) {
        const SwLuaBindingInfo* entry = Api()->luaBindingAt(&g_host, i);
        assert(entry != nullptr);
        assert(entry->name != nullptr && entry->name[0] != '\0');
        if (strcmp(entry->table, "base") == 0) ++base;
        else if (strcmp(entry->table, "mc") == 0) ++mc;
        else if (strcmp(entry->table, "mission") == 0) ++mission;
        else if (strcmp(entry->table, "other") == 0) ++other;
        else if (strcmp(entry->table, "stdlib") == 0) ++stdlib;
        else assert(false && "unknown table name");
    }
    assert(base == kLuaBindingCountBase);
    assert(mc == kLuaBindingCountMc);
    assert(mission == kLuaBindingCountMission);
    assert(other == kLuaBindingCountOther);
    assert(stdlib == kLuaBindingCountStdlib);
    assert(base + mc + mission + other + stdlib == kLuaBindingCount);
    puts("PASS: table names consistent, per-table sum == total");
}

void TestNameUniqueness() {
    // The kit is not globally name-unique. These are the 34 names that occur
    // more than once (391 distinct names for 427 entries); "type" occurs 4
    // times, every other duplicate occurs twice. The test asserts that the
    // generated table contains exactly this documented duplicate set, so an
    // accidental new duplicate is still caught.
    static const char* const kExpectedDuplicates[] = {
        "_G", "char", "close", "debug", "distance", "flush", "getmetatable",
        "httpGet", "identity", "invert", "ipairs", "len", "lines", "log",
        "multiply", "multiplyXYZW", "next", "pack", "pairs", "position", "read",
        "remove", "rotationToFaceXZ", "rotationX", "rotationY", "rotationZ",
        "setmetatable", "tonumber", "tostring", "translation", "transpose",
        "type", "unpack", "write",
    };

    std::set<std::string> names;
    std::set<std::string> duplicates;
    for (uint32_t i = 0; i < kLuaBindingCount; ++i) {
        const SwLuaBindingInfo* entry = Api()->luaBindingAt(&g_host, i);
        assert(entry != nullptr);
        if (!names.insert(entry->name).second) duplicates.insert(entry->name);
    }
    assert(names.size() == 391);
    assert(duplicates.size() == 34);
    for (const char* expected : kExpectedDuplicates) {
        assert(duplicates.count(expected) == 1);
    }

    // Find is deterministic: it returns the first occurrence in table order.
    const SwLuaBindingInfo* type = Api()->luaBindingFind(&g_host, "type");
    assert(type != nullptr);
    assert(strcmp(type->table, "base") == 0);
    assert(type->rva == 0x9C020u);
    puts("PASS: names unique except the 34 documented kit duplicates; Find is first-match");
}

}  // namespace

int main() {
    swml::native::SetNativeOwnerCheck(nullptr);
    swml::native::InitializeNativePrimitives(nullptr);

    TestCounts();
    TestFindKnown();
    TestFindUnknown();
    TestIndexBounds();
    TestTableConsistency();
    TestNameUniqueness();

    swml::native::ShutdownNativePrimitives();
    puts("All Lua binding tests passed.");
    return 0;
}
