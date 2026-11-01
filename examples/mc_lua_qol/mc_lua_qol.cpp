// The loader owns the version-specific Lua hooks, structures and callbacks.
#include "../../include/sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->lua) return false;
    g_host = host;
    if (!host->api->lua->enableQol(host)) {
        g_host = nullptr;
        return false;
    }
    host->log(host->modId, "Lua QoL enabled through the loader API (no game addresses in this mod)");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {
    if (g_host) g_host->api->lua->disableQol(g_host);
    g_host = nullptr;
}
