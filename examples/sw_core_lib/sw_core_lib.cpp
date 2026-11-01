// Mod principal de sw_core_lib : importe math_dep.dll depuis son propre sous-dossier
// et expose un service C (SwCoreRegisterClient) aux autres mods qui dépendent de sw_core_lib.
#include <windows.h>
#include <cstdio>

#include "../../include/sw_mod_sdk.h"

extern "C" __declspec(dllimport) int SwMathComputeChecksum(int a, int b);

namespace {
const SwModHost* g_host = nullptr;
int g_registeredClients = 0;
}  // namespace

SW_MOD_EXPORT int SwCoreRegisterClient(const char* clientModId) {
    ++g_registeredClients;
    if (g_host && g_host->log) {
        char msg[256];
        snprintf(msg, sizeof(msg), "client enregistre auprès de sw_core_lib : '%s' (total=%d)",
                 clientModId ? clientModId : "?", g_registeredClients);
        g_host->log("sw_core_lib", msg);
    }
    return g_registeredClients;
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    g_host = host;
    int chk = SwMathComputeChecksum(1, 11);  // appel vers math_dep.dll dans le même sous-dossier
    if (host && host->log) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "initialise (v%s par %s, math_dep checksum=%d, gameBase=%p)",
                 host->modVersion, host->modAuthor, chk, host->gameBase);
        host->log(host->modId, msg);
    }
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {
    if (g_host && g_host->log) {
        g_host->log("sw_core_lib", "dechargement propre (SwModUnload)");
    }
    g_host = nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
