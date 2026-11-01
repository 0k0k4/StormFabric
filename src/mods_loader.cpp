#include "mods_loader.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "../include/sw_mod_sdk.h"
#include "log.h"
#include "engine_api.h"

namespace swml {
namespace {

struct LoadedModInstance {
    std::string key;                   // LowerAscii(ModEntry::name)
    std::string id;                    // ModEntry::id
    std::string displayName;           // ModEntry::DisplayName()
    std::string version;               // ModEntry::version
    std::string author;                // ModEntry::author
    std::string dirUtf8;               // Chemin UTF-8 du dossier du mod
    std::wstring mainDllPath;          // Chemin complet de la DLL principale
    SwModHost host = {};               // Hôte persistant (adresse stable tant que le mod est chargé)
    HMODULE mainHandle = nullptr;      // HMODULE de la DLL principale
    std::vector<HMODULE> depHandles;   // HMODULEs des DLLs locales préchargées (native_deps)
    DLL_DIRECTORY_COOKIE dirCookie = nullptr;
    SwModInitFn initFn = nullptr;
    SwModTickFn tickFn = nullptr;
    SwModUnloadFn unloadFn = nullptr;
};

// Registre global des mods actuellement chargés dans le processus
std::map<std::string, std::unique_ptr<LoadedModInstance>> g_loaded;
std::vector<std::string> g_loadedOrder;  // ordre effectif de chargement
// Mémorise le dernier statut notifié par mod pour éviter de répéter la même alerte à chaque Sync
std::map<std::string, std::string> g_lastNotifiedIssue;

void ModSdkLog(const char* modId, const char* message) {
    const char* id = modId ? modId : "?";
    const char* msg = message ? message : "";
    if (_strnicmp(msg, "ERREUR:", 7) == 0 || _strnicmp(msg, "ERROR:", 6) == 0) {
        const char* body = strchr(msg, ':') + 1;
        while (*body == ' ') ++body;
        Notify(LogLevel::Error, std::string("Mod '") + id + "'", body);
    } else if (_strnicmp(msg, "WARN:", 5) == 0 || _strnicmp(msg, "ATTENTION:", 10) == 0) {
        const char* body = strchr(msg, ':') + 1;
        while (*body == ' ') ++body;
        Notify(LogLevel::Warn, std::string("Mod '") + id + "'", body);
    } else if (_strnicmp(msg, "INFO:", 5) == 0 || _strnicmp(msg, "NOTIF:", 6) == 0) {
        const char* body = strchr(msg, ':') + 1;
        while (*body == ' ') ++body;
        Notify(LogLevel::Info, std::string("Mod '") + id + "'", body);
    } else {
        LogInfo("[Mod:%s] %s", id, msg);
    }
}

void NotifyIssueOnce(const ModEntry& m, LogLevel level, const std::string& title,
                     const std::string& detail) {
    std::string key = LowerAscii(m.name);
    std::string sig = std::to_string((int)level) + ":" + m.statusDetail + ":" + detail;
    auto it = g_lastNotifiedIssue.find(key);
    if (it != g_lastNotifiedIssue.end() && it->second == sig) return;
    g_lastNotifiedIssue[key] = sig;
    Notify(level, title, detail);
}

void ClearIssueState(const ModEntry& m) {
    g_lastNotifiedIssue.erase(LowerAscii(m.name));
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string NormalizeRel(std::string s) {
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
    while (!s.empty() && (s.front() == '\\' || s.front() == ' ')) s.erase(s.begin());
    return s;
}

bool EndsWithDll(const std::string& s) {
    return s.size() >= 4 && _stricmp(s.c_str() + s.size() - 4, ".dll") == 0;
}

std::string StripDll(const std::string& s) {
    return EndsWithDll(s) ? s.substr(0, s.size() - 4) : s;
}

// Construit une table de recherche : id / nom de dossier / titre (en minuscules) -> index dans mods
std::map<std::string, int> BuildModLookup(const std::vector<ModEntry>& mods) {
    std::map<std::string, int> lut;
    for (int i = 0; i < (int)mods.size(); ++i) {
        const ModEntry& m = mods[i];
        if (!m.id.empty()) lut[LowerAscii(m.id)] = i;
        if (!m.name.empty()) {
            lut[LowerAscii(m.name)] = i;
            lut[LowerAscii(StripDll(m.name))] = i;
        }
        if (!m.title.empty()) lut[LowerAscii(m.title)] = i;
    }
    return lut;
}

// Vérifie si `depSpec` correspond à une DLL locale présente dans le dossier `dirPath` du mod.
bool ResolveLocalDll(const ModEntry& m, const std::string& depSpec, std::wstring& outPath) {
    std::string rel = NormalizeRel(depSpec);
    if (rel.empty()) return false;
    std::wstring candidate = m.dirPath + L"\\" + Utf8ToWide(rel);
    if (!FileExists(candidate) && !EndsWithDll(rel)) {
        candidate = m.dirPath + L"\\" + Utf8ToWide(rel + ".dll");
    }
    if (!FileExists(candidate)) return false;
    if (_wcsicmp(candidate.c_str(), m.mainDllPath.c_str()) == 0) return true;  // déjà la DLL main
    outPath = candidate;
    return true;
}

HMODULE LoadDllWithModSearchPath(const std::wstring& fullPath) {
    constexpr DWORD kSearchFlags =
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
        LOAD_LIBRARY_SEARCH_USER_DIRS;
    HMODULE h = LoadLibraryExW(fullPath.c_str(), nullptr, kSearchFlags);
    if (!h && GetLastError() == ERROR_INVALID_PARAMETER) {
        h = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    return h;
}

bool SafeCallInit(SwModInitFn fn, const SwModHost* host, bool* outResult) {
    __try {
        *outResult = fn(host);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outResult = false;
        return false;
    }
}

bool SafeCallTick(SwModTickFn fn, void* uiCtx) {
    __try {
        fn(uiCtx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCallUnload(SwModUnloadFn fn) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UnloadInstance(LoadedModInstance& inst, bool emitNotif = true) {
    ReleaseEngineApiOwner(&inst.host);
    LogDebug("Dechargement de '%s' (HMODULE=%p, deps=%zu)...", inst.displayName.c_str(),
             (void*)inst.mainHandle, inst.depHandles.size());
    if (inst.unloadFn) {
        if (!SafeCallUnload(inst.unloadFn)) {
            Notify(LogLevel::Error, "Crash SwModUnload : " + inst.displayName,
                   "Une exception est survenue pendant SwModUnload()");
        }
    }
    if (inst.mainHandle) {
        FreeLibrary(inst.mainHandle);
        inst.mainHandle = nullptr;
    }
    for (auto it = inst.depHandles.rbegin(); it != inst.depHandles.rend(); ++it) {
        if (*it) FreeLibrary(*it);
    }
    inst.depHandles.clear();
    if (inst.dirCookie) {
        RemoveDllDirectory(inst.dirCookie);
        inst.dirCookie = nullptr;
    }
    LogInfo("mod '%s' (id=%s) decharge de la memoire", inst.displayName.c_str(), inst.id.c_str());
    if (emitNotif) {
        Notify(LogLevel::Info, "Mod Unloaded : " + inst.displayName,
               "Plugin DLL unloaded from memory (" + inst.id + ")");
    }
}

template <typename Fn>
Fn FindExport(HMODULE h, const char* name1, const char* name2 = nullptr,
              const char* name3 = nullptr) {
    if (auto p = GetProcAddress(h, name1)) return reinterpret_cast<Fn>(p);
    if (name2) {
        if (auto p = GetProcAddress(h, name2)) return reinterpret_cast<Fn>(p);
    }
    if (name3) {
        if (auto p = GetProcAddress(h, name3)) return reinterpret_cast<Fn>(p);
    }
    return nullptr;
}

}  // namespace

void SetModEnabledWithDeps(std::vector<ModEntry>& mods, int index, bool enabled) {
    if (index < 0 || index >= (int)mods.size()) return;
    ModEntry& target = mods[index];
    if (!target.Toggleable()) return;

    if (!enabled) {
        target.enabled = false;
        ClearIssueState(target);
        SaveEnabled(target);
        LogInfo("mod '%s' -> desactive par l'utilisateur", target.DisplayName().c_str());
        return;
    }

    // Activation : on active aussi récursivement les dépendances inter-mods présentes
    std::map<std::string, int> lut = BuildModLookup(mods);
    std::queue<int> q;
    std::set<int> visited;
    q.push(index);
    visited.insert(index);

    while (!q.empty()) {
        int cur = q.front();
        q.pop();
        ModEntry& m = mods[cur];
        if (!m.Toggleable()) continue;
        if (!m.enabled) {
            m.enabled = true;
            ClearIssueState(m);
            if (cur == index) {
                LogInfo("mod '%s' -> active par l'utilisateur", m.DisplayName().c_str());
            } else {
                Notify(LogLevel::Warn, "Auto-Enabled Dependency : " + m.DisplayName(),
                       "Required by '" + target.DisplayName() + "'");
            }
        }
        for (const std::string& dep : m.dependencies) {
            auto it = lut.find(LowerAscii(dep));
            if (it == lut.end()) it = lut.find(LowerAscii(StripDll(dep)));
            if (it != lut.end() && it->second != cur && !visited.count(it->second)) {
                visited.insert(it->second);
                q.push(it->second);
            }
        }
    }
    SaveAllEnabled(mods);
}

void SyncLoadedMods(std::vector<ModEntry>& mods, void* gameBase, void* uiCtx, void* mainMenuUi) {
    const int n = (int)mods.size();
    std::map<std::string, int> lut = BuildModLookup(mods);

    // 1. Analyse des dépendances de chaque mod (inter-mods vs DLLs locales du sous-dossier)
    std::vector<std::vector<int>> modDeps(n);
    std::vector<std::vector<std::wstring>> localDllDeps(n);
    std::vector<std::string> missingDepName(n);

    for (int i = 0; i < n; ++i) {
        ModEntry& m = mods[i];
        if (!m.Toggleable()) {
            m.loadState = ModLoadState::NoDll;
            if (m.statusDetail.empty() || m.statusDetail == "DISABLED" ||
                m.statusDetail == "PENDING") {
                m.statusDetail = "NO DLL";
            }
            NotifyIssueOnce(m, LogLevel::Warn, "Mod Folder Without DLL : " + m.DisplayName(),
                            "Folder '" + m.name + "' has no loadable main DLL (" + m.detail + ")");
            continue;
        }
        if (!m.enabled) {
            m.loadState = ModLoadState::Unloaded;
            m.statusDetail = "DISABLED";
            continue;
        }

        // DLLs locales déclarées dans "native_deps"
        for (const std::string& nd : m.nativeDeps) {
            std::wstring fullDep;
            if (ResolveLocalDll(m, nd, fullDep)) {
                if (!fullDep.empty()) localDllDeps[i].push_back(fullDep);
            } else if (missingDepName[i].empty()) {
                missingDepName[i] = nd;
            }
        }

        // Entrées dans "dependencies" / "deps" : soit un autre mod, soit une DLL locale du dossier
        for (const std::string& dep : m.dependencies) {
            std::string dl = LowerAscii(dep);
            auto it = lut.find(dl);
            if (it == lut.end()) it = lut.find(LowerAscii(StripDll(dl)));

            if (it != lut.end() && it->second != i) {
                modDeps[i].push_back(it->second);
            } else {
                std::wstring fullDep;
                if (ResolveLocalDll(m, dep, fullDep)) {
                    if (!fullDep.empty()) localDllDeps[i].push_back(fullDep);
                } else if (missingDepName[i].empty()) {
                    missingDepName[i] = dep;
                }
            }
        }
    }

    // 2. Détermination des mods éligibles (actifs, sans dépendance manquante ni désactivée)
    std::vector<bool> eligible(n, false);
    for (int i = 0; i < n; ++i) {
        if (!mods[i].Toggleable() || !mods[i].enabled) continue;
        if (!missingDepName[i].empty()) {
            mods[i].loadState = ModLoadState::MissingDep;
            mods[i].statusDetail = "MISSING: " + missingDepName[i];
            NotifyIssueOnce(mods[i], LogLevel::Error,
                            "Missing Dependency : " + mods[i].DisplayName(),
                            "Required dependency '" + missingDepName[i] + "' was not found");
            continue;
        }
        eligible[i] = true;
    }

    // Propagation : si une dépendance d'un mod éligible n'est pas éligible, le mod ne l'est pas
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            if (!eligible[i]) continue;
            for (int depIdx : modDeps[i]) {
                if (!eligible[depIdx]) {
                    eligible[i] = false;
                    mods[i].loadState = ModLoadState::MissingDep;
                    mods[i].statusDetail = "NEED: " + mods[depIdx].id;
                    NotifyIssueOnce(mods[i], LogLevel::Warn,
                                    "Dependency Disabled : " + mods[i].DisplayName(),
                                    "Waiting for dependency '" + mods[depIdx].DisplayName() + "'");
                    changed = true;
                    break;
                }
            }
        }
    }

    // 3. Tri topologique (algorithme de Kahn) des mods éligibles
    std::vector<int> inDegree(n, 0);
    std::vector<std::vector<int>> dependents(n);
    for (int i = 0; i < n; ++i) {
        if (!eligible[i]) continue;
        for (int depIdx : modDeps[i]) {
            dependents[depIdx].push_back(i);
            ++inDegree[i];
        }
    }

    std::queue<int> ready;
    for (int i = 0; i < n; ++i) {
        if (eligible[i] && inDegree[i] == 0) ready.push(i);
    }

    std::vector<int> loadOrder;
    while (!ready.empty()) {
        int u = ready.front();
        ready.pop();
        loadOrder.push_back(u);
        for (int v : dependents[u]) {
            if (--inDegree[v] == 0) ready.push(v);
        }
    }

    // Détection d'éventuels cycles de dépendances
    std::set<std::string> keepLoadedKeys;
    for (int i = 0; i < n; ++i) {
        if (eligible[i] && inDegree[i] > 0) {
            eligible[i] = false;
            mods[i].loadState = ModLoadState::MissingDep;
            mods[i].statusDetail = "DEP CYCLE";
            NotifyIssueOnce(mods[i], LogLevel::Error,
                            "Dependency Cycle : " + mods[i].DisplayName(),
                            "Circular dependency detected in mod.json");
        } else if (eligible[i]) {
            keepLoadedKeys.insert(LowerAscii(mods[i].name));
        }
    }

    // 4. Déchargement (dans l'ordre inverse de chargement) des mods qui ne doivent plus être actifs
    std::vector<std::string> remainingOrder;
    for (auto it = g_loadedOrder.rbegin(); it != g_loadedOrder.rend(); ++it) {
        const std::string& key = *it;
        auto mapIt = g_loaded.find(key);
        if (mapIt == g_loaded.end()) continue;
        if (!keepLoadedKeys.count(key)) {
            UnloadInstance(*mapIt->second);
            g_loaded.erase(mapIt);
        }
    }
    for (const std::string& key : g_loadedOrder) {
        if (g_loaded.count(key)) remainingOrder.push_back(key);
    }
    g_loadedOrder = std::move(remainingOrder);

    // 5. Chargement dans l'ordre topologique
    for (int idx : loadOrder) {
        ModEntry& m = mods[idx];
        std::string key = LowerAscii(m.name);

        // Vérifie que toutes les dépendances en amont ont bien réussi leur chargement
        bool upstreamOk = true;
        for (int depIdx : modDeps[idx]) {
            if (mods[depIdx].loadState != ModLoadState::Loaded) {
                m.loadState = ModLoadState::MissingDep;
                m.statusDetail = "DEP ERR: " + mods[depIdx].id;
                NotifyIssueOnce(m, LogLevel::Error,
                                "Dependency Load Failed : " + m.DisplayName(),
                                "Required mod '" + mods[depIdx].DisplayName() + "' failed to load");
                upstreamOk = false;
                break;
            }
        }
        if (!upstreamOk) {
            auto ex = g_loaded.find(key);
            if (ex != g_loaded.end()) {
                UnloadInstance(*ex->second);
                g_loaded.erase(ex);
                g_loadedOrder.erase(
                    std::remove(g_loadedOrder.begin(), g_loadedOrder.end(), key),
                    g_loadedOrder.end());
            }
            continue;
        }

        // Déjà chargé en mémoire ?
        auto existing = g_loaded.find(key);
        if (existing != g_loaded.end()) {
            existing->second->host.uiContext = uiCtx;
            existing->second->host.mainMenuInterface = mainMenuUi;
            m.loadState = ModLoadState::Loaded;
            m.statusDetail = "LOADED";
            ClearIssueState(m);
            continue;
        }

        if (!FileExists(m.mainDllPath)) {
            m.loadState = ModLoadState::NoDll;
            m.statusDetail = "MISSING DLL";
            NotifyIssueOnce(m, LogLevel::Error, "Missing DLL : " + m.DisplayName(),
                            "File not found: " + m.mainDll);
            continue;
        }

        auto inst = std::make_unique<LoadedModInstance>();
        inst->key = key;
        inst->id = m.id;
        inst->displayName = m.DisplayName();
        inst->version = m.version;
        inst->author = m.author;
        inst->dirUtf8 = WideToUtf8(m.dirPath);
        inst->mainDllPath = m.mainDllPath;

        inst->host.sdkVersion = SW_MOD_SDK_VERSION;
        inst->host.gameBase = gameBase;
        inst->host.modId = inst->id.c_str();
        inst->host.modName = inst->displayName.c_str();
        inst->host.modVersion = inst->version.c_str();
        inst->host.modAuthor = inst->author.c_str();
        inst->host.modDir = inst->dirUtf8.c_str();
        inst->host.uiContext = uiCtx;
        inst->host.mainMenuInterface = mainMenuUi;
        inst->host.log = &ModSdkLog;
        inst->host.api = GetEngineApi();

        // Ajoute le dossier du mod aux chemins de recherche de DLLs Windows
        if (m.kind == ModKind::PluginDir && !m.dirPath.empty()) {
            inst->dirCookie = AddDllDirectory(m.dirPath.c_str());
            SetDllDirectoryW(m.dirPath.c_str());
            LogDebug("AddDllDirectory actif pour '%s' : %ls", m.DisplayName().c_str(),
                     m.dirPath.c_str());
        }

        // Précharge les DLLs locales déclarées (native_deps / deps locales)
        bool localDepsOk = true;
        for (const std::wstring& depPath : localDllDeps[idx]) {
            HMODULE hDep = LoadDllWithModSearchPath(depPath);
            if (!hDep) {
                DWORD err = GetLastError();
                m.loadState = (err == ERROR_BAD_EXE_FORMAT) ? ModLoadState::InvalidArch
                                                            : ModLoadState::LoadFailed;
                m.statusDetail = (err == ERROR_BAD_EXE_FORMAT) ? "ERR: 32-BIT DEP" : "DEP DLL ERR";
                NotifyIssueOnce(m, LogLevel::Error, "Native Dep Load Failed : " + m.DisplayName(),
                                "Failed to load local dependency DLL (Win32 error " +
                                    std::to_string(err) + ")");
                localDepsOk = false;
                break;
            }
            inst->depHandles.push_back(hDep);
            LogDebug("dependance locale chargee pour '%s' : %ls @ %p", m.DisplayName().c_str(),
                     depPath.c_str(), (void*)hDep);
        }

        if (!localDepsOk) {
            SetDllDirectoryW(nullptr);
            for (auto it = inst->depHandles.rbegin(); it != inst->depHandles.rend(); ++it) {
                FreeLibrary(*it);
            }
            if (inst->dirCookie) RemoveDllDirectory(inst->dirCookie);
            continue;
        }

        // Charge la DLL principale du mod
        HMODULE hMain = LoadDllWithModSearchPath(m.mainDllPath);
        DWORD loadErr = hMain ? 0 : GetLastError();
        SetDllDirectoryW(nullptr);

        if (!hMain) {
            for (auto it = inst->depHandles.rbegin(); it != inst->depHandles.rend(); ++it) {
                FreeLibrary(*it);
            }
            if (inst->dirCookie) RemoveDllDirectory(inst->dirCookie);

            if (loadErr == ERROR_BAD_EXE_FORMAT) {
                m.loadState = ModLoadState::InvalidArch;
                m.statusDetail = "ERR: 32-BIT DLL";
                NotifyIssueOnce(m, LogLevel::Error, "32-Bit DLL Error : " + m.DisplayName(),
                                "'" + m.mainDll + "' is 32-bit (stormworks64.exe requires 64-bit)");
            } else if (loadErr == ERROR_MOD_NOT_FOUND) {
                m.loadState = ModLoadState::LoadFailed;
                m.statusDetail = "ERR: DLL DEP(126)";
                NotifyIssueOnce(m, LogLevel::Error, "Missing Imported DLL : " + m.DisplayName(),
                                "LoadLibrary failed for '" + m.mainDll + "' (Win32 error 126)");
            } else if (loadErr == ERROR_DLL_INIT_FAILED) {
                m.loadState = ModLoadState::InitFailed;
                m.statusDetail = "DLLMAIN FALSE";
                NotifyIssueOnce(m, LogLevel::Error, "DllMain Failed : " + m.DisplayName(),
                                "DllMain returned FALSE for '" + m.mainDll + "' (error 1114)");
            } else {
                m.loadState = ModLoadState::LoadFailed;
                m.statusDetail = "LOAD ERR " + std::to_string(loadErr);
                NotifyIssueOnce(m, LogLevel::Error, "LoadLibrary Failed : " + m.DisplayName(),
                                "Failed to load '" + m.mainDll + "' (Win32 error " +
                                    std::to_string(loadErr) + ")");
            }
            continue;
        }

        inst->mainHandle = hMain;
        inst->initFn = FindExport<SwModInitFn>(hMain, "SwModInit", "StormworksModInit");
        inst->tickFn = FindExport<SwModTickFn>(hMain, "SwModTick", "StormworksModTick");
        inst->unloadFn = FindExport<SwModUnloadFn>(hMain, "SwModUnload", "StormworksModUnload",
                                                    "SwModShutdown");
        RegisterEngineApiOwner(&inst->host);

        // Appel de l'export d'initialisation optionnel SwModInit(const SwModHost*)
        if (inst->initFn) {
            bool initOk = false;
            if (!SafeCallInit(inst->initFn, &inst->host, &initOk)) {
                NotifyIssueOnce(m, LogLevel::Error, "SwModInit Crash : " + m.DisplayName(),
                                "Unhandled exception inside SwModInit() of '" + m.mainDll + "'");
                initOk = false;
            } else if (!initOk) {
                NotifyIssueOnce(m, LogLevel::Error, "SwModInit Rejected : " + m.DisplayName(),
                                "SwModInit() returned false for '" + m.mainDll + "'");
            }

            if (!initOk) {
                ReleaseEngineApiOwner(&inst->host);
                FreeLibrary(inst->mainHandle);
                for (auto it = inst->depHandles.rbegin(); it != inst->depHandles.rend(); ++it) {
                    FreeLibrary(*it);
                }
                if (inst->dirCookie) RemoveDllDirectory(inst->dirCookie);
                m.loadState = ModLoadState::InitFailed;
                m.statusDetail = "INIT FAILED";
                continue;
            }
        }

        std::string verInfo = m.version.empty() ? "" : (" v" + m.version);
        std::string authInfo = m.author.empty() ? "" : (" by " + m.author);
        LogInfo("mod '%s' (id=%s%s%s) charge @ %p depuis %ls [entree: %s%s]",
                m.DisplayName().c_str(), m.id.c_str(), verInfo.c_str(), authInfo.c_str(),
                (void*)hMain, m.mainDllPath.c_str(),
                inst->initFn ? "SwModInit" : "DllMain",
                inst->tickFn ? "+SwModTick" : "");

        Notify(LogLevel::Info, "Mod Loaded : " + m.DisplayName() + verInfo,
               m.mainDll + authInfo + " (" + (inst->initFn ? "SDK SwModInit" : "DllMain") + ")");

        ClearIssueState(m);
        g_loaded[key] = std::move(inst);
        g_loadedOrder.push_back(key);
        m.loadState = ModLoadState::Loaded;
        m.statusDetail = "LOADED";
    }
}

void ForceReloadAllMods(std::vector<ModEntry>& mods, void* gameBase, void* uiCtx,
                        void* mainMenuUi) {
    LogInfo("=== Rechargement force de tous les mods actifs ===");
    for (auto it = g_loadedOrder.rbegin(); it != g_loadedOrder.rend(); ++it) {
        auto mapIt = g_loaded.find(*it);
        if (mapIt != g_loaded.end()) {
            UnloadInstance(*mapIt->second, /*emitNotif=*/false);
        }
    }
    g_loaded.clear();
    g_loadedOrder.clear();
    g_lastNotifiedIssue.clear();
    SyncLoadedMods(mods, gameBase, uiCtx, mainMenuUi);
    Notify(LogLevel::Info, "Hot-Reload Complete",
           std::to_string(LoadedModsCount()) + " mod(s) reloaded in memory");
}

void DumpLoadedModsDiagnostics() {
    LogInfo("=== Diagnostic des modules charges (%zu mod(s) en memoire) ===",
            g_loadedOrder.size());
    for (size_t i = 0; i < g_loadedOrder.size(); ++i) {
        auto it = g_loaded.find(g_loadedOrder[i]);
        if (it == g_loaded.end() || !it->second) continue;
        const LoadedModInstance& inst = *it->second;
        LogInfo("  #%zu [%s] '%s' v%s par %s | HMODULE=%p | localDeps=%zu | Init=%p Tick=%p Unload=%p",
                i + 1, inst.id.c_str(), inst.displayName.c_str(),
                inst.version.empty() ? "-" : inst.version.c_str(),
                inst.author.empty() ? "-" : inst.author.c_str(),
                (void*)inst.mainHandle, inst.depHandles.size(),
                (void*)inst.initFn, (void*)inst.tickFn, (void*)inst.unloadFn);
    }
}

void TickLoadedMods(void* uiCtx) {
    for (const std::string& key : g_loadedOrder) {
        auto it = g_loaded.find(key);
        if (it == g_loaded.end() || !it->second || !it->second->tickFn) continue;
        if (!SafeCallTick(it->second->tickFn, uiCtx)) {
            Notify(LogLevel::Error, "SwModTick Crash : " + it->second->displayName,
                   "Exception in SwModTick() — tick disabled for this mod");
            it->second->tickFn = nullptr;
        }
    }
}

int LoadedModsCount() { return (int)g_loaded.size(); }

}  // namespace swml
