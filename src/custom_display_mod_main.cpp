#include "../include/sw_mod_sdk.h"
#include "custom_display_observer.h"
#include "custom_display_vehicle_observer.h"
#include "log.h"
#include <windows.h>

using namespace swml;

static const SwModHost* g_host = nullptr;

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || !host->gameBase) {
        if (host && host->log) host->log(host->modId, "ERROR: Cannot initialize (host or gameBase is null)");
        return false;
    }
    g_host = host;
    LogInit();
    uint8_t* base = static_cast<uint8_t*>(host->gameBase);
    bool ok1 = swml::InstallCustomDisplayVehicleObserver(base);
    bool ok2 = swml::InstallCustomDisplayObserver(base);
    char buf[128];
    snprintf(buf, sizeof(buf), "Observer installation: veh=%d, obs=%d", ok1, ok2);
    if (host->log) host->log(host->modId, buf);
    if (!ok1 || !ok2) {
        if (host->log) host->log(host->modId, "ERROR: Observer hook installation failed");
        return false;
    }
    if (host->log) {
        host->log(host->modId, "INFO: Custom Display C++ mod loaded successfully!");
    }
    return true;
}

SW_MOD_EXPORT void SwModTick(void* uiContext) {
    // Called each frame on main thread
}

SW_MOD_EXPORT void SwModUnload(void) {
    LogInfo("CustomDisplay: Unloading C++ mod via SwModUnload");
    swml::UninstallCustomDisplayVehicleObserver();
    swml::UninstallCustomDisplayObserver();
    if (g_host && g_host->log) {
        g_host->log(g_host->modId, "Custom Display C++ mod unloaded");
    }
    g_host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
