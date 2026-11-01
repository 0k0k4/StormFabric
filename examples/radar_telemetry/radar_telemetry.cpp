// Mod dépendant de `sw_core_lib` (déclaré dans `"dependencies": ["sw_core_lib"]` de mod.json).
// Vérifie à l'initialisation que `sw_core_lib.dll` est bien chargée avant lui et utilise SwModTick.
#include <windows.h>
#include <cstdio>

#include "../../include/sw_mod_sdk.h"

namespace {
const SwModHost* g_host = nullptr;
bool g_firstTickLogged = false;
}  // namespace

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    g_host = host;
    g_firstTickLogged = false;

    HMODULE hCore = GetModuleHandleW(L"sw_core_lib.dll");
    if (!hCore) {
        if (host && host->log) {
            host->log(host->modId, "ERREUR: dependance sw_core_lib.dll non trouvee en memoire !");
        }
        return false;
    }

    using RegisterFn = int (*)(const char*);
    auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(hCore, "SwCoreRegisterClient"));
    int slot = reg ? reg(host->modId) : -1;

    if (host && host->log) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "initialise avec succes (v%s par %s, lie a sw_core_lib slot #%d)",
                 host->modVersion, host->modAuthor, slot);
        host->log(host->modId, msg);
    }
    return true;
}

SW_MOD_EXPORT void SwModTick(void* uiContext) {
    if (!g_firstTickLogged && g_host && g_host->log) {
        g_firstTickLogged = true;
        char msg[128];
        snprintf(msg, sizeof(msg), "premier SwModTick recu (uiContext=%p)", uiContext);
        g_host->log("radar_telemetry", msg);
    }
}

SW_MOD_EXPORT void SwModUnload(void) {
    if (g_host && g_host->log) {
        g_host->log("radar_telemetry", "dechargement propre (SwModUnload)");
    }
    g_host = nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
