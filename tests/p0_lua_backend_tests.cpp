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
    puts("PASS: real Lua wrappers; fake stack/string/table natives; owner/thread/VM lifetime; bounds/types; relocation; protected longjmp; inert unload tokens");
}
