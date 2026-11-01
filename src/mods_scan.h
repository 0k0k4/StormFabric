#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace swml {

enum class ModKind { PluginDir, DllFile, DirWithoutDll };

enum class ModLoadState {
    Unloaded,     // Désactivé par l'utilisateur (non chargé en mémoire)
    Loaded,       // DLL chargée en mémoire (+ SwModInit réussi si présent)
    MissingDep,   // Dépendance (autre mod ou DLL) absente, désactivée ou cyclique
    InvalidArch,  // DLL 32 bits dans un processus 64 bits (ERROR_BAD_EXE_FORMAT)
    LoadFailed,   // LoadLibraryExW a échoué
    InitFailed,   // SwModInit a retourné false
    NoDll         // Dossier sans DLL ou "main" introuvable
};

struct ModEntry {
    ModKind kind = ModKind::DllFile;
    std::string id;           // ID canonique en minuscules ("id" de mod.json ou nom sans .dll)
    std::string name;         // Nom du sous-dossier ou fichier dans mods\ (clé _modloader.ini)
    std::string title;        // Champ "name" / "title" de mod.json (sinon vide)
    std::string author;       // Champ "author" / "authors" de mod.json (sinon vide)
    std::string version;      // Champ "version" de mod.json (sinon vide)
    std::string description;  // Champ "description" / "desc" de mod.json (sinon vide)
    std::string mainDll;      // Fichier DLL principal relatif au dossier du mod
    std::string detail;       // Détail technique (ex. "plugin.dll (+1 dll)" ou "DLL")

    std::wstring dirPath;      // Chemin complet du dossier du mod
    std::wstring mainDllPath;  // Chemin complet de la DLL principale à charger

    std::vector<std::string> dependencies;  // Dépendances ("dependencies" / "deps" dans mod.json)
    std::vector<std::string> nativeDeps;    // DLLs locales ("native_deps" / "dll_dependencies")

    uint64_t sizeBytes = 0;
    bool hasJson = false;
    bool enabled = true;  // État souhaité persistant (mods\_modloader.ini)

    ModLoadState loadState = ModLoadState::Unloaded;
    std::string statusDetail = "DISABLED";  // Badge affiché à droite ("LOADED", "DISABLED", "NEED: ...")

    // Texte affichable : « titre » si présent, sinon le nom (sans « .dll » pour une DLL seule).
    std::string DisplayName() const {
        if (!title.empty()) return title;
        if (kind == ModKind::DllFile && name.size() > 4) return name.substr(0, name.size() - 4);
        return name;
    }
    bool Toggleable() const { return kind != ModKind::DirWithoutDll && !mainDllPath.empty(); }
    bool IsLoaded() const { return loadState == ModLoadState::Loaded; }
};

// <dossier de stormworks64.exe>\mods  (créé s'il n'existe pas)
std::wstring ModsDirectory();

// Détecte les mods dans mods\ :
//  - sous-dossiers avec mod.json / manifest.json (name, author, version, description, main, deps...)
//  - fallback : sous-dossiers avec DLL sans mod.json, ou *.dll directement dans mods\
//  - sous-dossiers sans DLL ou avec "main" manquant signalés en NoDll.
std::vector<ModEntry> ScanMods();

// Écrit l'état activé/désactivé de `e` dans mods\_modloader.ini (les autres clés sont conservées).
void SaveEnabled(const ModEntry& e);

// Écrit l'état de toute la liste dans mods\_modloader.ini en une seule opération.
void SaveAllEnabled(const std::vector<ModEntry>& mods);

// Assainit un texte pour le moteur de texte du jeu (balises « <..> » neutralisées, contrôles).
std::string SafeText(std::string s);

// "12 KB", "3.4 MB" ...
std::string HumanSize(uint64_t bytes);

// Utilitaires de chaînes UTF-8 / Wide / minuscules ASCII
std::string LowerAscii(std::string s);
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

// Ouvre le dossier mods dans l'explorateur.
void OpenModsFolder();

}  // namespace swml
