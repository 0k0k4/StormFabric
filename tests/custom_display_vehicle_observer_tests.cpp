// Actual pass-through wrappers, fake natives. No game hooks or game execution.
#include "../src/custom_display_vehicle_observer.cpp"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool throwLog = false;
unsigned initCalls = 0, updateCalls = 0, uiTeardownCalls = 0, destroyCalls = 0;
unsigned stateTeardownCalls = 0, actionCalls = 0, enterCalls = 0, leaveCalls = 0;
DWORD thread = 0;
void *selfExpected = nullptr, *contextExpected = nullptr, *aExpected = nullptr, *cExpected = nullptr;
uintptr_t dExpected = 0;
int32_t actionExpected = 0;
uint32_t flagsExpected = 0;
std::vector<std::string> phases;
void InitNative(void* self, void* context) {
    assert(self == selfExpected && context == contextExpected && GetCurrentThreadId() == thread);
    ++initCalls; phases.push_back("native.init");
}
void UpdateNative(void* self, void* a, void* context, void* c) {
    assert(self == selfExpected && context == contextExpected && a == aExpected && c == cExpected);
    assert(GetCurrentThreadId() == thread); ++updateCalls;
}
void UiTeardownNative(void* self, void* context) {
    assert(self == selfExpected && context == contextExpected && GetCurrentThreadId() == thread);
    ++uiTeardownCalls; phases.push_back("native.ui.teardown");
}
void* DestroyNative(void* self, uint32_t flags) {
    assert(self == selfExpected && flags == flagsExpected && GetCurrentThreadId() == thread);
    ++destroyCalls; phases.push_back("native.destroy");
    return reinterpret_cast<void*>(0xfedcba9876543210ull);
}
void StateTeardownNative(void* self) {
    assert(self == selfExpected && GetCurrentThreadId() == thread);
    ++stateTeardownCalls; phases.push_back("native.state.teardown");
}
void ActionsNative(void* self, void* a, void* b, void* c, uintptr_t d, int32_t action) {
    assert(self == selfExpected && a == aExpected && b == contextExpected && c == cExpected);
    assert(d == dExpected && action == actionExpected && GetCurrentThreadId() == thread);
    ++actionCalls; phases.push_back("native.action");
}
void EnterNative(void* self, void* a, void* b, void* c) {
    assert(self == selfExpected && a == aExpected && b == contextExpected && c == cExpected);
    assert(GetCurrentThreadId() == thread); ++enterCalls; phases.push_back("native.enter");
}
void LeaveNative(void* self, void* a, void* b, void* c) {
    assert(self == selfExpected && a == aExpected && b == contextExpected && c == cExpected);
    assert(GetCurrentThreadId() == thread); ++leaveCalls; phases.push_back("native.leave");
}
}
namespace swml {
void LogInfo(const char* format, ...) {
    if (throwLog) throw std::runtime_error("logger failed");
    char buffer[1024]; va_list args; va_start(args, format);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args); va_end(args);
    phases.push_back(buffer);
}
void LogError(const char*, ...) {}
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
void GetLastDialogValues(int& w, int& h, int& px, int& py) { w = 1; h = 1; px = 32; py = 32; }
void SetLastDialogValues(int, int, int, int) {}
std::vector<std::pair<std::string, std::pair<int, int>>> GetAllRegisteredDisplays() { return {}; }
}

int main() {
    using namespace swml;
    SetEnvironmentVariableA("SWML_CD_VEHICLE_OBSERVE", nullptr);
    assert(!InstallCustomDisplayVehicleObserver(nullptr));
    SetEnvironmentVariableA("SWML_CD_VEHICLE_OBSERVE", "true");
    assert(!InstallCustomDisplayVehicleObserver(nullptr));
    SetEnvironmentVariableA("SWML_CD_VEHICLE_OBSERVE", "1");
    assert(!InstallCustomDisplayVehicleObserver(nullptr)); // Auth gate, no fallback.
    SetEnvironmentVariableA("SWML_CD_VEHICLE_OBSERVE", nullptr);

    std::vector<uint8_t> object(0x400), context(0x400);
    layout.widgets.parent = 0x28; layout.widgets.contextModal = 0x1b0;
    layout.stateRoot = 0x100; layout.widgets.rootContext = 0x50;
    void* parent = reinterpret_cast<void*>(0x1234), *modal = reinterpret_cast<void*>(0x5678);
    memcpy(object.data() + layout.widgets.parent, &parent, sizeof(parent));
    memcpy(context.data() + layout.widgets.contextModal, &modal, sizeof(modal));
    const auto beforeObject = object, beforeContext = context;
    selfExpected = object.data(); contextExpected = context.data();
    aExpected = reinterpret_cast<void*>(0x1234567812345678ull);
    cExpected = reinterpret_cast<void*>(0x9876543298765432ull);
    dExpected = 0xabcdef0123456789ull; thread = GetCurrentThreadId();
    originalInit = &InitNative; originalUpdate = &UpdateNative;
    originalUiTeardown = &UiTeardownNative; originalDestroy = &DestroyNative;
    originalStateTeardown = &StateTeardownNative; originalActions = &ActionsNative;
    originalEnter = &EnterNative; originalLeave = &LeaveNative;
    for (bool logFails : {false, true}) {
        throwLog = logFails;
        auto count = initCalls; Init(selfExpected, contextExpected); assert(initCalls == count + 1);
        count = updateCalls; Update(selfExpected, aExpected, contextExpected, cExpected);
        assert(updateCalls == count + 1);
        count = uiTeardownCalls; UiTeardown(selfExpected, contextExpected); assert(uiTeardownCalls == count + 1);
        for (uint32_t flags : {0u, 1u, UINT32_MAX}) {
            flagsExpected = flags; count = destroyCalls;
            assert(Destroy(selfExpected, flags) == reinterpret_cast<void*>(0xfedcba9876543210ull));
            assert(destroyCalls == count + 1);
        }
        count = stateTeardownCalls; StateTeardown(selfExpected); assert(stateTeardownCalls == count + 1);
        count = enterCalls; Enter(selfExpected, aExpected, contextExpected, cExpected); assert(enterCalls == count + 1);
        count = leaveCalls; Leave(selfExpected, aExpected, contextExpected, cExpected); assert(leaveCalls == count + 1);
        for (int32_t action : {0, 0x61, 0x1e, -1, INT32_MIN, INT32_MAX}) {
            actionExpected = action; count = actionCalls;
            Actions(selfExpected, aExpected, contextExpected, cExpected, dExpected, action);
            assert(actionCalls == count + 1);
        }
        assert(object == beforeObject && context == beforeContext);
    }
    throwLog = false;
    // Sample cap never blocks native frames or lifecycle calls.
    events.store(maxEvents); auto count = updateCalls;
    for (int i = 0; i < 1000; ++i) Update(selfExpected, aExpected, contextExpected, cExpected);
    assert(updateCalls == count + 1000 && events.load() == maxEvents);
    auto logs = phases.size(); count = initCalls;
    Init(selfExpected, contextExpected);
    assert(initCalls == count + 1 && phases.size() == logs + 1); // Native only.
    logs = phases.size(); count = enterCalls;
    Enter(selfExpected, aExpected, contextExpected, cExpected);
    assert(enterCalls == count + 1 && phases.size() == logs + 1);
    logs = phases.size(); count = leaveCalls;
    Leave(selfExpected, aExpected, contextExpected, cExpected);
    assert(leaveCalls == count + 1 && phases.size() == logs + 1);
    events.store(0);
    phases.clear(); Init(selfExpected, contextExpected);
    assert(phases.size() == 3 && phases[0].find("phase=ui.init.before") != std::string::npos &&
        phases[1] == "native.init" && phases[2].find("phase=ui.init.after") != std::string::npos);
    // Invalid memory remains diagnostic failure; original still receives EXACT args.
    contextExpected = reinterpret_cast<void*>(1);
    count = initCalls; Init(selfExpected, contextExpected); assert(initCalls == count + 1);
    assert(phases.back().find("valid=0") != std::string::npos);
    puts("PASS: vehicle UI pass-through wrappers, eight original argument/return contracts, same-thread calls, no writes, logger failure/cap/unreadable-context tolerance (fake natives only)");
}
