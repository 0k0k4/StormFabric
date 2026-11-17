// Uses the "demo.counter" service. mod.json lists "service_provider" in "dependencies": the
// loader then loads the provider first, unloads it last, and lets this mod get its table.
#include "../../include/sw_mod_sdk.h"
#include "demo_service.h"
#include <stdio.h>

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION) return false;
    const SwEngineApi* api = host->api;
    if (api->structSize < offsetof(SwEngineApi, services) + sizeof(api->services) || !api->services) return false;
    const void* table = nullptr;
    uint32_t version = 0;
    const SwResult r = api->services->get(host, DEMO_COUNTER_SERVICE, DEMO_COUNTER_VERSION, &table, &version, nullptr);
    if (r != SW_OK) {
        char text[96];
        snprintf(text, sizeof(text), "service " DEMO_COUNTER_SERVICE " unavailable (result %d)", int(r));
        host->log(host->modId, text);
        return false;
    }
    const auto* counter = static_cast<const DemoCounterService*>(table);
    // Members appended later must be checked against structSize before use.
    if (counter->structSize < offsetof(DemoCounterService, providerName) + sizeof(counter->providerName)) return false;
    char text[128];
    snprintf(text, sizeof(text), "%s says: counter = %lld (service v%u.%u)", counter->providerName(),
             static_cast<long long>(counter->add(42)), SW_SERVICE_MAJOR(version), SW_SERVICE_MINOR(version));
    host->log(host->modId, text);
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {}
