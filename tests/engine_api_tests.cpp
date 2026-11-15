// Unit tests use a fake game API: never start Stormworks or patch a game executable.
#include "../src/engine_api.h"
#include "../src/native_lua.h"
#include "../src/native_video.h"
#include "../src/engine_profile.h"
#include "../src/mod_runtime.h"
#include "../src/log.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
bool profileValid = true;
bool qolProfileValid = true;
bool addonProfileValid = false, addonInstalled = false;
bool tickProfileValid = false;
int tickSubscriptions = 0;
int addonRegistrations = 0;
bool luaEnabled = false;
const SwModHost* releasedOwner = nullptr;
int initCount = 0, clickCount = 0;
bool failLuaInit = false;
void LogStub(const char*, const char*) {}
void* New(size_t n) { return calloc(1, n); }
void Reserve(sw::EStr* s, uint32_t n) { s->data = static_cast<char*>(malloc(n)); s->cap = n; }
void Free(void* p) { free(p); }
void* Add(void*, void*, void* p) { return p; }
void* Button(void*, float, float, int, sw::EStr* ic, sw::EStr* lb, uint32_t, int, float, float) {
    free(ic->data); free(lb->data);
    return New(0x100);
}
void Click(void*, void*) { ++clickCount; }
bool RegisterLibrary(const SwModHost*, const char*, const SwLuaFunction*, size_t) { return true; }
bool RegisterAddon(const SwModHost*, const char*, const SwLuaFunction*, size_t) { ++addonRegistrations; return true; }
void Ignore(const SwEvent*, void*) {}
void IgnoreTick(const SwSimulationTick*, void*) {}
int Callback(void*, void*) { return 0; }
template<class T> T Read(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<char*>(p) + offset); }

// ABI v1 prefix: SDK v2 must not move any existing field.
struct V1Host {
    uint32_t sdkVersion;
    void* gameBase;
    const char *modId, *modName, *modVersion, *modAuthor, *modDir;
    void *uiContext, *mainMenuInterface;
    void (*log)(const char*, const char*);
};
static_assert(offsetof(SwModHost, log) == offsetof(V1Host, log), "SDK v1 log offset changed");
static_assert(offsetof(SwModHost, api) == sizeof(V1Host), "SDK v1 prefix changed");
static_assert(offsetof(SwEngineApi, video) == 24, "Engine API UI/Lua prefix changed");
}

namespace swml {
void Log(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogError(const char*, ...) {}
void LogWarn(const char*, ...) {}
void Notify(LogLevel, const std::string&, const std::string&, float) {}
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
bool ValidateLuaProfile(uint8_t*) { return profileValid; }
bool ValidateQolEditorProfile(uint8_t*) { return qolProfileValid; }
bool ValidateAddonLuaProfile(uint8_t*) { return addonProfileValid; }
bool NativeAddonLuaInstalled() { return addonInstalled; }
bool ValidateSimulationTickProfile(uint8_t*) { return tickProfileValid; }
SwResult SubscribeSimulationTick(const SwModHost*, SwSimulationTickCallback, void*, SwHandle* out) { ++tickSubscriptions; *out = 9; return SW_OK; }
SwResult UnsubscribeSimulationTick(const SwModHost*, SwHandle handle) { return handle == 9 ? SW_OK : SW_NOT_FOUND; }
bool EnableNativeLua(const SwModHost*) { ++initCount; return !failLuaInit; }
bool EnableNativeQolEditor(const SwModHost*) { return !failLuaInit; }
void DisableNativeLua() {}
void SetNativeLuaEnabled(bool enabled) { luaEnabled = enabled; }
void ReleaseNativeLuaOwner(const SwModHost* owner) { releasedOwner = owner; }
bool ValidateVideoProfile(uint8_t*) { return profileValid; }
bool InstallNativeVideo(uint8_t*) { return true; }
bool VideoEnable(const SwModHost*) { return true; }
void ReleaseNativeVideoOwner(const SwModHost*) {}
const SwVideoApi* NativeVideoPrimitives() {
    static SwVideoApi api = {sizeof(SwVideoApi),&VideoEnable};
    return &api;
}
const SwLuaApi* NativeLuaPrimitives() {
    static SwLuaApi api = {};
    api.structSize = sizeof(api);
    api.registerLibrary = &RegisterLibrary;
    api.registerAddonLibrary = &RegisterAddon;
    return &api;
}
const SwLuaExtrasApi* NativeLuaExtras() { static SwLuaExtrasApi api={sizeof(SwLuaExtrasApi)};return &api; }
}

int main() {
    sw::Api game;
    game.base = reinterpret_cast<uint8_t*>(1);
    game.op_new = &New;
    game.str_reserve = &Reserve;
    game.str_free = &Free;
    game.make_button = &Button;
    game.add_child = &Add;
    // Regression: legacy native hooks can invalidate Lua after early profile validation.
    // Video must refuse startup, then succeed after a fresh initialization without that conflict.
    swml::InitializeEngineApi(game);
    SwModHost startupOwner = {};
    startupOwner.log = &LogStub;
    swml::RegisterEngineApiOwner(&startupOwner);
    failLuaInit = true;
    assert(!swml::GetEngineApi()->video->enable(&startupOwner) && initCount == 1);
    swml::ReleaseEngineApiOwner(&startupOwner);
    failLuaInit = false;
    initCount = 0;
    swml::InitializeEngineApi(game);
    const SwEngineApi* api = swml::GetEngineApi();
    assert(api->abiVersion == SW_ENGINE_API_VERSION && api->structSize == sizeof(SwEngineApi));
    assert(api->ui && api->lua && api->ui->structSize == sizeof(SwUiApi));

    SwModHost a = {}, b = {}, unknown = {};
    a.log = b.log = &LogStub;
    a.api = b.api = api;
    void* ctx = reinterpret_cast<void*>(1);
    assert(!api->ui->button(&unknown, ctx, nullptr, 0, 0, 0, "new", "test", .14f, &Click, nullptr));
    assert(!api->lua->enableQol(&unknown));
    assert(api->video && !api->video->enable(&unknown));
    swml::RegisterEngineApiOwner(&a);
    swml::RegisterEngineApiOwner(&b);
    assert(api->video->enable(&a));
    void* widget = api->ui->button(&a, ctx, nullptr, 0, 0, 0, "new", "test", .14f, &Click, nullptr);
    assert(widget);
    auto thunk = Read<SwUiClickFn>(widget, sw::off::kBtnCallback);
    void* token = Read<void*>(widget, sw::off::kBtnUserData);
    thunk(ctx, token);
    assert(clickCount == 1);
    assert(api->lua->enableQol(&a) && luaEnabled && initCount == 1);
    assert(api->lua->enableQol(&b) && initCount == 1);
    api->lua->disableQol(&a);
    assert(luaEnabled); // b still owns the feature
    swml::ReleaseEngineApiOwner(&b);
    assert(!luaEnabled && releasedOwner == &b);
    swml::ReleaseEngineApiOwner(&a);
    thunk(ctx, token); // stale native UI still exists; plugin code must NOT be called
    assert(clickCount == 1 && releasedOwner == &a);
    assert(!api->lua->enableQol(&a));
    assert(!api->video->enable(&a));
    free(widget);

    // Address reuse must not resurrect the old token after hot-reload.
    swml::RegisterEngineApiOwner(&a);
    thunk(ctx, token);
    assert(clickCount == 1);
    SwLuaFunction functions[] = {{"test", &Callback, nullptr}};
    assert(api->lua->registerLibrary(&a, "example", functions, 1));
    assert(!luaEnabled); // custom libraries do not implicitly enable all QoL features
    swml::ReleaseEngineApiOwner(&a);

    // Core Lua stays available while the optional QoL/editor profile is rejected:
    // capability separation must not let editor targets gate custom libraries.
    qolProfileValid = false;
    swml::InitializeEngineApi(game);
    api = swml::GetEngineApi();
    assert(api->lua && !(swml::RuntimePrimitives()->capabilities() & SW_CAP_LUA_QOL));
    swml::RegisterEngineApiOwner(&a);
    assert(api->lua->registerLibrary(&a, "example", functions, 1));
    assert(!luaEnabled);
    assert(!api->lua->enableQol(&a)); // fail-closed: no editor hooks without a validated profile
    qolProfileValid = true;
    swml::InitializeEngineApi(game);
    api = swml::GetEngineApi();
    assert(swml::RuntimePrimitives()->capabilities() & SW_CAP_LUA_QOL);
    swml::RegisterEngineApiOwner(&a);
    assert(api->lua->enableQol(&a) && luaEnabled);
    swml::ReleaseEngineApiOwner(&a);

    // Addon/mission script VMs: optional capability of the newer build only. An
    // uncertified build, or hooks that failed to install, refuse the registration
    // and the events while microcontroller libraries keep working.
    const uint64_t addonMask = SW_EVENT_MASK(SW_EVENT_ADDON_LUA_CREATED) | SW_EVENT_MASK(SW_EVENT_ADDON_LUA_DESTROYED);
    SwHandle subscription = 0;
    assert(!(swml::RuntimePrimitives()->capabilities() & SW_CAP_LUA_ADDON));
    swml::RegisterEngineApiOwner(&a);
    assert(!api->lua->registerAddonLibrary(&a, "example", functions, 1) && addonRegistrations == 0);
    assert(api->runtime->subscribe(&a, addonMask, &Ignore, nullptr, &subscription) == SW_UNAVAILABLE);
    assert(api->lua->registerLibrary(&a, "example", functions, 1));
    swml::ReleaseEngineApiOwner(&a);
    addonProfileValid = true;
    swml::InitializeEngineApi(game);
    api = swml::GetEngineApi();
    assert(swml::RuntimePrimitives()->capabilities() & SW_CAP_LUA_ADDON);
    swml::RegisterEngineApiOwner(&a);
    assert(!api->lua->registerAddonLibrary(&unknown, "example", functions, 1));
    assert(!api->lua->registerAddonLibrary(&a, "example", functions, 1) && addonRegistrations == 0); // hooks not installed
    assert(api->runtime->subscribe(&a, addonMask, &Ignore, nullptr, &subscription) == SW_UNAVAILABLE);
    addonInstalled = true;
    assert(api->lua->registerAddonLibrary(&a, "example", functions, 1) && addonRegistrations == 1);
    assert(api->runtime->subscribe(&a, addonMask, &Ignore, nullptr, &subscription) == SW_OK && subscription);
    assert(!luaEnabled); // addon libraries do not implicitly enable QoL
    swml::ReleaseEngineApiOwner(&a);

    // Simulation tick: optional capability; refused without a validated profile.
    SwHandle tick = 0;
    assert(api->runtime->structSize == sizeof(SwRuntimeApi) && api->runtime->subscribeSimulationTick);
    assert(!(swml::RuntimePrimitives()->capabilities() & SW_CAP_SIMULATION_TICK));
    swml::RegisterEngineApiOwner(&a);
    assert(api->runtime->subscribeSimulationTick(&a, &IgnoreTick, nullptr, &tick) == SW_UNAVAILABLE && tickSubscriptions == 0);
    swml::ReleaseEngineApiOwner(&a);
    tickProfileValid = true;
    swml::InitializeEngineApi(game);
    api = swml::GetEngineApi();
    assert(swml::RuntimePrimitives()->capabilities() & SW_CAP_SIMULATION_TICK);
    assert(!(swml::RuntimePrimitives()->capabilities() & SW_CAP_SIMULATION_EVENTS)); // no world/time claim
    assert(api->runtime->subscribeSimulationTick(&unknown, &IgnoreTick, nullptr, &tick) == SW_INVALID_OWNER);
    swml::RegisterEngineApiOwner(&a);
    assert(api->runtime->subscribeSimulationTick(&a, nullptr, nullptr, &tick) == SW_INVALID_ARGUMENT);
    assert(api->runtime->subscribeSimulationTick(&a, &IgnoreTick, nullptr, &tick) == SW_OK && tick == 9 && tickSubscriptions == 1);
    assert(api->runtime->unsubscribeSimulationTick(&unknown, tick) == SW_INVALID_OWNER);
    assert(api->runtime->unsubscribeSimulationTick(&a, tick) == SW_OK);
    swml::ReleaseEngineApiOwner(&a);

    profileValid = false;
    swml::InitializeEngineApi(game);
    assert(swml::GetEngineApi()->ui && !swml::GetEngineApi()->lua && !swml::GetEngineApi()->video);
    assert(!(swml::RuntimePrimitives()->capabilities() & SW_CAP_SIMULATION_TICK));
    assert(!(swml::RuntimePrimitives()->capabilities() & SW_CAP_LUA_ADDON));
    puts("PASS: SDK v1 prefix, C API layout, owner validation, inert callbacks, hot-reload token isolation, QoL ownership, addon Lua gating, simulation tick gating, profile rejection");
}
