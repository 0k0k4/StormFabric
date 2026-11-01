// Native SDK test mod: no game addresses, raw native handles or external overlay.
#include "../../include/sw_mod_sdk.h"
#include <cstdio>
#include <cstring>
#include <atomic>
namespace {
const SwModHost* host = nullptr;
SwHandle subscription = 0, action = 0, timer = 0, deferred = 0;
SwHandle roots[3] = {}, labels[3] = {};
std::atomic<unsigned> passed{0}, failed{0};
unsigned luaCreated = 0, luaDestroyed = 0, timerCalls = 0;
bool visible = true;
double nextRefresh = 0;

void Log(const char* text) { host->log(host->modId, text); }
void Check(bool ok, const char* name) {
    char text[256]; sprintf_s(text, "%s: %s", ok ? "PASS" : "FAIL", name);
    if (ok) ++passed; else ++failed;
    Log(text);
}
void Refresh() {
    char text[256]; sprintf_s(text, "PASS %u / FAIL %u | Lua +%u -%u\nF8: toggle HUD | button: rerun checks",
                            passed.load(), failed.load(), luaCreated, luaDestroyed);
    for (unsigned i = 1; i <= 2; ++i)
        if (labels[i]) host->api->nativeUi->setText(host, labels[i], text);
}
void Unexpected(void*) { Check(false, "cancelled task must not execute"); }
void Repeating(void*) {
    ++timerCalls;
    Check(host->api->runtime->isGameThread(), "repeating task on game/UI thread");
    if (timerCalls == 3) {
        Check(host->api->runtime->cancelTask(host, timer) == SW_OK, "cancel repeating timer inside callback");
        timer = 0;
        Refresh();
    }
}
void RunChecks(void*) {
    const auto* runtime = host->api->runtime;
    const auto* storage = host->api->storage;
    Check(runtime->isGameThread(), "deferred task on game/UI thread");
    Check(runtime->cancelTask(host, deferred) == SW_NOT_FOUND, "one-shot task already retired");
    const uint8_t bytes[] = {0, 42, 255, 0}; uint8_t out[4] = {}; size_t size = 0;
    Check(storage->writeData(host, SW_STORAGE_MOD_DATA, "p0_selftest", bytes, sizeof(bytes)) == SW_STORAGE_OK, "binary data write");
    Check(storage->readData(host, SW_STORAGE_MOD_DATA, "p0_selftest", nullptr, 0, &size) == SW_STORAGE_BUFFER_TOO_SMALL && size == 4, "storage size query");
    out[0] = 99;
    Check(storage->readData(host, SW_STORAGE_MOD_DATA, "p0_selftest", out, 1, &size) == SW_STORAGE_BUFFER_TOO_SMALL && out[0] == 99, "short buffer untouched");
    Check(storage->readData(host, SW_STORAGE_MOD_DATA, "p0_selftest", out, sizeof(out), &size) == SW_STORAGE_OK && size == 4 && !memcmp(bytes, out, 4), "binary roundtrip including NUL");
    Check(storage->writeConfig(host, "p0_selftest", bytes, sizeof(bytes)) == SW_STORAGE_OK, "config write");
    Check(storage->readConfig(host, "p0_selftest", out, sizeof(out), &size) == SW_STORAGE_OK && size == 4 && !memcmp(bytes, out, 4), "config read");
    Check(storage->writeData(host, SW_STORAGE_MOD_DATA, "p0_empty", nullptr, 0) == SW_STORAGE_OK &&
          storage->readData(host, SW_STORAGE_MOD_DATA, "p0_empty", nullptr, 0, &size) == SW_STORAGE_OK && size == 0, "empty data roundtrip");
    Check(storage->writeConfig(host, "../escape", bytes, 4) == SW_STORAGE_INVALID_KEY, "path traversal rejected");
    Check(storage->readConfig(nullptr, "p0_selftest", out, 4, &size) == SW_STORAGE_INVALID_OWNER, "unregistered storage owner rejected");
    SwHandle cancelled = 0;
    Check(runtime->runOnGameThread(host, &Unexpected, nullptr, &cancelled) == SW_OK &&
          runtime->cancelTask(host, cancelled) == SW_OK && runtime->cancelTask(host, cancelled) == SW_NOT_FOUND, "cancel queued task and reject stale handle");
    SwHandle temporaryAction = 0;
    Check(host->api->input->registerAction(host, "selftest_temporary", &temporaryAction) == SW_OK &&
          host->api->input->unregisterAction(host, temporaryAction) == SW_OK &&
          host->api->input->unregisterAction(host, temporaryAction) == SW_NOT_FOUND, "action registration/removal/stale handle");
    double simulation = 123;
    if (!(runtime->capabilities() & SW_CAP_SIMULATION_EVENTS)) {
        Check(runtime->getSimulationTime(&simulation) == SW_UNAVAILABLE && simulation == 123, "unavailable simulation time leaves output untouched");
        Log("SKIP: simulation ticks/world/entities/vehicles/players: native lifetimes not certified");
    }
    if (!(runtime->capabilities() & SW_CAP_WORLD_STORAGE)) {
        Check(storage->writeData(host, SW_STORAGE_WORLD_DATA, "p0_selftest", bytes, 4) == SW_STORAGE_UNAVAILABLE, "world storage explicitly unavailable");
        Log("SKIP: persistence attached to a game save");
    }
    if (timer) runtime->cancelTask(host, timer);
    timerCalls = 0;
    Check(runtime->schedule(host, .25, .25, &Repeating, nullptr, &timer) == SW_OK, "schedule repeating timer");
    host->api->nativeUi->showNotification(host, failed ? 3 : 1, "P0 SDK self-test", failed ? "Some checks failed: inspect loader log" : "Checks passed; timer and Lua checks remain separate");
    Refresh();
}
void QueueChecks() {
    if (deferred) host->api->runtime->cancelTask(host, deferred);
    if (host->api->runtime->runOnGameThread(host, &RunChecks, nullptr, &deferred) != SW_OK)
        Check(false, "queue runtime checks");
}
void Click(const SwEvent* event, void*) {
    Check(event->type == SW_EVENT_UI_CLICK, "native button click event");
    QueueChecks();
}
void MakePanel(SwUiSurface surface) {
    auto* ui = host->api->nativeUi;
    unsigned i = static_cast<unsigned>(surface);
    if (ui->createHudRoot(host, surface, &roots[i]) != SW_OK) return;
    SwHandle window = 0, button = 0;
    SwUiLayout panel = {.06f, -.12f, .76f, .33f, 0};
    if (ui->createWindow(host, roots[i], &panel, "P0 SDK test panel", &window) != SW_OK) {
        ui->destroy(host, roots[i]); roots[i] = 0; return;
    }
    SwUiLayout text = {.02f, -.07f, .71f, .12f, 0};
    Check(ui->text(host, window, &text, "Waiting for tests", 0xffffffff, &labels[i]) == SW_OK, "native text creation");
    SwUiLayout buttonLayout = {.02f, -.23f, .40f, .065f, 0};
    Check(ui->button(host, window, &buttonLayout, "Run SDK checks", &Click, nullptr, &button) == SW_OK, "native button creation");
    SwHandle disposable = 0;
    Check(ui->text(host, window, &text, "temporary", 0xffffffff, &disposable) == SW_OK &&
          ui->destroy(host, disposable) == SW_OK && ui->setText(host, disposable, "stale") == SW_NOT_FOUND, "native destruction/stale handle rejected");
    ui->setVisible(host, roots[i], visible);
    Refresh();
}
void OnEvent(const SwEvent* event, void*) {
    if (event->type == SW_EVENT_LUA_CREATED) { ++luaCreated; Log("Lua VM created (opaque generation; no stack access here)"); return; }
    if (event->type == SW_EVENT_LUA_DESTROYED) { ++luaDestroyed; Log("Lua VM destroyed/reinitialized"); return; }
    unsigned surface = static_cast<unsigned>(event->surface);
    if (event->type == SW_EVENT_UI_CLOSED && surface < 3) { roots[surface] = labels[surface] = 0; return; }
    if (event->type == SW_EVENT_ACTION && event->handle == action && event->value) {
        SwActionState state = {};
        Check(host->api->input->getActionState(host, action, &state) == SW_OK && state.pressed && state.down, "F8 action pressed snapshot");
        visible = !visible;
        for (unsigned i = 1; i <= 2; ++i) if (roots[i]) host->api->nativeUi->setVisible(host, roots[i], visible);
        uint8_t saved = visible ? 1 : 0;
        Check(host->api->storage->writeConfig(host, "hud_visible", &saved, 1) == SW_STORAGE_OK, "persist visibility (reload mod to check)");
        return;
    }
    if (surface != SW_SURFACE_HUD && surface != SW_SURFACE_EDITOR) return;
    if (event->type == SW_EVENT_UI_READY || event->type == SW_EVENT_UI_UPDATE) {
        if (!roots[surface]) MakePanel(event->surface);
        if (event->monotonicSeconds >= nextRefresh) { nextRefresh = event->monotonicSeconds + 1; Refresh(); }
    }
}

// Called ONLY by a registered microcontroller Lua closure. Exercises real native
// string/table operations and restores the caller's stack before returning results.
int LuaSelfTest(void* L, void*) {
    auto* lua = host->api->lua; auto* extras = lua->extras;
    int base = lua->getTop(L); unsigned before = failed.load();
    const char binary[] = {'A', '\0', 'B'}; char out[8] = {}; size_t length = 0;
    SwLuaType type = SW_LUA_TYPE_NONE;
    bool ok = extras->pushString(host, L, binary, 3) == SW_LUA_OK;
    Check(ok, "Lua push binary string");
    if (ok) {
        Check(extras->copyString(host, L, -1, out, sizeof(out), &length) == SW_LUA_OK && length == 3 && !memcmp(binary, out, 3), "Lua binary string copy");
        Check(extras->getType(host, L, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_STRING, "Lua string type");
    }
    ok = extras->pushInteger(host, L, INT64_C(9007199254740993)) == SW_LUA_OK;
    Check(ok && extras->getType(host, L, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_INTEGER, "Lua integer distinct from double");
    ok = extras->pushNil(host, L) == SW_LUA_OK;
    Check(ok && extras->getType(host, L, -1, &type) == SW_LUA_OK && type == SW_LUA_TYPE_NIL, "Lua nil type");
    ok = extras->createTable(host, L, 4) == SW_LUA_OK;
    Check(ok, "Lua table allocation");
    if (ok) {
        int table = lua->getTop(L);
        ok = extras->pushString(host, L, "roundtrip", 9) == SW_LUA_OK;
        if (ok) ok = extras->setField(host, L, table, "message") == SW_LUA_OK;
        if (ok) ok = extras->getField(host, L, table, "message") == SW_LUA_OK;
        if (ok) ok = extras->copyString(host, L, -1, out, sizeof(out), &length) == SW_LUA_BUFFER_TOO_SMALL && length == 9;
        Check(ok, "Lua set/get field and bounded copy");
    }
    Check(extras->raiseError(host, L, "not raised", 10) == SW_LUA_UNSUPPORTED, "unsafe Lua longjmp API explicitly unsupported");
    Check(extras->pop(host, L, lua->getTop(L) - base) == SW_LUA_OK && lua->getTop(L) == base, "Lua stack restored");
    if (!lua->checkStack(L, 2)) return 0;
    lua->pushBool(L, failed.load() == before);
    const char* message = failed.load() == before ? "P0 Lua checks passed" : "P0 Lua checks failed: inspect loader log";
    if (extras->pushString(host, L, message, strlen(message)) != SW_LUA_OK) return 1;
    return 2;
}
}
SW_MOD_EXPORT bool SwModInit(const SwModHost* h) {
    if (!h || h->sdkVersion < 2 || !h->api || h->api->abiVersion != SW_ENGINE_API_VERSION ||
        h->api->structSize < offsetof(SwEngineApi, world) + sizeof(h->api->world) ||
        !h->api->runtime || !h->api->input || !h->api->nativeUi || !h->api->storage ||
        h->api->runtime->structSize < sizeof(SwRuntimeApi) || h->api->input->structSize < sizeof(SwInputApi) ||
        h->api->nativeUi->structSize < sizeof(SwNativeUiApi) || h->api->storage->structSize < sizeof(SwStorageApi)) return false;
    host = h;
    passed = 0; failed = 0; luaCreated = luaDestroyed = timerCalls = 0;
    visible = true; nextRefresh = 0;
    uint8_t value = 1; size_t size = 0;
    if (host->api->storage->readConfig(host, "hud_visible", &value, 1, &size) == SW_STORAGE_OK && size == 1) {
        visible = value != 0; Log("Saved HUD visibility loaded; F8 toggles it");
    }
    uint64_t mask = SW_EVENT_MASK(SW_EVENT_UI_READY) | SW_EVENT_MASK(SW_EVENT_UI_UPDATE) |
                    SW_EVENT_MASK(SW_EVENT_UI_CLOSED) | SW_EVENT_MASK(SW_EVENT_ACTION);
    if (h->api->runtime->capabilities() & SW_CAP_LUA_EVENTS)
        mask |= SW_EVENT_MASK(SW_EVENT_LUA_CREATED) | SW_EVENT_MASK(SW_EVENT_LUA_DESTROYED);
    if (h->api->runtime->subscribe(host, mask, &OnEvent, nullptr, &subscription) != SW_OK) return false;
    if (h->api->input->registerAction(host, "toggle_hud", &action) != SW_OK) return false;
    Check(h->api->input->bindKey(host, action, 0x77, 0) == SW_OK, "bind F8 (not consumed by game)");
    if (h->api->lua && h->api->lua->structSize >= offsetof(SwLuaApi, extras) + sizeof(h->api->lua->extras) &&
        h->api->lua->extras && h->api->lua->extras->structSize >= sizeof(SwLuaExtrasApi)) {
        SwLuaFunction functions[] = {{"run", &LuaSelfTest, nullptr}};
        Check(h->api->lua->registerLibrary(host, "p0_test", functions, 1), "register p0_test.run for NEW Lua VMs");
    } else Log("SKIP: Lua extras unavailable");
    QueueChecks();
    Log("P0 test mod enabled. Load a world for HUD; use demo.lua in a NEW microcontroller VM for Lua tests.");
    return true;
}
SW_MOD_EXPORT void SwModUnload() {
    // Owner and resources are revoked by loader before this export.
    host = nullptr; subscription = action = timer = deferred = 0;
    for (unsigned i = 0; i < 3; ++i) roots[i] = labels[i] = 0;
}
