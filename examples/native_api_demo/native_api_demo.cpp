// A custom microcontroller Lua library, without game addresses or structure offsets.
#include "../../include/sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

static int Sum(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    double sum = 0;
    for (int i = 1; i <= lua->getTop(L); ++i) sum += lua->argNumber(L, i, 0);
    if (!lua->checkStack(L, 1)) return 0;
    lua->pushNumber(L, sum);
    return 1;
}

static int Clamp(void* L, void*) {
    return g_host->api->lua->qol->clamp(L);
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->lua) return false;
    g_host = host;
    SwLuaFunction functions[] = {{"sum", &Sum, nullptr}, {"clamp", &Clamp, nullptr}};
    if (!host->api->lua->registerLibrary(host, "sw_demo", functions, 2)) {
        g_host = nullptr;
        return false;
    }
    host->log(host->modId, "Registered sw_demo.sum(...) and sw_demo.clamp(x,min,max) for new Lua VMs");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
