// Content overlay: mods add or replace files of the game's rom/ tree without touching it.
//
// The game reads rom/ through kernel32 (the static CRT's fopen calls CreateFileW through
// the executable's import table, directories are listed with FindFirstFile(Ex)W). The
// executable's import entries for these functions are redirected to the hooks below:
//   - CreateFileW/A for reading rom\<path> open the mod's file when the overlay has one;
//   - FindFirstFile(Ex)W on a rom\ directory returns the real entries merged with the
//     mods' entries (a mod file with the same name replaces the real one);
//   - GetFileAttributesExW reports mod files and mod-only directories.
// The index is built once at startup and never changes afterwards, so the hooks read it
// without locking. Only the synthetic find handles need a mutex.
#include "content_overlay.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "log.h"

namespace swml {
namespace {

struct OverlayChild {
    std::wstring name;      // original case, as shown in directory listings
    std::wstring fullPath;  // file or directory inside a mod's content folder
    bool directory = false;
};
struct OverlayDir {
    std::wstring modDir;                // a mod folder holding this directory
    std::vector<OverlayChild> children;
};

std::wstring g_romPrefix;                                  // lowercase "<game>\rom\"
std::unordered_map<std::wstring, std::wstring> g_files;    // lowercase rel path -> mod file
std::unordered_map<std::wstring, OverlayDir> g_dirs;       // lowercase rel dir ("" = rom)
std::set<std::string> g_activeMods;                        // lowercase ModEntry::name
bool g_installed = false;

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetAttributesExFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using FindFirstWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstExWFn = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindNextWFn = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
using FindCloseFn = BOOL(WINAPI*)(HANDLE);

// Originals: the kernel32 functions themselves until the import entries are redirected.
CreateFileWFn g_createFileW = &CreateFileW;
CreateFileAFn g_createFileA = &CreateFileA;
GetAttributesExFn g_getAttributesEx = &GetFileAttributesExW;
FindFirstWFn g_findFirstW = &FindFirstFileW;
FindFirstExWFn g_findFirstExW = &FindFirstFileExW;
FindNextWFn g_findNextW = &FindNextFileW;
FindCloseFn g_findClose = &FindClose;

// Logging budget: the first redirections and merged listings are reported, not every call.
std::mutex g_logMutex;
std::set<std::wstring> g_loggedPaths;
constexpr size_t kMaxLoggedPaths = 64;
constexpr size_t kMaxOverlayFiles = 100000;

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

void LogOnce(const std::wstring& key, const char* what) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_loggedPaths.size() >= kMaxLoggedPaths || !g_loggedPaths.insert(key).second) return;
    LogInfo("Content: %s rom\\%s", what, WideToUtf8(key).c_str());
}

// Maps a path the game passes to the lowercase path relative to rom\ ("" for rom itself).
bool RelativeToRom(LPCWSTR path, std::wstring& relative) {
    if (!path || !*path || g_romPrefix.empty()) return false;
    wchar_t buffer[1024];
    const DWORD length = GetFullPathNameW(path, static_cast<DWORD>(_countof(buffer)), buffer, nullptr);
    if (!length || length >= _countof(buffer)) return false;
    std::wstring full(buffer, length);
    std::replace(full.begin(), full.end(), L'/', L'\\');
    full = Lower(full);
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    if (full + L"\\" == g_romPrefix) { relative.clear(); return true; }
    if (full.compare(0, g_romPrefix.size(), g_romPrefix) != 0) return false;
    relative = full.substr(g_romPrefix.size());
    return true;
}

// Case-insensitive '*' / '?' matching, as used by FindFirstFile patterns ("*.*" = all).
bool WildcardMatch(const wchar_t* pattern, const wchar_t* name) {
    if (!wcscmp(pattern, L"*.*") || !wcscmp(pattern, L"*")) return true;
    const wchar_t *star = nullptr, *resume = nullptr;
    while (*name) {
        if (*pattern == L'?' || std::towlower(*pattern) == std::towlower(*name)) { ++pattern; ++name; continue; }
        if (*pattern == L'*') { star = pattern++; resume = name; continue; }
        if (star) { pattern = star + 1; name = ++resume; continue; }
        return false;
    }
    while (*pattern == L'*') ++pattern;
    return !*pattern;
}

bool IsReadOnlyOpen(DWORD access, DWORD disposition) {
    const DWORD writeAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
                              FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    return disposition == OPEN_EXISTING && !(access & writeAccess);
}

const std::wstring* OverlayFile(LPCWSTR path) {
    std::wstring relative;
    if (!RelativeToRom(path, relative)) return nullptr;
    auto it = g_files.find(relative);
    if (it == g_files.end()) return nullptr;
    LogOnce(relative, "reading mod file for");
    return &it->second;
}

// ---- Synthetic find handles ------------------------------------------------------------
struct MergedFind {
    std::vector<WIN32_FIND_DATAW> entries;
    size_t next = 0;
};
std::mutex g_findMutex;
std::set<void*> g_findHandles;

bool TakeMerged(HANDLE handle, MergedFind** out) {
    std::lock_guard<std::mutex> lock(g_findMutex);
    auto it = g_findHandles.find(handle);
    if (it == g_findHandles.end()) return false;
    *out = static_cast<MergedFind*>(*it);
    return true;
}

// Lists `pattern` (dir\spec) merged with the overlay, or returns false to let the original
// call run untouched (path outside rom\, or no mod entry in that directory).
bool MergedListing(LPCWSTR pattern, FINDEX_INFO_LEVELS level, FINDEX_SEARCH_OPS op, DWORD flags,
                   std::vector<WIN32_FIND_DATAW>& entries) {
    if (!pattern) return false;
    std::wstring text(pattern);
    const size_t slash = text.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? L"." : text.substr(0, slash);
    const std::wstring spec = slash == std::wstring::npos ? text : text.substr(slash + 1);
    std::wstring relative;
    if (spec.empty() || !RelativeToRom(directory.c_str(), relative)) return false;
    auto dir = g_dirs.find(relative);
    if (dir == g_dirs.end() || dir->second.children.empty()) return false;

    WIN32_FIND_DATAW data;
    HANDLE real = g_findFirstExW(pattern, level, &data, op, nullptr, flags);
    if (real != INVALID_HANDLE_VALUE) {
        do entries.push_back(data); while (g_findNextW(real, &data));
        g_findClose(real);
    }
    size_t added = 0;
    for (const OverlayChild& child : dir->second.children) {
        if (!WildcardMatch(spec.c_str(), child.name.c_str())) continue;
        HANDLE one = g_findFirstExW(child.fullPath.c_str(), level, &data, FindExSearchNameMatch, nullptr, 0);
        if (one == INVALID_HANDLE_VALUE) continue;
        g_findClose(one);
        wcsncpy_s(data.cFileName, child.name.c_str(), _TRUNCATE);
        data.cAlternateFileName[0] = 0;
        auto same = std::find_if(entries.begin(), entries.end(), [&](const WIN32_FIND_DATAW& e) {
            return _wcsicmp(e.cFileName, child.name.c_str()) == 0;
        });
        if (same == entries.end()) { entries.push_back(data); ++added; }
        else if (!child.directory) *same = data; // the mod's file replaces the game's
    }
    LogOnce(relative.empty() ? L"(root)" : relative, "merged mod entries into listing of");
    return true;
}

HANDLE OpenMerged(std::vector<WIN32_FIND_DATAW>& entries, LPVOID out) {
    if (entries.empty()) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    auto merged = std::make_unique<MergedFind>();
    merged->entries = std::move(entries);
    *static_cast<WIN32_FIND_DATAW*>(out) = merged->entries[0];
    merged->next = 1;
    std::lock_guard<std::mutex> lock(g_findMutex);
    void* handle = merged.release();
    g_findHandles.insert(handle);
    return static_cast<HANDLE>(handle);
}

// ---- Hooks --------------------------------------------------------------------------------
HANDLE WINAPI HkCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                            DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (IsReadOnlyOpen(access, disposition))
        if (const std::wstring* mod = OverlayFile(path))
            return g_createFileW(mod->c_str(), access, share, security, disposition, flags, templateFile);
    return g_createFileW(path, access, share, security, disposition, flags, templateFile);
}

HANDLE WINAPI HkCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                            DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (path && IsReadOnlyOpen(access, disposition)) {
        wchar_t wide[1024];
        if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, static_cast<int>(_countof(wide))))
            if (const std::wstring* mod = OverlayFile(wide))
                return g_createFileW(mod->c_str(), access, share, security, disposition, flags, templateFile);
    }
    return g_createFileA(path, access, share, security, disposition, flags, templateFile);
}

BOOL WINAPI HkGetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    std::wstring relative;
    if (RelativeToRom(path, relative)) {
        auto file = g_files.find(relative);
        if (file != g_files.end()) return g_getAttributesEx(file->second.c_str(), level, out);
        auto dir = g_dirs.find(relative);
        if (dir != g_dirs.end() && !g_getAttributesEx(path, level, out))
            return g_getAttributesEx(dir->second.modDir.c_str(), level, out); // mod-only directory
    }
    return g_getAttributesEx(path, level, out);
}

HANDLE WINAPI HkFindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID out,
                                 FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    std::vector<WIN32_FIND_DATAW> entries;
    if (!filter && MergedListing(pattern, level, op, flags, entries)) return OpenMerged(entries, out);
    return g_findFirstExW(pattern, level, out, op, filter, flags);
}

HANDLE WINAPI HkFindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW out) {
    std::vector<WIN32_FIND_DATAW> entries;
    if (MergedListing(pattern, FindExInfoStandard, FindExSearchNameMatch, 0, entries)) return OpenMerged(entries, out);
    return g_findFirstW(pattern, out);
}

BOOL WINAPI HkFindNextFileW(HANDLE handle, LPWIN32_FIND_DATAW out) {
    MergedFind* merged = nullptr;
    if (!TakeMerged(handle, &merged)) return g_findNextW(handle, out);
    if (merged->next >= merged->entries.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    *out = merged->entries[merged->next++];
    return TRUE;
}

BOOL WINAPI HkFindClose(HANDLE handle) {
    {
        std::lock_guard<std::mutex> lock(g_findMutex);
        auto it = g_findHandles.find(handle);
        if (it != g_findHandles.end()) {
            delete static_cast<MergedFind*>(*it);
            g_findHandles.erase(it);
            return TRUE;
        }
    }
    return g_findClose(handle);
}

// ---- Index ----------------------------------------------------------------------------------
void IndexDirectory(const std::wstring& diskDir, const std::wstring& relative, const ModEntry& mod,
                    size_t& files, size_t& conflicts) {
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((diskDir + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::wstring child = relative.empty() ? Lower(name) : relative + L"\\" + Lower(name);
        const std::wstring full = diskDir + L"\\" + name;
        OverlayDir& parent = g_dirs[relative];
        auto existing = std::find_if(parent.children.begin(), parent.children.end(),
                                     [&](const OverlayChild& c) { return _wcsicmp(c.name.c_str(), name.c_str()) == 0; });
        if (directory) {
            if (existing == parent.children.end()) parent.children.push_back({name, full, true});
            OverlayDir& sub = g_dirs[child];
            if (sub.modDir.empty()) sub.modDir = full;
            IndexDirectory(full, child, mod, files, conflicts);
        } else if (files < kMaxOverlayFiles) {
            if (g_files.count(child)) {
                ++conflicts;
                LogWarn("Content: rom\\%s is provided by several mods; '%s' is ignored for it",
                        WideToUtf8(child).c_str(), mod.name.c_str());
                continue;
            }
            g_files[child] = full;
            if (existing == parent.children.end()) parent.children.push_back({name, full, false});
            ++files;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

// Redirects the executable's import entries for `names` (KERNEL32.dll). Every entry must still
// hold the kernel32 function itself; otherwise nothing is written (another tool hooked it).
struct ImportPatch { const char* name; void* hook; void** original; void** slot; };
bool PatchImports(ImportPatch* patches, size_t count) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!base || !kernel) return false;
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; imports->Name; ++imports) {
            if (_stricmp(reinterpret_cast<const char*>(base + imports->Name), "KERNEL32.dll") != 0) continue;
            if (!imports->OriginalFirstThunk) return false;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                for (size_t i = 0; i < count; ++i)
                    if (!strcmp(reinterpret_cast<const char*>(byName->Name), patches[i].name))
                        patches[i].slot = reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
        for (size_t i = 0; i < count; ++i) {
            if (!patches[i].slot) { LogError("Content: KERNEL32!%s is not imported; overlay disabled", patches[i].name); return false; }
            if (*patches[i].slot != reinterpret_cast<void*>(GetProcAddress(kernel, patches[i].name))) {
                LogError("Content: KERNEL32!%s import already redirected; overlay disabled", patches[i].name);
                return false;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            DWORD old = 0, ignored = 0;
            if (!VirtualProtect(patches[i].slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *patches[i].original = *patches[i].slot;
            InterlockedExchangePointer(patches[i].slot, patches[i].hook);
            VirtualProtect(patches[i].slot, sizeof(void*), old, &ignored);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool BuildIndex(const std::vector<ModEntry>& mods, const std::wstring& gameDir) {
    g_romPrefix = Lower(gameDir + L"\\rom\\");
    size_t files = 0, conflicts = 0, contributors = 0;
    for (const ModEntry& mod : mods) {
        if (!mod.enabled || mod.contentPath.empty()) continue;
        const size_t before = files;
        IndexDirectory(mod.contentPath, L"", mod, files, conflicts);
        if (files > before) {
            ++contributors;
            g_activeMods.insert(LowerAscii(mod.name));
            LogInfo("Content: mod '%s' provides %zu file(s)", mod.name.c_str(), files - before);
        }
    }
    if (files) LogInfo("Content: %zu file(s) from %zu mod(s) overlay rom\\ (%zu conflict(s))", files, contributors, conflicts);
    return files != 0;
}

}  // namespace

bool InstallContentOverlay(const std::vector<ModEntry>& mods) {
    if (g_installed) return true;
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::wstring gameDir(exe, length);
    gameDir.erase(gameDir.find_last_of(L'\\'));
    if (!BuildIndex(mods, gameDir)) return false;
    ImportPatch patches[] = {
        {"CreateFileW", reinterpret_cast<void*>(&HkCreateFileW), reinterpret_cast<void**>(&g_createFileW), nullptr},
        {"CreateFileA", reinterpret_cast<void*>(&HkCreateFileA), reinterpret_cast<void**>(&g_createFileA), nullptr},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&HkGetFileAttributesExW), reinterpret_cast<void**>(&g_getAttributesEx), nullptr},
        {"FindFirstFileW", reinterpret_cast<void*>(&HkFindFirstFileW), reinterpret_cast<void**>(&g_findFirstW), nullptr},
        {"FindFirstFileExW", reinterpret_cast<void*>(&HkFindFirstFileExW), reinterpret_cast<void**>(&g_findFirstExW), nullptr},
        {"FindNextFileW", reinterpret_cast<void*>(&HkFindNextFileW), reinterpret_cast<void**>(&g_findNextW), nullptr},
        {"FindClose", reinterpret_cast<void*>(&HkFindClose), reinterpret_cast<void**>(&g_findClose), nullptr},
    };
    if (!PatchImports(patches, _countof(patches))) {
        g_files.clear(); g_dirs.clear(); g_activeMods.clear();
        return false;
    }
    g_installed = true;
    LogInfo("Content: overlay installed on the game's file imports");
    return true;
}

bool ContentOverlayActive(const std::string& modName) {
    return g_installed && g_activeMods.count(LowerAscii(modName)) != 0;
}

}  // namespace swml
