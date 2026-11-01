#include "../include/sw_mod_sdk.h"

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    return host && host->sdkVersion >= 2 && host->api &&
           host->api->abiVersion == SW_ENGINE_API_VERSION;
}
