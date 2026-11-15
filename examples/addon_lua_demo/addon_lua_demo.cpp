// A native library for ADDON/MISSION scripts (script.lua), without game addresses.
// The addon keeps using the game's own server.* API and calls into this DLL through
// the `sf_demo` global table. Requires SW_CAP_LUA_ADDON (newer reviewed build only).
#include "../../include/sw_mod_sdk.h"
#include <stdio.h>
#include <string.h>

static const SwModHost* g_host = nullptr;
static unsigned g_pings = 0;

// sf_demo.ping() -> "pong", count
static int Ping(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    if (!lua->checkStack(L, 2)) return 0;
    if (lua->extras->pushString(g_host, L, "pong", 4) != SW_LUA_OK) return 0;
    lua->pushNumber(L, ++g_pings);
    return 2;
}

// sf_demo.log(message) -> writes to StormworksModLoader.log
static int Log(void* L, void*) {
    char message[512];
    g_host->api->lua->argString(L, 1, message, sizeof(message));
    char line[560];
    snprintf(line, sizeof(line), "[addon] %s", message);
    g_host->log(g_host->modId, line);
    return 0;
}

// Pushes server.<name> (the game's own API function), leaving `server` below it.
static bool PushServerFunction(void* L, const char* name) {
    const SwLuaExtrasApi* x = g_host->api->lua->extras;
    if (x->getGlobal(g_host, L, "server") != SW_LUA_OK) return false;
    if (x->getField(g_host, L, -1, name) == SW_LUA_OK) return true;
    x->pop(g_host, L, 1);
    return false;
}

// sf_demo.announce(text) -> true/false. The DLL itself calls server.announce(...).
static int Announce(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    const SwLuaExtrasApi* x = lua->extras;
    char text[256], error[256];
    lua->argString(L, 1, text, sizeof(text));
    bool ok = false;
    if (lua->checkStack(L, 6) && PushServerFunction(L, "announce")) {
        x->pushString(g_host, L, "StormFabric native", 18);
        x->pushString(g_host, L, text, strlen(text));
        ok = x->call(g_host, L, 2, 0, error, sizeof(error)) == SW_LUA_OK;
        if (!ok) g_host->log(g_host->modId, error);
        x->pop(g_host, L, 1); // server
    }
    lua->pushBool(L, ok);
    return 1;
}

// sf_demo.hostName() -> name of peer 0, read by the DLL through server.getPlayerName(0).
static int HostName(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    const SwLuaExtrasApi* x = lua->extras;
    char name[128] = "", error[256];
    size_t length = 0;
    if (lua->checkStack(L, 6) && PushServerFunction(L, "getPlayerName")) {
        x->pushInteger(g_host, L, 0);
        if (x->call(g_host, L, 1, 1, error, sizeof(error)) == SW_LUA_OK) {
            x->copyString(g_host, L, -1, name, sizeof(name), &length);
            x->pop(g_host, L, 1); // result
        } else g_host->log(g_host->modId, error);
        x->pop(g_host, L, 1); // server
    }
    char line[200];
    snprintf(line, sizeof(line), "server.getPlayerName(0) called from C++ -> '%s'", name);
    g_host->log(g_host->modId, line);
    return x->pushString(g_host, L, name, strlen(name)) == SW_LUA_OK ? 1 : 0;
}

static void OnVm(const SwEvent* event, void*) {
    g_host->log(g_host->modId, event->type == SW_EVENT_ADDON_LUA_CREATED
        ? "Addon script VM created (sf_demo installed)" : "Addon script VM destroyed/reloaded");
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->lua) return false;
    const SwLuaApi* lua = host->api->lua;
    // registerAddonLibrary and extras are appended members: check before reading.
    if (lua->structSize < offsetof(SwLuaApi, registerAddonLibrary) + sizeof(lua->registerAddonLibrary) ||
        !lua->registerAddonLibrary || !lua->extras) return false;
    const SwRuntimeApi* runtime = host->api->runtime;
    if (!runtime || !(runtime->capabilities() & SW_CAP_LUA_ADDON)) {
        host->log(host->modId, "Addon Lua is not certified for this game build; nothing registered");
        return false;
    }
    g_host = host;
    SwLuaFunction functions[] = {{"ping", &Ping, nullptr}, {"log", &Log, nullptr},
                                 {"announce", &Announce, nullptr}, {"hostName", &HostName, nullptr}};
    // getGlobal/call are appended extras: without them only ping/log are offered.
    const bool canCall = lua->extras->structSize >= offsetof(SwLuaExtrasApi, call) + sizeof(lua->extras->call);
    if (!lua->registerAddonLibrary(host, "sf_demo", functions, canCall ? 4 : 2)) {
        g_host = nullptr;
        return false;
    }
    SwHandle subscription = 0;
    runtime->subscribe(host, SW_EVENT_MASK(SW_EVENT_ADDON_LUA_CREATED) |
                             SW_EVENT_MASK(SW_EVENT_ADDON_LUA_DESTROYED), &OnVm, nullptr, &subscription);
    host->log(host->modId, "Registered sf_demo.ping() and sf_demo.log(msg) for newly created addon VMs");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
