// DLL de dépendance native locale (placée dans le sous-dossier mods\sw_core_lib\math_dep.dll).
// Chargée automatiquement depuis le sous-dossier du mod sans polluer la racine du jeu.
#include <windows.h>

extern "C" __declspec(dllexport) int SwMathComputeChecksum(int a, int b) {
    return (a * 31) + b;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
