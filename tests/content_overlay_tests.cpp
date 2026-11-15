// End-to-end test of the content overlay against real files: a fake rom\ next to this
// executable, mod content folders in %TEMP%, and the overlay installed on THIS executable's
// own kernel32 imports. Never touches Stormworks.
#include "../src/content_overlay.cpp"
#include <cassert>
#include <cstdio>
#include <fstream>

namespace swml {
void LogInfo(const char*, ...) {}
void LogWarn(const char*, ...) {}
void LogError(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
std::string WideToUtf8(const std::wstring& w) {
    std::string s; for (wchar_t c : w) s.push_back(static_cast<char>(c)); return s;
}
std::string LowerAscii(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }
}

namespace {
std::wstring g_exeDir, g_temp;

void Write(const std::wstring& path, const char* text) {
    const size_t slash = path.find_last_of(L'\\');
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos && at <= slash; at = path.find(L'\\', at + 1))
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
    std::ofstream(path, std::ios::binary) << text;
}
std::string ReadThrough(HANDLE file) {
    assert(file != INVALID_HANDLE_VALUE);
    char buffer[64] = {}; DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    return std::string(buffer, read);
}
std::string ReadW(const std::wstring& path) {
    return ReadThrough(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
}
std::vector<std::wstring> ListW(const std::wstring& pattern, bool ex, std::vector<WIN32_FIND_DATAW>* data = nullptr) {
    WIN32_FIND_DATAW entry;
    HANDLE find = ex ? FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0)
                     : FindFirstFileW(pattern.c_str(), &entry);
    std::vector<std::wstring> names;
    if (find == INVALID_HANDLE_VALUE) return names;
    do { names.push_back(entry.cFileName); if (data) data->push_back(entry); } while (FindNextFileW(find, &entry));
    assert(GetLastError() == ERROR_NO_MORE_FILES);
    assert(FindClose(find));
    std::sort(names.begin(), names.end());
    return names;
}
void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW entry;
    HANDLE find = swml::g_findFirstW((dir + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(dir + L"\\" + name);
        else DeleteFileW((dir + L"\\" + name).c_str());
    } while (swml::g_findNextW(find, &entry));
    swml::g_findClose(find);
    RemoveDirectoryW(dir.c_str());
}
}

int main() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    g_exeDir = buffer; g_exeDir.erase(g_exeDir.find_last_of(L'\\'));
    GetTempPathW(MAX_PATH, buffer);
    g_temp = std::wstring(buffer) + L"sf_content_test";
    const std::wstring rom = g_exeDir + L"\\rom";
    RemoveTree(rom); RemoveTree(g_temp);
    Write(rom + L"\\data\\definitions\\a.xml", "game a");
    Write(rom + L"\\data\\definitions\\b.xml", "game b");
    Write(rom + L"\\data\\definitions\\notes.txt", "game notes");
    Write(g_temp + L"\\m1\\content\\data\\definitions\\b.xml", "mod b, longer");
    Write(g_temp + L"\\m1\\content\\data\\definitions\\c.xml", "mod c");
    Write(g_temp + L"\\m1\\content\\data\\newdir\\d.txt", "mod d");
    Write(g_temp + L"\\m2\\content\\data\\definitions\\C.XML", "mod2 c");  // conflict: m1 wins
    Write(g_temp + L"\\m2\\content\\meshes\\e.mesh", "mod2 e");
    Write(g_temp + L"\\m3\\content\\data\\definitions\\z.xml", "disabled z");

    std::vector<swml::ModEntry> mods(3);
    mods[0].name = "M1"; mods[0].contentPath = g_temp + L"\\m1\\content";
    mods[1].name = "m2"; mods[1].contentPath = g_temp + L"\\m2\\content";
    mods[2].name = "m3"; mods[2].contentPath = g_temp + L"\\m3\\content"; mods[2].enabled = false;
    assert(!swml::ContentOverlayActive("m1"));
    assert(swml::InstallContentOverlay(mods));
    assert(swml::InstallContentOverlay(mods)); // idempotent
    assert(swml::ContentOverlayActive("m1") && swml::ContentOverlayActive("M2") && !swml::ContentOverlayActive("m3"));

    // Reads: mod files replace or extend rom\, game files stay reachable, disabled mods are absent.
    const std::wstring defs = rom + L"\\data\\definitions\\";
    assert(ReadW(defs + L"a.xml") == "game a");
    assert(ReadW(defs + L"b.xml") == "mod b, longer");
    assert(ReadW(defs + L"C.xml") == "mod c");
    assert(ReadW(rom + L"\\meshes\\e.mesh") == "mod2 e");
    assert(CreateFileW((defs + L"z.xml").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    // Relative, forward-slash and ANSI paths resolve the same way.
    SetCurrentDirectoryW(g_exeDir.c_str());
    assert(ReadThrough(CreateFileA("rom/data/definitions/c.xml", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)) == "mod c");
    assert(ReadThrough(CreateFileA("rom\\data\\..\\data\\definitions\\b.xml", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)) == "mod b, longer");
    // Writes are never redirected: they reach the game's file.
    assert(ReadThrough(CreateFileW((defs + L"b.xml").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)) == "game b");
    assert(ReadThrough(CreateFileW((defs + L"b.xml").c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, 0, nullptr)) == "game b");

    // Listings merge both sides without duplicates, honour the pattern and report mod sizes.
    for (bool ex : {false, true}) {
        std::vector<WIN32_FIND_DATAW> data;
        const auto xml = ListW(defs + L"*.xml", ex, &data);
        assert((xml == std::vector<std::wstring>{L"a.xml", L"b.xml", L"c.xml"}));
        for (const auto& d : data) if (!_wcsicmp(d.cFileName, L"b.xml")) assert(d.nFileSizeLow == 13);
        const auto all = ListW(defs + L"*", ex);
        assert((all == std::vector<std::wstring>{L".", L"..", L"a.xml", L"b.xml", L"c.xml", L"notes.txt"}));
        assert((ListW(rom + L"\\data\\*", ex) == std::vector<std::wstring>{L".", L"..", L"definitions", L"newdir"}));
        assert((ListW(rom + L"\\data\\newdir\\*.txt", ex) == std::vector<std::wstring>{L"d.txt"}));
        assert((ListW(rom + L"\\meshes\\*", ex) == std::vector<std::wstring>{L"e.mesh"})); // mod-only directory
        assert(ListW(defs + L"*.mesh", ex).empty() && GetLastError() == ERROR_FILE_NOT_FOUND);
    }
    // Attributes: mod files and mod-only directories exist for the game.
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    assert(GetFileAttributesExW((defs + L"c.xml").c_str(), GetFileExInfoStandard, &attributes) && attributes.nFileSizeLow == 5);
    assert(GetFileAttributesExW((rom + L"\\data\\newdir").c_str(), GetFileExInfoStandard, &attributes) &&
           (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    assert(!GetFileAttributesExW((defs + L"z.xml").c_str(), GetFileExInfoStandard, &attributes));
    // Paths outside rom\ are untouched (the mod folders themselves list normally).
    assert((ListW(g_temp + L"\\m2\\content\\data\\definitions\\*.xml", false) == std::vector<std::wstring>{L"C.XML"}));
    assert(ReadW(g_temp + L"\\m3\\content\\data\\definitions\\z.xml") == "disabled z");
    // Unknown handles still reach the real FindClose.
    assert(!FindClose(reinterpret_cast<HANDLE>(0x1234)));

    RemoveTree(rom); RemoveTree(g_temp);
    puts("PASS: content overlay; reads, replacement, conflicts, relative/ANSI paths, write passthrough, merged listings, attributes");
}
