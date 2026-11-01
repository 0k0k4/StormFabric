#pragma once
#include <vector>

#include "mods_scan.h"

namespace swml {

// Active ou désactive `mods[index]`. Si `enabled == true`, active aussi récursivement toutes les
// dépendances inter-mods présentes dans `mods` afin que le mod puisse se charger immédiatement.
// Persiste l'état dans mods\_modloader.ini.
void SetModEnabledWithDeps(std::vector<ModEntry>& mods, int index, bool enabled);

// Synchronise les DLLs chargées en mémoire avec la liste `mods` :
//  - Décharge (SwModUnload + FreeLibrary) dans l'ordre inverse des dépendances les mods qui ont été
//    désactivés, supprimés ou dont une dépendance n'est plus active.
//  - Résout le graphe de dépendances (tri topologique), précharge les DLLs locales du sous-dossier
//    (AddDllDirectory + native_deps), charge la DLL principale (LoadLibraryExW) et appelle
//    l'export optionnel SwModInit(const SwModHost*).
//  - Met à jour `loadState` et `statusDetail` de chaque entrée dans `mods` et émet les
//    notifications (INFO / WARN / ERROR) correspondantes.
void SyncLoadedMods(std::vector<ModEntry>& mods, void* gameBase, void* uiCtx, void* mainMenuUi);

// Décharge tous les mods actuellement chargés puis recharge tous les mods actifs depuis le disque.
void ForceReloadAllMods(std::vector<ModEntry>& mods, void* gameBase, void* uiCtx, void* mainMenuUi);

// Écrit dans les logs un diagnostic complet des modules chargés en mémoire (HMODULE, exports, deps).
void DumpLoadedModsDiagnostics();

// Appelle l'export optionnel SwModTick(uiCtx) de chaque mod actif chargé (à chaque frame UI).
void TickLoadedMods(void* uiCtx);

// Nombre de mods actuellement chargés en mémoire.
int LoadedModsCount();

}  // namespace swml
