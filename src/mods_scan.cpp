#include "mods_scan.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "log.h"

namespace swml {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

namespace {

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool EndsWithNoCaseUtf8(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    return _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

std::string StripDllExt(const std::string& name) {
    if (EndsWithNoCaseUtf8(name, ".dll")) return name.substr(0, name.size() - 4);
    return name;
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L'\\');
    std::wstring dir = slash == std::wstring::npos ? L"." : p.substr(0, slash);
    wchar_t full[MAX_PATH * 2];
    DWORD fn = GetFullPathNameW(dir.c_str(), MAX_PATH * 2, full, nullptr);
    return fn > 0 && fn < MAX_PATH * 2 ? std::wstring(full, fn) : dir;
}

std::string ReadFileHead(const std::wstring& path, size_t max = 65536) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string buf(max, '\0');
    DWORD got = 0;
    ReadFile(h, buf.data(), (DWORD)max, &got, nullptr);
    CloseHandle(h);
    buf.resize(got);
    // Retire un éventuel BOM UTF-8
    if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        buf.erase(0, 3);
    }
    return buf;
}

bool FileExistsAndSize(const std::wstring& path, uint64_t* outSize = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (outSize) *outSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return true;
}

// ---- Parseur JSON léger pour mod.json / manifest.json -----------------------------------------
// Supporte chaînes, tableaux de chaînes, tableaux d'objets {"id": "..."}, booléens et commentaires.

struct JsonManifest {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string mainDll;
    std::vector<std::string> dependencies;
    std::vector<std::string> nativeDeps;
    bool hasEnabledDefault = false;
    bool enabledDefault = true;
};

struct JsonCursor {
    const std::string& src;
    size_t pos = 0;

    void SkipWs() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
                pos += 2;
                while (pos < src.size() && src[pos] != '\n') ++pos;
            } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/')) ++pos;
                if (pos + 1 < src.size()) pos += 2;
            } else {
                break;
            }
        }
    }

    bool Match(char expected) {
        SkipWs();
        if (pos < src.size() && src[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    char Peek() {
        SkipWs();
        return pos < src.size() ? src[pos] : '\0';
    }

    bool ParseString(std::string& out) {
        out.clear();
        SkipWs();
        if (pos >= src.size() || src[pos] != '"') return false;
        ++pos;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return true;
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back(' '); break;
                    case 'r': break;
                    case 't': out.push_back(' '); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    void SkipValue() {
        char c = Peek();
        if (c == '"') {
            std::string dummy;
            ParseString(dummy);
        } else if (c == '{') {
            ++pos;
            int depth = 1;
            while (pos < src.size() && depth > 0) {
                SkipWs();
                if (pos >= src.size()) break;
                if (src[pos] == '"') {
                    std::string dummy;
                    ParseString(dummy);
                } else if (src[pos] == '{') {
                    ++depth;
                    ++pos;
                } else if (src[pos] == '}') {
                    --depth;
                    ++pos;
                } else {
                    ++pos;
                }
            }
        } else if (c == '[') {
            ++pos;
            int depth = 1;
            while (pos < src.size() && depth > 0) {
                SkipWs();
                if (pos >= src.size()) break;
                if (src[pos] == '"') {
                    std::string dummy;
                    ParseString(dummy);
                } else if (src[pos] == '[') {
                    ++depth;
                    ++pos;
                } else if (src[pos] == ']') {
                    --depth;
                    ++pos;
                } else {
                    ++pos;
                }
            }
        } else {
            while (pos < src.size()) {
                char ch = src[pos];
                if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
                    ch == '\r' || ch == '\n') {
                    break;
                }
                ++pos;
            }
        }
    }

    // Parse une liste de chaînes (ou d'objets {"id": "..."}) ou une chaîne simple.
    void ParseStringList(std::vector<std::string>& out) {
        char c = Peek();
        if (c == '"') {
            std::string single;
            if (ParseString(single) && !single.empty()) out.push_back(single);
            return;
        }
        if (!Match('[')) {
            SkipValue();
            return;
        }
        while (pos < src.size()) {
            if (Match(']')) break;
            char itemC = Peek();
            if (itemC == '"') {
                std::string s;
                if (ParseString(s) && !s.empty()) out.push_back(s);
            } else if (itemC == '{') {
                // Objet de dépendance : ex. { "id": "sw_core_lib", "version": "1.0" }
                Match('{');
                std::string depId;
                while (pos < src.size() && !Match('}')) {
                    std::string k;
                    if (!ParseString(k) || !Match(':')) {
                        SkipValue();
                    } else if ((k == "id" || k == "name" || k == "mod" || k == "dll") &&
                               Peek() == '"') {
                        std::string v;
                        ParseString(v);
                        if (depId.empty() || k == "id") depId = v;
                    } else {
                        SkipValue();
                    }
                    Match(',');
                }
                if (!depId.empty()) out.push_back(depId);
            } else {
                SkipValue();
            }
            if (!Match(',')) {
                Match(']');
                break;
            }
        }
    }
};

JsonManifest ParseModJson(const std::string& text) {
    JsonManifest m;
    JsonCursor cur{text, 0};
    if (!cur.Match('{')) return m;
    while (cur.pos < text.size()) {
        if (cur.Match('}')) break;
        std::string key;
        if (!cur.ParseString(key)) break;
        if (!cur.Match(':')) break;
        std::string k = LowerAscii(key);

        if (k == "id") {
            cur.ParseString(m.id);
        } else if (k == "name" || (k == "title" && m.name.empty())) {
            cur.ParseString(m.name);
        } else if (k == "author" || k == "authors") {
            if (cur.Peek() == '[') {
                std::vector<std::string> authors;
                cur.ParseStringList(authors);
                m.author.clear();
                for (size_t i = 0; i < authors.size(); ++i) {
                    if (i > 0) m.author += ", ";
                    m.author += authors[i];
                }
            } else {
                cur.ParseString(m.author);
            }
        } else if (k == "version") {
            cur.ParseString(m.version);
        } else if (k == "description" || (k == "desc" && m.description.empty())) {
            cur.ParseString(m.description);
        } else if (k == "main" || k == "dll" || k == "entry" || k == "binary") {
            std::string v;
            if (cur.ParseString(v) && (m.mainDll.empty() || k == "main")) m.mainDll = v;
        } else if (k == "dependencies" || k == "deps") {
            cur.ParseStringList(m.dependencies);
        } else if (k == "native_deps" || k == "dll_dependencies" || k == "libs") {
            cur.ParseStringList(m.nativeDeps);
        } else if (k == "enabled" || k == "enabled_by_default") {
            cur.SkipWs();
            if (text.compare(cur.pos, 4, "true") == 0) {
                m.hasEnabledDefault = true;
                m.enabledDefault = true;
                cur.pos += 4;
            } else if (text.compare(cur.pos, 5, "false") == 0) {
                m.hasEnabledDefault = true;
                m.enabledDefault = false;
                cur.pos += 5;
            } else {
                cur.SkipValue();
            }
        } else {
            cur.SkipValue();
        }
        cur.Match(',');
    }
    return m;
}

struct DirDllEntry {
    std::wstring fileName;
    std::string utf8Name;
    uint64_t sizeBytes = 0;
};

struct DirScanResult {
    std::vector<DirDllEntry> dlls;
    std::wstring metaFile;
    uint64_t totalDllBytes = 0;
};

DirScanResult ScanSubdirectory(const std::wstring& dir) {
    DirScanResult res;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return res;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (EndsWithNoCase(n, L".dll")) {
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            res.dlls.push_back({n, WideToUtf8(n), sz});
            res.totalDllBytes += sz;
        } else if (_wcsicmp(n.c_str(), L"mod.json") == 0) {
            res.metaFile = n;  // mod.json prioritaire sur manifest.json
        } else if (_wcsicmp(n.c_str(), L"manifest.json") == 0 && res.metaFile.empty()) {
            res.metaFile = n;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(res.dlls.begin(), res.dlls.end(), [](const DirDllEntry& a, const DirDllEntry& b) {
        return _stricmp(a.utf8Name.c_str(), b.utf8Name.c_str()) < 0;
    });
    return res;
}

bool IsListedInDeps(const std::string& dllFileName, const std::vector<std::string>& deps) {
    std::string stem = LowerAscii(StripDllExt(dllFileName));
    std::string full = LowerAscii(dllFileName);
    for (const std::string& d : deps) {
        std::string dl = LowerAscii(d);
        if (dl == full || dl == stem) return true;
    }
    return false;
}

std::string NormalizeRelPath(std::string p) {
    for (char& c : p) {
        if (c == '/') c = '\\';
    }
    while (!p.empty() && (p.front() == '\\' || p.front() == ' ')) p.erase(p.begin());
    return p;
}

// --- état activé/désactivé : mods\_modloader.ini, lignes « nom=0|1 » ---------------------------

std::wstring IniPath() { return ModsDirectory() + L"\\_modloader.ini"; }

std::map<std::string, int> ReadIni() {
    std::map<std::string, int> m;
    std::string text = ReadFileHead(IniPath(), 1 << 20);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.rfind('=');
        if (eq == std::string::npos || eq + 1 >= line.size()) continue;
        m[LowerAscii(line.substr(0, eq))] = line[eq + 1] == '1' ? 1 : 0;
    }
    return m;
}

void WriteIni(const std::map<std::string, int>& m) {
    std::string out =
        "# Etat des mods C++ (1 = active, 0 = desactive) - gere par StormworksModLoader\r\n";
    for (const auto& kv : m) out += kv.first + "=" + (kv.second ? "1" : "0") + "\r\n";
    HANDLE h = CreateFileW(IniPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("ERREUR: ecriture de %ls impossible", IniPath().c_str());
        return;
    }
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
}

}  // namespace

std::wstring ModsDirectory() {
    std::wstring dir = ExeDirectory() + L"\\mods";
    CreateDirectoryW(dir.c_str(), nullptr);  // sans effet s'il existe déjà
    return dir;
}

std::vector<ModEntry> ScanMods() {
    std::vector<ModEntry> out;
    std::wstring root = ModsDirectory();
    std::map<std::string, int> ini = ReadIni();

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring wname = fd.cFileName;
            if (wname == L"." || wname == L"..") continue;

            ModEntry e;
            e.name = WideToUtf8(wname);
            bool defaultEnabled = true;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                e.dirPath = root + L"\\" + wname;
                DirScanResult ds = ScanSubdirectory(e.dirPath);
                e.sizeBytes = ds.totalDllBytes;

                JsonManifest jm;
                if (!ds.metaFile.empty()) {
                    e.hasJson = true;
                    std::string jsonText = ReadFileHead(e.dirPath + L"\\" + ds.metaFile);
                    jm = ParseModJson(jsonText);
                    e.id = LowerAscii(jm.id.empty() ? e.name : jm.id);
                    e.title = jm.name;
                    e.author = jm.author;
                    e.version = jm.version;
                    e.description = jm.description;
                    e.dependencies = jm.dependencies;
                    e.nativeDeps = jm.nativeDeps;
                    if (jm.hasEnabledDefault) defaultEnabled = jm.enabledDefault;
                } else {
                    e.id = LowerAscii(e.name);
                }

                // Résolution de la DLL principale du sous-dossier
                std::string wantedMain = NormalizeRelPath(jm.mainDll);
                if (!wantedMain.empty()) {
                    std::wstring candidate = e.dirPath + L"\\" + Utf8ToWide(wantedMain);
                    uint64_t mainSz = 0;
                    if (!FileExistsAndSize(candidate, &mainSz) &&
                        !EndsWithNoCaseUtf8(wantedMain, ".dll")) {
                        wantedMain += ".dll";
                        candidate = e.dirPath + L"\\" + Utf8ToWide(wantedMain);
                    }
                    if (FileExistsAndSize(candidate, &mainSz)) {
                        e.kind = ModKind::PluginDir;
                        e.mainDll = wantedMain;
                        e.mainDllPath = candidate;
                        if (e.sizeBytes == 0) e.sizeBytes = mainSz;
                        size_t extraDlls = ds.dlls.size() > 1 ? ds.dlls.size() - 1 : 0;
                        e.detail = extraDlls > 0
                                       ? e.mainDll + " (+" + std::to_string(extraDlls) + " dll)"
                                       : e.mainDll;
                    } else {
                        e.kind = ModKind::DirWithoutDll;
                        e.mainDll = wantedMain;
                        e.detail = "missing: " + wantedMain;
                        e.loadState = ModLoadState::NoDll;
                        e.statusDetail = "MISSING DLL";
                    }
                } else if (!ds.dlls.empty()) {
                    // Pas de "main" explicite dans mod.json : on écarte d'abord les native_deps,
                    // puis on privilégie <id>.dll ou <dossier>.dll, sinon la première DLL.
                    const DirDllEntry* chosen = nullptr;
                    std::string folderStem = LowerAscii(e.name);
                    for (const DirDllEntry& d : ds.dlls) {
                        if (IsListedInDeps(d.utf8Name, e.nativeDeps) ||
                            IsListedInDeps(d.utf8Name, e.dependencies)) {
                            continue;
                        }
                        std::string stem = LowerAscii(StripDllExt(d.utf8Name));
                        if (stem == e.id || stem == folderStem) {
                            chosen = &d;
                            break;
                        }
                        if (!chosen) chosen = &d;
                    }
                    if (!chosen) chosen = &ds.dlls[0];

                    e.kind = ModKind::PluginDir;
                    e.mainDll = chosen->utf8Name;
                    e.mainDllPath = e.dirPath + L"\\" + chosen->fileName;
                    size_t extraDlls = ds.dlls.size() > 1 ? ds.dlls.size() - 1 : 0;
                    e.detail = extraDlls > 0
                                   ? e.mainDll + " (+" + std::to_string(extraDlls) + " dll)"
                                   : e.mainDll;
                } else {
                    e.kind = ModKind::DirWithoutDll;
                    e.detail = "no DLL found";
                    e.loadState = ModLoadState::NoDll;
                    e.statusDetail = "NO DLL";
                }
            } else if (EndsWithNoCase(wname, L".dll")) {
                e.kind = ModKind::DllFile;
                e.id = LowerAscii(StripDllExt(e.name));
                e.dirPath = root;
                e.mainDll = e.name;
                e.mainDllPath = root + L"\\" + wname;
                e.detail = "standalone DLL";
                e.sizeBytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            } else {
                continue;  // fichier non-DLL à la racine de mods\ (ex: _modloader.ini)
            }

            if (e.Toggleable()) {
                auto it = ini.find(LowerAscii(e.name));
                if (it == ini.end()) it = ini.find(e.id);
                e.enabled = (it == ini.end()) ? defaultEnabled : (it->second != 0);
                e.loadState = ModLoadState::Unloaded;
                e.statusDetail = e.enabled ? "PENDING" : "DISABLED";
            } else {
                e.enabled = false;
            }

            out.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(out.begin(), out.end(), [](const ModEntry& a, const ModEntry& b) {
        if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
        return _stricmp(a.DisplayName().c_str(), b.DisplayName().c_str()) < 0;
    });
    return out;
}

void SaveEnabled(const ModEntry& e) {
    if (!e.Toggleable()) return;
    std::map<std::string, int> ini = ReadIni();
    ini[LowerAscii(e.name)] = e.enabled ? 1 : 0;
    WriteIni(ini);
}

void SaveAllEnabled(const std::vector<ModEntry>& mods) {
    std::map<std::string, int> ini = ReadIni();
    for (const ModEntry& e : mods) {
        if (!e.Toggleable()) continue;
        ini[LowerAscii(e.name)] = e.enabled ? 1 : 0;
    }
    WriteIni(ini);
}

std::string SafeText(std::string s) {
    // Le moteur de texte interprète « <n> » comme une balise de style et '\n' comme un saut de ligne.
    for (char& c : s) {
        if (c == '<') c = '(';
        else if (c == '>') c = ')';
        else if ((unsigned char)c < 0x20) c = ' ';
    }
    return s;
}

std::string HumanSize(uint64_t b) {
    char buf[32];
    if (b < 1024) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    return buf;
}

void OpenModsFolder() {
    // Résolution à l'appel : pas d'import de shell32 dans le proxy (chargé très tôt par le loader).
    using ShellExecuteWFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    auto fn = shell ? reinterpret_cast<ShellExecuteWFn>(GetProcAddress(shell, "ShellExecuteW"))
                    : nullptr;
    if (!fn) return;
    std::wstring dir = ModsDirectory();
    fn(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace swml
