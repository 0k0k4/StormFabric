// Publishes the "demo.counter" service for other mods (see demo_service.h).
#include "../../include/sw_mod_sdk.h"
#include "demo_service.h"
#include <atomic>

static std::atomic<int64_t> g_counter{0};
static int64_t Add(int64_t amount) { return g_counter += amount; }
static const char* ProviderName(void) { return "service_provider"; }
// Static storage: the loader hands this exact table to consumers, it is not copied.
static const DemoCounterService kService = {sizeof(DemoCounterService), &Add, &ProviderName};

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION) return false;
    const SwEngineApi* api = host->api;
    if (api->structSize < offsetof(SwEngineApi, services) + sizeof(api->services) || !api->services) {
        host->log(host->modId, "services unavailable on this loader");
        return false;
    }
    if (api->services->publish(host, DEMO_COUNTER_SERVICE, DEMO_COUNTER_VERSION, &kService, sizeof(kService)) != SW_OK) {
        host->log(host->modId, "publish failed");
        return false;
    }
    host->log(host->modId, "published " DEMO_COUNTER_SERVICE " v1.0");
    return true; // withdrawn automatically when this mod unloads
}

SW_MOD_EXPORT void SwModUnload(void) {}
