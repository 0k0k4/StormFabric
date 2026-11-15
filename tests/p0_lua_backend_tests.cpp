// Tests the actual production wrappers against a fake Lua state and native functions.
// Never loads a PE, installs a hook, or launches/attaches to Stormworks.
#include "../src/native_lua.cpp"
#include <cassert>
#include <atomic>
#include <climits>
#include <csetjmp>
#include <thread>

namespace swml {
uint32_t EngineRva(uint8_t*, uint32_t) { return 0; }
bool ValidateEngineBinding(uint8_t*, uint32_t) { return false; }
bool ValidateLuaProfile(uint8_t*) { return false; }
bool ValidateQolEditorProfile(uint8_t*) { return false; }
bool ResolveAddonLuaProfile(uint8_t*, AddonLuaProfile*) { return false; }
void* ResolveLuaPcallk(uint8_t*) { return nullptr; }
bool ResolveSimulationTickProfile(uint8_t*, SimulationTickProfile*) { return false; }
void* ResolveGameEventProfile(uint8_t*) { return nullptr; }
std::vector<std::pair<SwEventType,SwHandle>> lifecycleEvents;
void QueueModLuaEvent(SwEventType type,SwHandle id) {lifecycleEvents.emplace_back(type,id);}
}
namespace {
struct Value { uint64_t bits; uint32_t tag; uint32_t padding; };
static_assert(sizeof(Value) == 16, "native TValue layout");
alignas(16) uint8_t stateMemory[0x100] = {}, ci[0x50] = {}, script[0x380] = {};
Value stack[256] = {}, relocated[256] = {};
uintptr_t L = reinterpret_cast<uintptr_t>(stateMemory);
void* state = stateMemory;
SwModHost owner{}, otherOwner{};
bool stackAvailable = true, relocateNext = false, failNative = false, failResize = false;
int stackChecks = 0, nativeCalls = 0;
std::jmp_buf* activeJump = nullptr;
std::vector<std::unique_ptr<uint8_t[]>> objects;
std::map<uintptr_t, std::map<std::string, Value>> fields;
const SwLuaExtrasApi* api = swml::NativeLuaExtras();

uintptr_t FakeIndex(uintptr_t lua, int index) {
    assert(lua == L);
    uintptr_t base = **(uintptr_t**)(L + 0x20) + 16;
    return index > 0 ? base + static_cast<uintptr_t>(index - 1) * 16 :
                      *(uintptr_t*)(L + 0x10) + static_cast<intptr_t>(index) * 16;
}
bool FakeCheck(uintptr_t lua, int slots) {
    assert(lua == L && slots >= 0);
    ++stackChecks;
    if (relocateNext) {
        const uintptr_t old = *(uintptr_t*)(L + 0x38);
        const uintptr_t offset = *(uintptr_t*)(L + 0x10) - old;
        memcpy(relocated, reinterpret_cast<void*>(old), sizeof(stack));
        *(uintptr_t*)(L + 0x38) = reinterpret_cast<uintptr_t>(relocated);
        *(uintptr_t*)ci = reinterpret_cast<uintptr_t>(relocated);
        *(uintptr_t*)(L + 0x10) = reinterpret_cast<uintptr_t>(relocated) + offset;
        relocateNext = false;
    }
    return stackAvailable && LuaGetTop(L) + slots < 255;
}
void FailIfRequested() {
    ++nativeCalls;
    if (failNative) { assert(activeJump); std::longjmp(*activeJump, 1); }
}
#pragma warning(push)
#pragma warning(disable:4611) // intentional setjmp fixture: this function owns only POD locals
int FakeProtected(uintptr_t lua, void (*fn)(uintptr_t, void*), void* data) {
    std::jmp_buf jump;
    std::jmp_buf* saved = activeJump;
    activeJump = &jump;
    const int failed = setjmp(jump);
    if (!failed) fn(lua, data);
    activeJump = saved;
    return failed ? 4 : 0;
}
#pragma warning(pop)
uintptr_t Object(size_t n) {
    auto bytes = std::make_unique<uint8_t[]>(n);
    memset(bytes.get(), 0, n);
    uintptr_t result = reinterpret_cast<uintptr_t>(bytes.get());
    objects.push_back(std::move(bytes));
    return result;
}
void PushValue(const Value& value) {
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    memcpy(reinterpret_cast<void*>(top), &value, sizeof(value));
    *(uintptr_t*)(L + 0x10) = top + 16;
}
const char* FakeString(uintptr_t lua, const char* bytes, size_t n) {
    assert(lua == L);
    FailIfRequested(); // jump before any fake C++ allocation/destructor frame
    uintptr_t object = Object(0x19 + n);
    const bool shortString = n <= 40;
    *(uint8_t*)(object + 8) = shortString ? 4 : 0x14;
    if (shortString) *(uint8_t*)(object + 0xb) = static_cast<uint8_t>(n);
    else *(size_t*)(object + 0x10) = n;
    memcpy(reinterpret_cast<void*>(object + 0x18), bytes, n);
    PushValue({object, shortString ? 0x44u : 0x54u, 0});
    return reinterpret_cast<const char*>(object + 0x18);
}
uintptr_t FakeTable(uintptr_t lua) { assert(lua == L); FailIfRequested(); return Object(0x38); }
void FakeResize(uintptr_t lua, uintptr_t table, uint32_t n) {
    assert(lua == L); FailIfRequested();
    if (failResize) { assert(activeJump); std::longjmp(*activeJump, 1); }
    *(uint32_t*)(table + 0xc) = n;
}
void FakeSetField(uintptr_t lua, uintptr_t tv, const char* key) {
    assert(lua == L); FailIfRequested();
    fields[*(uintptr_t*)tv][key] = *reinterpret_cast<Value*>(*(uintptr_t*)(L + 0x10) - 16);
    *(uintptr_t*)(L + 0x10) -= 16;
}
int FakeGetField(uintptr_t lua, uintptr_t tv, const char* key) {
    assert(lua == L); FailIfRequested();
    PushValue(fields[*(uintptr_t*)tv][key]);
    return static_cast<int>(reinterpret_cast<Value*>(*(uintptr_t*)(L + 0x10) - 16)->tag & 0xf);
}
void ResetState() {
    memset(stateMemory, 0, sizeof(stateMemory));
    memset(stack, 0, sizeof(stack));
    *(uintptr_t*)(L + 0x38) = reinterpret_cast<uintptr_t>(stack);
    *(uintptr_t*)(L + 0x10) = reinterpret_cast<uintptr_t>(stack + 1);
    *(uintptr_t*)(L + 0x20) = reinterpret_cast<uintptr_t>(ci);
    *(uintptr_t*)ci = reinterpret_cast<uintptr_t>(stack);
    *(uintptr_t*)(script + 8) = L;
    g_liveLuaVms[L] = {reinterpret_cast<uintptr_t>(script), ++g_nextLuaGeneration};
    stackAvailable = true;
    failNative = relocateNext = false;
}

int Exercise(void* lua, void*) {
    assert(lua == state && api->structSize == sizeof(SwLuaExtrasApi));
    assert(ApiGetTop(lua) == 0);
    const int checks = stackChecks;
    assert(api->pushNil(&otherOwner, lua) == SW_LUA_INVALID_CONTEXT);
    assert(api->pushNil(&owner, reinterpret_cast<void*>(1)) == SW_LUA_INVALID_CONTEXT);
    assert(api->pushNil(nullptr, lua) == SW_LUA_INVALID_ARGUMENT);
    assert(api->pushString(&owner, lua, nullptr, 1) == SW_LUA_INVALID_ARGUMENT);
    assert(api->pushString(&owner, lua, "", SW_LUA_MAX_STRING_BYTES + 1) == SW_LUA_INVALID_ARGUMENT);
    assert(api->createTable(&owner, lua, SW_LUA_MAX_ARRAY_SLOTS + 1) == SW_LUA_INVALID_ARGUMENT);
    assert(!ApiCheckStack(lua, INT_MAX));
    assert(!ApiCheckStack(lua, -1));
    assert(stackChecks == checks);
    std::thread wrongThread([&] {
        assert(api->pushNil(&owner, lua) == SW_LUA_INVALID_CONTEXT);
        assert(ApiGetTop(lua) == 0);
    });
    wrongThread.join();
    SwLuaType type = SW_LUA_TYPE_NIL;
    assert(api->getType(&owner, lua, 1, &type) == SW_LUA_OUT_OF_RANGE && type == SW_LUA_TYPE_NONE);
    assert(api->getType(&owner, lua, INT_MIN, &type) == SW_LUA_OUT_OF_RANGE);
    assert(api->getType(&owner, lua, 0, &type) == SW_LUA_OUT_OF_RANGE);
    assert(api->getType(&owner, lua, 1, nullptr) == SW_LUA_INVALID_ARGUMENT);
    stackAvailable = false;
    assert(api->pushInteger(&owner, lua, 1) == SW_LUA_STACK_LIMIT);
    ApiPushNumber(lua, 123);
    assert(ApiGetTop(lua) == 0);
    stackAvailable = true;
    assert(api->pushNil(&owner, lua) == SW_LUA_OK);
    assert(api->getType(&owner, lua, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_NIL);
    assert(api->pushInteger(&owner, lua, INT64_MIN) == SW_LUA_OK);
    assert(api->getType(&owner, lua, 2, &type) == SW_LUA_OK && type == SW_LUA_TYPE_INTEGER);
    assert(*reinterpret_cast<int64_t*>(FakeIndex(L, 2)) == INT64_MIN);
    ApiPushNumber(lua, 1.5);
    ApiPushBool(lua, false);
    assert(api->getType(&owner, lua, 3, &type) == SW_LUA_OK && type == SW_LUA_TYPE_NUMBER);
    assert(api->getType(&owner, lua, 4, &type) == SW_LUA_OK && type == SW_LUA_TYPE_BOOLEAN);
    assert(api->pop(&owner, lua, -1) == SW_LUA_OUT_OF_RANGE);
    assert(api->pop(&owner, lua, 5) == SW_LUA_OUT_OF_RANGE);
    assert(api->pop(&owner, lua, 4) == SW_LUA_OK);
    const char binary[] = {'h', '\0', static_cast<char>(0xc3), static_cast<char>(0xa9)};
    assert(api->pushString(&owner, lua, binary, sizeof(binary)) == SW_LUA_OK);
    char out[8] = {}; size_t length = 99;
    assert(api->copyString(&owner, lua, -1, out, sizeof(out), &length) == SW_LUA_OK);
    assert(length == sizeof(binary) && !memcmp(out, binary, length) && out[length] == 0);
    assert(ApiArgString(lua, 1, out, sizeof(out)) == sizeof(binary));
    assert(api->copyString(&owner, lua, 1, out, 3, &length) == SW_LUA_BUFFER_TOO_SMALL);
    assert(out[2] == 0 && length == 4);
    assert(api->copyString(&owner, lua, 1, nullptr, 0, &length) == SW_LUA_OK && length == 4);
    assert(api->copyString(&owner, lua, 1, nullptr, 2, &length) == SW_LUA_INVALID_ARGUMENT);
    std::string longString(73, 'x'); longString[40] = 0;
    assert(api->pushString(&owner, lua, longString.data(), longString.size()) == SW_LUA_OK);
    assert(api->copyString(&owner, lua, -1, nullptr, 0, &length) == SW_LUA_OK && length == 73);
    assert(api->pop(&owner, lua, 2) == SW_LUA_OK);
    assert(api->pushString(&owner, lua, nullptr, 0) == SW_LUA_OK);
    assert(api->copyString(&owner, lua, 1, out, 1, &length) == SW_LUA_OK && length == 0);
    assert(api->pop(&owner, lua, 1) == SW_LUA_OK);
    assert(api->createTable(&owner, lua, 12) == SW_LUA_OK);
    const uintptr_t table = *reinterpret_cast<uintptr_t*>(FakeIndex(L, 1));
    assert(*(uint32_t*)(table + 0xc) == 12);
    assert(api->copyString(&owner, lua, 1, out, sizeof(out), &length) == SW_LUA_TYPE_MISMATCH);
    assert(api->pushInteger(&owner, lua, INT64_MAX) == SW_LUA_OK);
    relocateNext = true; // setField must reacquire the table TValue after checkStack relocates it
    assert(api->setField(&owner, lua, -2, "answer") == SW_LUA_OK && ApiGetTop(lua) == 1);
    assert(api->getField(&owner, lua, -1, "answer") == SW_LUA_OK && ApiGetTop(lua) == 2);
    assert(*reinterpret_cast<int64_t*>(FakeIndex(L, -1)) == INT64_MAX);
    assert(api->getField(&owner, lua, -1, "wrong") == SW_LUA_TYPE_MISMATCH);
    assert(api->pop(&owner, lua, 1) == SW_LUA_OK);
    assert(api->getField(&owner, lua, 1, "missing") == SW_LUA_OK);
    assert(api->getType(&owner, lua, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_NIL);
    assert(api->pop(&owner, lua, 1) == SW_LUA_OK);
    assert(api->getField(&owner, lua, 1, "") == SW_LUA_INVALID_ARGUMENT);
    char bigKey[256]; memset(bigKey, 'z', sizeof(bigKey));
    assert(api->getField(&owner, lua, 1, bigKey) == SW_LUA_INVALID_ARGUMENT);
    *(uintptr_t*)(table + 0x28) = 1;
    assert(api->getField(&owner, lua, 1, "answer") == SW_LUA_UNSUPPORTED);
    *(uintptr_t*)(table + 0x28) = 0;
    assert(api->raiseError(&owner, lua, "error", 5) == SW_LUA_UNSUPPORTED && ApiGetTop(lua) == 1);
    failNative = true;
    assert(api->pushString(&owner, lua, "oom", 3) == SW_LUA_ENGINE_ERROR && ApiGetTop(lua) == 1);
    assert(api->createTable(&owner, lua, 0) == SW_LUA_ENGINE_ERROR && ApiGetTop(lua) == 1);
    assert(api->getField(&owner, lua, 1, "answer") == SW_LUA_ENGINE_ERROR && ApiGetTop(lua) == 1);
    failNative = false;
    failResize = true;
    assert(api->createTable(&owner, lua, 12) == SW_LUA_ENGINE_ERROR && ApiGetTop(lua) == 1);
    failResize = false;
    const uint64_t generation = g_liveLuaVms[L].generation;
    ++g_liveLuaVms[L].generation;
    assert(api->pushNil(&owner, lua) == SW_LUA_INVALID_CONTEXT);
    g_liveLuaVms[L].generation = generation;
    auto savedProtect = g_rawrunprotected; g_rawrunprotected = nullptr;
    assert(api->createTable(&owner, lua, 0) == SW_LUA_UNSUPPORTED);
    g_rawrunprotected = savedProtect;
    assert(api->pop(&owner, lua, 1) == SW_LUA_OK);
    swml::ReleaseNativeLuaOwner(&owner);
    assert(api->pushNil(&owner, lua) == SW_LUA_INVALID_CONTEXT);
    return 0;
}
int BadResults(void*, void*) { return INT_MAX; }
int Throws(void*, void*) { throw 123; }
// Natives used only by library installation: push _G, and turn the pushed
// light-userdata token into a C closure value.
uintptr_t globalObject = 0;
void FakeRawGeti(uintptr_t lua, int index, long long n) {
    assert(lua == L && index == kRegistryIdx && n == 2);
    if (!globalObject) globalObject = Object(0x38);
    PushValue({globalObject, 0x45u, 0});
}
// Fake lua_pcallk: the "function" value selects success (1) or a raised error (2).
int pcallCount = 0;
int FakePcall(uintptr_t lua, int nargs, int nresults, int errfunc, intptr_t ctx, void* k) {
    assert(lua == L && errfunc == 0 && ctx == 0 && k == nullptr && nargs >= 0 && nresults >= 0);
    ++pcallCount;
    auto* function = reinterpret_cast<Value*>(*(uintptr_t*)(L + 0x10) - static_cast<uintptr_t>(nargs + 1) * 16);
    assert((function->tag & 0xf) == 6);
    const uint64_t mode = function->bits;
    *(uintptr_t*)(L + 0x10) = reinterpret_cast<uintptr_t>(function);
    if (mode == 2) { FakeString(L, "boom: script error", 18); return 2; }
    for (int i = 0; i < nresults; ++i) PushValue({static_cast<uint64_t>(100 + i), 0x13u, 0});
    return 0;
}
int CallChecks(void* lua, void*) {
    assert(api->structSize == sizeof(SwLuaExtrasApi) && ApiGetTop(lua) == 0);
    SwLuaType type = SW_LUA_TYPE_NONE;
    // ---- getGlobal ----
    FakeRawGeti(L, kRegistryIdx, 2); *(uintptr_t*)(L + 0x10) -= 16; // materialise _G
    fields[globalObject]["server"] = {Object(0x38), 0x45u, 0};
    assert(api->getGlobal(&otherOwner, lua, "server") == SW_LUA_INVALID_CONTEXT);
    assert(api->getGlobal(&owner, lua, nullptr) == SW_LUA_INVALID_ARGUMENT);
    assert(api->getGlobal(&owner, lua, "") == SW_LUA_INVALID_ARGUMENT && ApiGetTop(lua) == 0);
    assert(api->getGlobal(&owner, lua, "server") == SW_LUA_OK && ApiGetTop(lua) == 1);
    assert(api->getType(&owner, lua, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_TABLE);
    assert(*reinterpret_cast<uintptr_t*>(FakeIndex(L, 1)) != globalObject); // _G itself was removed
    assert(api->getGlobal(&owner, lua, "missing") == SW_LUA_OK && ApiGetTop(lua) == 2);
    assert(api->getType(&owner, lua, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_NIL);
    *(uintptr_t*)(globalObject + 0x28) = 1; // a metatable on _G could run script code
    assert(api->getGlobal(&owner, lua, "server") == SW_LUA_UNSUPPORTED && ApiGetTop(lua) == 2);
    *(uintptr_t*)(globalObject + 0x28) = 0;
    failNative = true;
    assert(api->getGlobal(&owner, lua, "server") == SW_LUA_ENGINE_ERROR && ApiGetTop(lua) == 2);
    failNative = false;
    assert(api->pop(&owner, lua, 2) == SW_LUA_OK);
    // ---- call ----
    char error[32] = "untouched";
    assert(api->call(&owner, lua, 0, 0, nullptr, 0) == SW_LUA_OUT_OF_RANGE); // nothing to call
    assert(api->call(&otherOwner, lua, 0, 0, nullptr, 0) == SW_LUA_INVALID_CONTEXT);
    assert(api->pushInteger(&owner, lua, 5) == SW_LUA_OK);
    assert(api->call(&owner, lua, 0, 0, error, sizeof(error)) == SW_LUA_TYPE_MISMATCH && ApiGetTop(lua) == 1);
    assert(api->call(&owner, lua, -1, 0, nullptr, 0) == SW_LUA_INVALID_ARGUMENT);
    assert(api->call(&owner, lua, 0, 256, nullptr, 0) == SW_LUA_INVALID_ARGUMENT);
    assert(api->call(&owner, lua, 256, 0, nullptr, 0) == SW_LUA_INVALID_ARGUMENT);
    assert(api->call(&owner, lua, 0, 0, nullptr, 4) == SW_LUA_INVALID_ARGUMENT);
    assert(api->pop(&owner, lua, 1) == SW_LUA_OK && pcallCount == 0);
    PushValue({1, 0x66u, 0});
    auto savedCall = g_pcallk; g_pcallk = nullptr;
    assert(api->call(&owner, lua, 0, 0, nullptr, 0) == SW_LUA_UNSUPPORTED && ApiGetTop(lua) == 1);
    g_pcallk = savedCall;
    stackAvailable = false;
    assert(api->call(&owner, lua, 0, 1, nullptr, 0) == SW_LUA_STACK_LIMIT && ApiGetTop(lua) == 1 && pcallCount == 0);
    stackAvailable = true;
    assert(api->pushInteger(&owner, lua, 1) == SW_LUA_OK && api->pushInteger(&owner, lua, 2) == SW_LUA_OK);
    assert(api->call(&owner, lua, 3, 0, nullptr, 0) == SW_LUA_OUT_OF_RANGE); // more arguments than values
    assert(api->call(&owner, lua, 2, 3, error, sizeof(error)) == SW_LUA_OK && pcallCount == 1);
    assert(ApiGetTop(lua) == 3 && error[0] == 0);
    for (int i = 0; i < 3; ++i) assert(*reinterpret_cast<int64_t*>(FakeIndex(L, i + 1)) == 100 + i);
    assert(api->pushNil(&owner, lua) == SW_LUA_OK); // the callback context survives the call
    assert(api->pop(&owner, lua, 4) == SW_LUA_OK);
    // A raised error comes back as a status: nothing is left on the stack.
    PushValue({2, 0x66u, 0});
    assert(api->pushNil(&owner, lua) == SW_LUA_OK);
    assert(api->call(&owner, lua, 1, 5, error, sizeof(error)) == SW_LUA_CALL_FAILED && ApiGetTop(lua) == 0);
    assert(!strcmp(error, "boom: script error"));
    PushValue({2, 0x66u, 0});
    char tiny[5] = "zzzz";
    assert(api->call(&owner, lua, 0, 0, tiny, sizeof(tiny)) == SW_LUA_CALL_FAILED && !strcmp(tiny, "boom"));
    PushValue({2, 0x66u, 0});
    assert(api->call(&owner, lua, 0, 0, nullptr, 0) == SW_LUA_CALL_FAILED && ApiGetTop(lua) == 0);
    return 0;
}
void FakeClosure(uintptr_t lua, void* fn, int upvalues) {
    assert(lua == L && fn && upvalues == 1);
    auto* top = reinterpret_cast<Value*>(*(uintptr_t*)(L + 0x10) - 16);
    assert(top->tag == 2);
    top->tag = 0x66;
}
alignas(16) uint8_t addonScript[0x90] = {};
// ---- game event fixtures ----
struct FakeValue { int32_t type; uint8_t pad[0x84]; double number; int64_t i90; int64_t i98; const char* text; uint8_t pad2[8]; uint8_t boolean; uint8_t pad3[7]; };
static_assert(sizeof(FakeValue) == 0xb8 && offsetof(FakeValue, number) == 0x88 && offsetof(FakeValue, i98) == 0x98 &&
              offsetof(FakeValue, text) == 0xa0 && offsetof(FakeValue, boolean) == 0xb0, "game value layout");
struct SeenEvent { std::string name; std::vector<SwGameValue> values; std::vector<std::string> texts; void* state; uint64_t sequence; };
std::vector<SeenEvent> events;
int originalCalls = 0;
uint64_t FakeCallByName(uintptr_t, EStr*, GameArgRing*) { ++originalCalls; return 0xabcd01; }
void OnEvent(const SwGameEvent* event, void* data) {
    assert(event->structSize == sizeof(SwGameEvent) && data == reinterpret_cast<void*>(0x55));
    SeenEvent seen{event->name, {}, {}, event->luaState, event->sequence};
    for (uint32_t i = 0; i < event->argumentCount; ++i) {
        seen.values.push_back(event->arguments[i]);
        seen.texts.push_back(event->arguments[i].string ? event->arguments[i].string : "");
    }
    events.push_back(seen);
    // The receiving VM is usable through lua->extras; what the mod leaves is dropped.
    if (event->luaState == state) { // only the main fake state implements the Lua natives
        assert(api->pushNil(&owner, event->luaState) == SW_LUA_OK);
        assert(api->pushNil(&otherOwner, event->luaState) == SW_LUA_INVALID_CONTEXT);
    }
}
int filteredEvents = 0;
void OnlyJoin(const SwGameEvent* event, void*) { assert(!strcmp(event->name, "onPlayerJoin")); ++filteredEvents; }
void CrashEvent(const SwGameEvent*, void*) { *(volatile int*)nullptr = 1; }
uint64_t CallEvent(uintptr_t target, const char* name, std::vector<FakeValue>& values, uint32_t head = 0) {
    EStr text{const_cast<char*>(name), static_cast<uint32_t>(strlen(name)), 0};
    const uint32_t capacity = static_cast<uint32_t>(values.size()) + head;
    std::vector<FakeValue> ring(capacity ? capacity : 1);
    for (size_t i = 0; i < values.size(); ++i) ring[(head + i) % capacity] = values[i];
    GameArgRing args{reinterpret_cast<uint8_t*>(ring.data()), capacity, head, static_cast<uint32_t>(values.size())};
    return HkCallByName(target, &text, &args);
}
FakeValue Value(int32_t type) { FakeValue v{}; v.type = type; return v; }
// ---- simulation tick fixtures ----
std::vector<std::pair<uint64_t, void*>> ticks;
int tickPushes = 0;
void OnTick(const SwSimulationTick* tick, void* data) {
    assert(tick->structSize == sizeof(SwSimulationTick) && data == reinterpret_cast<void*>(0x77));
    ticks.emplace_back(tick->sequence, tick->luaState);
    if (tick->luaState) {
        // lua->extras is usable during the tick, and junk left on the stack is discarded.
        assert(api->pushNil(&owner, tick->luaState) == SW_LUA_OK);
        assert(api->pushInteger(&owner, tick->luaState, 3) == SW_LUA_OK);
        assert(api->pushNil(&otherOwner, tick->luaState) == SW_LUA_INVALID_CONTEXT);
        ++tickPushes;
    }
}
SwHandle selfHandle = 0;
int selfCalls = 0;
void UnsubscribeSelf(const SwSimulationTick*, void*) { ++selfCalls; assert(swml::UnsubscribeSimulationTick(&owner, selfHandle) == SW_OK); }
void CrashTick(const SwSimulationTick*, void*) { throw 7; }
uint64_t forwarded[12] = {};
void FakeSimUpdate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                   uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12) {
    const uint64_t args[12] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12};
    memcpy(forwarded, args, sizeof(args));
    assert(ticks.empty() || ticks.back().first == g_tickSequence); // tick is delivered after the update
}
int addonCalls = 0;
int AddonPing(void* lua, void*) {
    ++addonCalls;
    // Microcontroller-only QoL must be inert for an addon script object.
    assert(swml::NativeLuaPrimitives()->qol->clamp(lua) == 0);
    assert(swml::NativeLuaPrimitives()->qol->getNumbers(lua) == 0);
    assert(api->pushInteger(&owner, lua, 7) == SW_LUA_OK);
    return 1;
}
void FakeAddonInit(uintptr_t self) { assert(self == reinterpret_cast<uintptr_t>(addonScript)); *(uintptr_t*)(self + 8) = L; }
void FakeAddonDestroy(uintptr_t self) {
    assert(g_liveLuaVms.count(L) == 0); // invalidated before the original close runs
    *(uintptr_t*)(self + 8) = 0;
}
void FakeInit(uintptr_t self) { assert(self == reinterpret_cast<uintptr_t>(script)); *(uintptr_t*)(self + 8) = L; }
void FakeDestroy(uintptr_t self) {
    assert(g_liveLuaVms.count(L) == 0); // invalidated before the original destructor runs
    *(uintptr_t*)(self + 8) = 0;
}
LibraryFunction* Token(SwLuaCallback callback) {
    auto entry = std::make_unique<LibraryFunction>();
    entry->owner = &owner; entry->callback = callback;
    auto result = entry.get(); g_libraryFunctions.push_back(std::move(entry));
    return result;
}
int Dispatch(LibraryFunction* token) {
    g_upvalueOverride = reinterpret_cast<uintptr_t>(token);
    int result = DispatchLibraryFunction(L);
    g_upvalueOverride = 0;
    return result;
}
std::atomic<bool> entered{false}, mayReturn{false}, released{false};
int WaitForUnload(void*, void*) {
    entered = true;
    while (!mayReturn) std::this_thread::yield();
    assert(!released);
    return 0;
}
}
int main() {
    g_installed = true;
    g_index2addr = &FakeIndex; g_checkstack = &FakeCheck;
    g_pushlstring = &FakeString; g_newtable = &FakeTable; g_resizearray = &FakeResize;
    g_getfield = &FakeGetField; g_setfield = &FakeSetField; g_rawrunprotected = &FakeProtected;
    ResetState();
    assert(api->pushNil(&owner, reinterpret_cast<void*>(1)) == SW_LUA_INVALID_CONTEXT);
    assert(!ApiCheckStack(reinterpret_cast<void*>(1), 1));
    assert(Dispatch(Token(&Exercise)) == 0 && g_luaCallbackFrame == nullptr);
    assert(api->pushNil(&owner, state) == SW_LUA_INVALID_CONTEXT);
    ResetState();
    g_rawgeti = &FakeRawGeti; g_pcallk = &FakePcall;
    assert(Dispatch(Token(&CallChecks)) == 0 && g_luaCallbackFrame == nullptr && pcallCount == 4);
    char outside[8] = "keep";
    assert(api->call(&owner, state, 0, 0, outside, sizeof(outside)) == SW_LUA_INVALID_CONTEXT && !strcmp(outside, "keep"));
    assert(api->getGlobal(&owner, state, "server") == SW_LUA_INVALID_CONTEXT);
    ResetState();
    auto bad = Token(&BadResults);
    assert(Dispatch(bad) == 0 && !bad->callback && ApiGetTop(state) == 0);
    auto throws = Token(&Throws);
    assert(Dispatch(throws) == 0 && !throws->callback && g_luaCallbackFrame == nullptr);
    auto stale = Token(&Exercise);
    g_liveLuaVms.erase(L);
    assert(Dispatch(stale) == 0); // VM destruction invalidates a retained closure
    ResetState();
    stale->owner = nullptr; stale->callback = nullptr;
    assert(Dispatch(stale) == 0); // same host/state address cannot revive old owner tokens
    assert(Dispatch(reinterpret_cast<LibraryFunction*>(1)) == 0); // forged upvalue not dereferenced
    auto inflight = Token(&WaitForUnload);
    std::thread callback([&] { assert(Dispatch(inflight) == 0); });
    while (!entered) std::this_thread::yield();
    std::thread unload([&] { swml::ReleaseNativeLuaOwner(&owner); released = true; });
    mayReturn = true;
    callback.join(); unload.join();
    assert(released && !inflight->owner && !inflight->callback);
    char library[] = "copied_library", function[] = "copied_function";
    SwLuaFunction functions[] = {{function, &BadResults, reinterpret_cast<void*>(123)}};
    assert(!RegisterLibrary(&owner, "math", functions, 1));
    assert(!RegisterLibrary(&owner, "bad.name", functions, 1));
    assert(!RegisterLibrary(nullptr, library, functions, 1));
    assert(!RegisterLibrary(&owner, library, functions, 0));
    assert(RegisterLibrary(&owner, library, functions, 1));
    assert(!RegisterLibrary(&otherOwner, library, functions, 1));
    library[0] = 'x'; function[0] = 'x'; functions[0].callback = nullptr;
    assert(g_libraryFunctions.back()->library == "copied_library");
    assert(g_libraryFunctions.back()->name == "copied_function");
    assert(g_libraryFunctions.back()->callback == &BadResults);
    swml::ReleaseNativeLuaOwner(&owner);
    g_enabled = false;
    g_origMcLuaInit = &FakeInit; g_origMcLuaDestroy = &FakeDestroy;
    const auto beforeInit = g_liveLuaVms.at(L).generation;
    HkMcLuaInit(reinterpret_cast<uintptr_t>(script));
    assert(g_liveLuaVms.at(L).generation > beforeInit);
    const auto firstInit = g_liveLuaVms.at(L).generation;
    HkMcLuaInit(reinterpret_cast<uintptr_t>(script));
    assert(g_liveLuaVms.at(L).generation > firstInit);
    HkMcLuaDestroy(reinterpret_cast<uintptr_t>(script));
    assert(g_liveLuaVms.count(L) == 0);
    assert(swml::lifecycleEvents.size()==5);
    assert(swml::lifecycleEvents[0]==std::make_pair(SW_EVENT_LUA_DESTROYED,beforeInit));
    assert(swml::lifecycleEvents[1]==std::make_pair(SW_EVENT_LUA_CREATED,firstInit));
    assert(swml::lifecycleEvents[2]==std::make_pair(SW_EVENT_LUA_DESTROYED,firstInit));
    assert(swml::lifecycleEvents[3].first==SW_EVENT_LUA_CREATED);
    assert(swml::lifecycleEvents[4]==std::make_pair(SW_EVENT_LUA_DESTROYED,swml::lifecycleEvents[3].second));

    // ---- Addon/mission script VMs -------------------------------------------------------
    swml::lifecycleEvents.clear();
    SwLuaFunction addonFunctions[] = {{"ping", &AddonPing, nullptr}};
    assert(!RegisterAddonLibrary(&owner, "sf_demo", addonFunctions, 1)); // hooks not installed: fail closed
    g_addonInstalled = true;
    for (const char* name : {"server", "matrix", "g_savedata", "pairs", "onTick", "onPlayerJoin", "httpReply", "math"})
        assert(!RegisterAddonLibrary(&owner, name, addonFunctions, 1));
    assert(RegisterAddonLibrary(&owner, "online", addonFunctions, 1)); // "on" + lowercase is not a callback name
    assert(RegisterAddonLibrary(&owner, "sf_demo", addonFunctions, 1));
    assert(!RegisterAddonLibrary(&otherOwner, "sf_demo", addonFunctions, 1));
    functions[0].callback = &BadResults;
    assert(RegisterLibrary(&owner, "sf_demo", functions, 1)); // scopes are independent
    g_rawgeti = &FakeRawGeti; g_pushcclosure = &FakeClosure;
    g_origAddonLuaInit = &FakeAddonInit; g_origAddonLuaDestroy = &FakeAddonDestroy;
    ResetState(); g_liveLuaVms.clear(); fields.clear();
    const uintptr_t addonSelf = reinterpret_cast<uintptr_t>(addonScript);
    HkAddonLuaInit(addonSelf);
    assert(g_liveLuaVms.count(L) == 1 && g_liveLuaVms.at(L).addon && g_liveLuaVms.at(L).self == addonSelf);
    const auto addonGeneration = g_liveLuaVms.at(L).generation;
    // Only addon-scoped libraries are installed as globals of the addon VM.
    size_t addonTables = 0; LibraryFunction* pingToken = nullptr;
    for (const auto& table : fields) for (const auto& field : table.second) {
        if (field.first == "sf_demo" || field.first == "online") ++addonTables;
        assert(field.first != function);
    }
    assert(addonTables == 2 && ApiGetTop(state) == 0);
    for (const auto& e : g_libraryFunctions) if (e->addon && e->library == "sf_demo") pingToken = e.get();
    assert(pingToken && Dispatch(pingToken) == 1 && addonCalls == 1);
    ResetState(); g_liveLuaVms.clear(); g_liveLuaVms[L] = {addonSelf, addonGeneration, true};
    HkAddonLuaInit(addonSelf); // script reload: destroy then create, new generation
    assert(g_liveLuaVms.at(L).generation > addonGeneration);
    const auto reloaded = g_liveLuaVms.at(L).generation;
    HkAddonLuaDestroy(addonSelf);
    assert(g_liveLuaVms.count(L) == 0 && Dispatch(pingToken) == 0 && addonCalls == 1);
    assert(swml::lifecycleEvents.size() == 4);
    assert(swml::lifecycleEvents[0] == std::make_pair(SW_EVENT_ADDON_LUA_CREATED, addonGeneration));
    assert(swml::lifecycleEvents[1] == std::make_pair(SW_EVENT_ADDON_LUA_DESTROYED, addonGeneration));
    assert(swml::lifecycleEvents[2] == std::make_pair(SW_EVENT_ADDON_LUA_CREATED, reloaded));
    assert(swml::lifecycleEvents[3] == std::make_pair(SW_EVENT_ADDON_LUA_DESTROYED, reloaded));
    swml::ReleaseNativeLuaOwner(&owner);
    assert(!pingToken->owner && !pingToken->callback);

    // ---- Simulation tick --------------------------------------------------------------
    SwHandle tickHandle = 0;
    assert(swml::SubscribeSimulationTick(nullptr, &OnTick, nullptr, &tickHandle) == SW_INVALID_OWNER);
    assert(swml::SubscribeSimulationTick(&owner, nullptr, nullptr, &tickHandle) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeSimulationTick(&owner, &OnTick, nullptr, nullptr) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeSimulationTick(&owner, &OnTick, nullptr, &tickHandle) == SW_UNAVAILABLE); // profile rejected
    g_tickInstalled = true; // the call-site redirection itself is covered by lua_profile_tests
    assert(swml::SubscribeSimulationTick(&owner, &OnTick, reinterpret_cast<void*>(0x77), &tickHandle) == SW_OK && tickHandle);
    ResetState(); g_liveLuaVms.clear();
    RunSimulationTick();                       // no addon VM alive: tick without a state
    assert(ticks.size() == 1 && ticks[0].second == nullptr);
    g_liveLuaVms[L] = {addonSelf, 77, true};
    *(uintptr_t*)(addonSelf + 8) = L;
    *(uint32_t*)(addonSelf + 0x78) = 3;        // errored script: not handed out
    RunSimulationTick();
    assert(ticks.size() == 2 && ticks[1].second == nullptr);
    *(uint32_t*)(addonSelf + 0x78) = 0;
    const int topBefore = ApiGetTop(state);
    RunSimulationTick();
    assert(ticks.size() == 3 && ticks[2].second == state && tickPushes == 1);
    assert(ticks[2].first == ticks[1].first + 1 && ticks[1].first == ticks[0].first + 1);
    assert(*(uintptr_t*)(L + 0x10) == reinterpret_cast<uintptr_t>(stack + 1) && topBefore == 0);
    assert(g_luaCallbackFrame == nullptr);
    g_liveLuaVms[L] = {reinterpret_cast<uintptr_t>(script), 78, false}; // microcontroller VMs are never used
    RunSimulationTick();
    assert(ticks.back().second == nullptr);
    g_liveLuaVms[L] = {addonSelf, 77, true};
    // The hook forwards all twelve arguments unchanged, then ticks.
    g_origSimUpdate = &FakeSimUpdate;
    const size_t before = ticks.size();
    HkSimulationUpdate(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0xffffffffffffffffull);
    for (int i = 0; i < 11; ++i) assert(forwarded[i] == static_cast<uint64_t>(i + 1));
    assert(forwarded[11] == 0xffffffffffffffffull && ticks.size() == before + 1);
    // Unsubscribe from inside a tick, ownership checks, crash isolation, owner release.
    assert(swml::SubscribeSimulationTick(&owner, &UnsubscribeSelf, nullptr, &selfHandle) == SW_OK);
    RunSimulationTick(); RunSimulationTick();
    assert(selfCalls == 1);
    assert(swml::UnsubscribeSimulationTick(&owner, selfHandle) == SW_NOT_FOUND);
    assert(swml::UnsubscribeSimulationTick(&otherOwner, tickHandle) == SW_NOT_FOUND);
    SwHandle crashHandle = 0;
    assert(swml::SubscribeSimulationTick(&otherOwner, &CrashTick, nullptr, &crashHandle) == SW_OK);
    const size_t beforeCrash = ticks.size();
    RunSimulationTick();                       // the crash is contained; other subscribers still run
    assert(ticks.size() == beforeCrash + 1 && g_luaCallbackFrame == nullptr);
    assert(swml::UnsubscribeSimulationTick(&otherOwner, crashHandle) == SW_NOT_FOUND); // already disabled
    for (int i = 0; i < 63; ++i) { SwHandle h; assert(swml::SubscribeSimulationTick(&owner, &OnTick, nullptr, &h) == SW_OK); }
    SwHandle overLimit = 0;
    assert(swml::SubscribeSimulationTick(&owner, &OnTick, nullptr, &overLimit) == SW_LIMIT_REACHED);
    swml::ReleaseNativeLuaOwner(&owner);
    const size_t beforeRelease = ticks.size();
    RunSimulationTick();
    assert(ticks.size() == beforeRelease); // released owners are never called again
    assert(swml::UnsubscribeSimulationTick(&owner, tickHandle) == SW_NOT_FOUND);

    // ---- Game events --------------------------------------------------------------------
    SwHandle eventHandle = 0;
    assert(swml::SubscribeGameEvent(&owner, "onTick", &OnEvent, nullptr, &eventHandle) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeGameEvent(&owner, "onDraw", &OnEvent, nullptr, &eventHandle) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeGameEvent(&owner, "player", &OnEvent, nullptr, &eventHandle) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeGameEvent(&owner, "onBad.name", &OnEvent, nullptr, &eventHandle) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeGameEvent(&owner, nullptr, nullptr, nullptr, &eventHandle) == SW_INVALID_ARGUMENT);
    assert(swml::SubscribeGameEvent(nullptr, nullptr, &OnEvent, nullptr, &eventHandle) == SW_INVALID_OWNER);
    assert(swml::SubscribeGameEvent(&owner, nullptr, &OnEvent, nullptr, &eventHandle) == SW_UNAVAILABLE); // profile rejected
    g_origCallByName = &FakeCallByName;
    std::vector<FakeValue> none;
    // No subscription: the hook only forwards, and returns the original result untouched.
    assert(CallEvent(addonSelf, "onPlayerJoin", none) == 0xabcd01 && originalCalls == 1 && events.empty());
    g_eventInstalled = true; // the entry patch itself is covered by lua_profile_tests
    swml::ReleaseNativeLuaOwner(&owner); swml::ReleaseNativeLuaOwner(&otherOwner);
    assert(swml::SubscribeGameEvent(&owner, nullptr, &OnEvent, reinterpret_cast<void*>(0x55), &eventHandle) == SW_OK);
    alignas(16) static uint8_t addonB[0x90] = {}, addonC[0x90] = {}, mcScript[0x90] = {};
    alignas(16) static uint8_t stateB[0x100] = {}, stateC[0x100] = {}, stateM[0x100] = {};
    ResetState(); g_liveLuaVms.clear();
    const uintptr_t sB = reinterpret_cast<uintptr_t>(addonB), sC = reinterpret_cast<uintptr_t>(addonC), sM = reinterpret_cast<uintptr_t>(mcScript);
    *(uintptr_t*)(addonSelf + 8) = L;
    *(uintptr_t*)(sB + 8) = reinterpret_cast<uintptr_t>(stateB);
    *(uintptr_t*)(sC + 8) = reinterpret_cast<uintptr_t>(stateC);
    *(uintptr_t*)(sM + 8) = reinterpret_cast<uintptr_t>(stateM);
    g_liveLuaVms[L] = {addonSelf, 90, true};
    g_liveLuaVms[reinterpret_cast<uintptr_t>(stateB)] = {sB, 91, true};
    g_liveLuaVms[reinterpret_cast<uintptr_t>(stateC)] = {sC, 92, true};
    g_liveLuaVms[reinterpret_cast<uintptr_t>(stateM)] = {sM, 93, false}; // microcontroller VM
    // All six value types decode, with the ring's head offset honoured.
    std::vector<FakeValue> join = {Value(0), Value(1), Value(2), Value(3), Value(4), Value(5)};
    join[0].number = 2.5; join[1].i90 = -1; join[2].text = "Moi"; join[3].boolean = 1; join[4].i98 = 76561198000000000ll;
    assert(CallEvent(addonSelf, "onPlayerJoin", join, 2) == 0xabcd01);
    assert(events.size() == 1 && events[0].name == "onPlayerJoin" && events[0].state == state);
    const auto& v = events[0].values;
    assert(v.size() == 6 && v[0].type == SW_GAME_VALUE_NUMBER && v[0].number == 2.5);
    assert(v[1].type == SW_GAME_VALUE_INTEGER && v[1].integer == -1 && v[1].number == -1.0);
    assert(v[2].type == SW_GAME_VALUE_STRING && events[0].texts[2] == "Moi" && v[2].length == 3);
    assert(v[3].type == SW_GAME_VALUE_BOOLEAN && v[3].boolean);
    assert(v[4].type == SW_GAME_VALUE_INTEGER && v[4].integer == 76561198000000000ll);
    assert(v[5].type == SW_GAME_VALUE_TABLE);
    assert(*(uintptr_t*)(L + 0x10) == reinterpret_cast<uintptr_t>(stack + 1) && g_luaCallbackFrame == nullptr);
    // The same event dispatched to the other addon scripts is not delivered again...
    *(uintptr_t*)(reinterpret_cast<uintptr_t>(stateB) + 0x10) = *(uintptr_t*)(reinterpret_cast<uintptr_t>(stateB) + 0x38);
    assert(CallEvent(sB, "onPlayerJoin", join) == 0xabcd01 && CallEvent(sC, "onPlayerJoin", join) == 0xabcd01);
    assert(events.size() == 1);
    // ...but the same content dispatched again starts a new event (script repeats).
    assert(CallEvent(addonSelf, "onPlayerJoin", join) && events.size() == 2 && events[1].sequence == events[0].sequence + 1);
    CallEvent(sB, "onPlayerJoin", join);
    assert(events.size() == 2);
    // Different content is a different event, even for the next script in the round.
    join[2].text = "Other";
    CallEvent(sC, "onPlayerJoin", join);
    assert(events.size() == 3 && events[2].texts[2] == "Other" && events[2].state == stateC);
    // Microcontrollers, frame callbacks and unknown scripts never produce events.
    std::vector<FakeValue> tickArgs = {Value(1)};
    const int callsBefore = originalCalls;
    CallEvent(sM, "onPlayerJoin", join);
    CallEvent(addonSelf, "onTick", tickArgs);
    CallEvent(addonSelf, "onDraw", none);
    CallEvent(reinterpret_cast<uintptr_t>(addonScript) + 1, "onChatMessage", join);
    assert(events.size() == 3 && originalCalls == callsBefore + 4); // still forwarded to the game
    // Name filters, unsubscribe, crash isolation.
    SwHandle joinHandle = 0, eventCrash = 0;
    assert(swml::SubscribeGameEvent(&otherOwner, "onPlayerJoin", &OnlyJoin, nullptr, &joinHandle) == SW_OK);
    CallEvent(addonSelf, "onVehicleSpawn", tickArgs);
    assert(events.size() == 4 && filteredEvents == 0);
    join[2].text = "Third";
    CallEvent(addonSelf, "onPlayerJoin", join);
    assert(events.size() == 5 && filteredEvents == 1);
    assert(swml::UnsubscribeGameEvent(&owner, joinHandle) == SW_NOT_FOUND);
    assert(swml::UnsubscribeGameEvent(&otherOwner, joinHandle) == SW_OK);
    assert(swml::UnsubscribeGameEvent(&otherOwner, joinHandle) == SW_NOT_FOUND);
    assert(swml::SubscribeGameEvent(&otherOwner, "onChatMessage", &CrashEvent, nullptr, &eventCrash) == SW_OK);
    std::vector<FakeValue> chat = {Value(1), Value(2), Value(2)};
    chat[1].text = "Moi"; chat[2].text = "hello";
    CallEvent(addonSelf, "onChatMessage", chat);
    assert(events.size() == 6 && events[5].texts[2] == "hello" && g_luaCallbackFrame == nullptr && g_eventDepth == 0);
    assert(swml::UnsubscribeGameEvent(&otherOwner, eventCrash) == SW_NOT_FOUND); // disabled by its crash
    // A broken ring is skipped, never dereferenced past its bounds.
    EStr badName{const_cast<char*>("onPlayerLeave"), 13, 0};
    GameArgRing broken{nullptr, 0, 0, 3};
    HkCallByName(addonSelf, &badName, &broken);
    GameArgRing overfull{reinterpret_cast<uint8_t*>(join.data()), 2, 0, 3};
    HkCallByName(addonSelf, &badName, &overfull);
    assert(events.size() == 6);
    // Owner release stops delivery and keeps the subscription counter balanced.
    swml::ReleaseNativeLuaOwner(&owner);
    assert(g_eventSubscriptions.load() == 0);
    join[2].text = "After";
    CallEvent(addonSelf, "onPlayerJoin", join);
    assert(events.size() == 6);
    puts("PASS: game events; value decoding, one delivery per event, repeats, filters, MC/frame rejection, crash isolation, release");
    puts("PASS: simulation tick; argument forwarding, addon state selection, stack restore, self-unsubscribe, crash isolation, limits, release");
    puts("PASS: addon script VMs; scoped libraries, reserved names, QoL refusal, reload generations, lifecycle events");
    puts("PASS: getGlobal and protected call; context, bounds, types, raised error as status, truncated message");
    puts("PASS: real Lua wrappers; fake stack/string/table natives; owner/thread/VM lifetime; bounds/types; relocation; protected longjmp; inert unload tokens");
}
