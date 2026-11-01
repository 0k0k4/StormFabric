// Proxy DLL : iphlpapi.dll
//
// stormworks64.exe importe IPHLPAPI.DLL (GetAdaptersInfo). IPHLPAPI n'est pas dans les KnownDLLs :
// Windows cherche d'abord dans le dossier de l'exécutable. En déposant cette DLL sous le nom
// `iphlpapi.dll` à côté de stormworks64.exe, le loader la charge pendant l'initialisation du
// processus (avant le point d'entrée du jeu), appelle DllMain, puis résout GetAdaptersInfo & co.
// via les forwarders de proxy_exports.h vers la vraie DLL de System32.
#include <windows.h>

#include "log.h"
#include "mods_ui.h"
#include "proxy_exports.h"  // /export:... forwarders vers C:\Windows\System32\iphlpapi.dll

namespace {
bool IsStormworksClient() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"stormworks64.exe") == 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (IsStormworksClient()) {
            swml::LogInit();
            swml::Log("StormworksModLoader 1.0 charge (proxy iphlpapi)");
            if (swml::InstallModsUi()) {
                swml::Log("initialisation terminee");
            } else {
                swml::Log("initialisation abandonnee : le jeu demarre sans modification");
            }
        }
    }
    return TRUE;
}
