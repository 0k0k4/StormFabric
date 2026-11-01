#include "log.h"
#include "discovery_progress.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

namespace swml {
namespace {

wchar_t g_logPath[MAX_PATH * 2] = {};
wchar_t g_iniPath[MAX_PATH * 2] = {};
bool g_ready = false;
CRITICAL_SECTION g_cs;
bool g_csInit = false;

ModLoaderConfig g_cfg;
HANDLE g_conOut = INVALID_HANDLE_VALUE;
bool g_consoleAllocated = false;
discovery_progress::State g_discovery;
bool g_progressVisible = false;
ULONGLONG g_progressLastDrawTick = 0;

// All progress/console helpers are called with g_cs held; none call Log*.
void ClearProgressLine() {
    if (!g_progressVisible || g_conOut == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_conOut, &info)) {
        COORD start = {0, info.dwCursorPosition.Y};
        DWORD written = 0;
        FillConsoleOutputCharacterA(g_conOut, ' ', info.dwSize.X, start, &written);
        SetConsoleCursorPosition(g_conOut, start);
    }
    g_progressVisible = false;
}

void DrawProgressLine() {
    if (!g_discovery.valid || g_conOut == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_conOut, &info) || info.dwSize.X < 2) return;
    // Reserve the last column to avoid automatic wrapping/scrolling.
    std::string line = g_discovery.Line();
    SHORT width = (std::min)(info.dwSize.X,
        static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1));
    if (width < 2) return;
    line.resize((std::min)(line.size(), static_cast<size_t>(width - 1)));
    SetConsoleTextAttribute(g_conOut, g_discovery.failed
        ? FOREGROUND_RED | FOREGROUND_INTENSITY
        : FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    DWORD written = 0;
    WriteFile(g_conOut, line.data(), (DWORD)line.size(), &written, nullptr);
    if (g_discovery.finished) {
        WriteFile(g_conOut, "\r\n", 2, &written, nullptr);
    } else {
        g_progressVisible = true;
    }
}

constexpr size_t kMaxHistory = 250;
std::deque<LogEntry> g_history;
uint64_t g_nextLogId = 1;

std::deque<NotificationItem> g_notifs;
uint64_t g_nextNotifId = 1;
uint64_t g_notifRevision = 1;

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string ReadFileText(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > (1 << 20)) {
        CloseHandle(h);
        return {};
    }
    std::string buf(sz, '\0');
    DWORD got = 0;
    ReadFile(h, buf.data(), sz, &got, nullptr);
    CloseHandle(h);
    buf.resize(got);
    return buf;
}

std::map<std::string, int> ReadIniMap() {
    std::map<std::string, int> m;
    std::string text = ReadFileText(g_iniPath);
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
        m[Lower(line.substr(0, eq))] = line[eq + 1] == '1' ? 1 : 0;
    }
    return m;
}

void WriteIniMap(const std::map<std::string, int>& m) {
    std::string out =
        "# Etat des mods C++ et parametres (1 = active, 0 = desactive) - StormworksModLoader\r\n";
    for (const auto& kv : m) out += kv.first + "=" + (kv.second ? "1" : "0") + "\r\n";
    HANDLE h = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
}

void LoadConfigFromIni() {
    std::map<std::string, int> m = ReadIniMap();
    auto get = [&](const char* key, bool defVal) -> bool {
        auto it = m.find(key);
        return it == m.end() ? defVal : (it->second != 0);
    };
    g_cfg.openTerminalOnStartup = get("cfg.open_terminal_startup", true);
    g_cfg.showNotifications     = get("cfg.show_notifications", true);
    g_cfg.notifInfo             = get("cfg.notif_info", true);
    g_cfg.notifWarn             = get("cfg.notif_warn", true);
    g_cfg.notifError            = get("cfg.notif_error", true);
    g_cfg.verboseDebug          = get("cfg.verbose_debug", true);
    g_cfg.hotReloadMods         = get("cfg.hot_reload_mods", true);
    g_cfg.autoOpenModsMenu      = get("cfg.auto_open_mods_menu", false);

    char env[8] = {};
    if (GetEnvironmentVariableA("SWML_AUTOOPEN", env, sizeof(env)) > 0 && env[0] == '1') {
        g_cfg.autoOpenModsMenu = true;
    }
}

const char* LevelTag(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO ";
}

WORD LevelColorAttr(LogLevel lv, bool isModMsg) {
    switch (lv) {
        case LogLevel::Debug: return FOREGROUND_BLUE | FOREGROUND_GREEN;  // Cyan foncé
        case LogLevel::Info:
            return isModMsg
                       ? (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY)  // Cyan vif
                       : (FOREGROUND_GREEN | FOREGROUND_INTENSITY);                   // Vert vif
        case LogLevel::Warn:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;          // Jaune vif
        case LogLevel::Error:
            return FOREGROUND_RED | FOREGROUND_INTENSITY;                             // Rouge vif
    }
    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
}

void WriteEntryToConsoleRaw(const LogEntry& e) {
    if (g_conOut == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;

    // [HH:MM:SS.mmm] en gris sombre
    SetConsoleTextAttribute(g_conOut, FOREGROUND_INTENSITY);
    std::string ts = "[" + e.timeStr + "] ";
    WriteFile(g_conOut, ts.data(), (DWORD)ts.size(), &written, nullptr);

    // [LEVEL] en couleur vive
    bool isMod = e.message.rfind("[Mod:", 0) == 0;
    SetConsoleTextAttribute(g_conOut, LevelColorAttr(e.level, isMod));
    std::string tag = std::string("[") + LevelTag(e.level) + "] ";
    WriteFile(g_conOut, tag.data(), (DWORD)tag.size(), &written, nullptr);

    // Corps du message
    WORD bodyColor = (e.level == LogLevel::Error)
                         ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                         : (e.level == LogLevel::Warn)
                               ? (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
                               : (e.level == LogLevel::Debug)
                                     ? (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
                                     : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
                                        FOREGROUND_INTENSITY);
    SetConsoleTextAttribute(g_conOut, bodyColor);
    std::string body = e.message + "\r\n";
    WriteFile(g_conOut, body.data(), (DWORD)body.size(), &written, nullptr);
}

void WriteEntryToConsole(const LogEntry& e) {
    ClearProgressLine();
    WriteEntryToConsoleRaw(e);
    if (g_discovery.valid && !g_discovery.finished) DrawProgressLine();
}

void PrintConsoleBanner() {
    if (g_conOut == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    SetConsoleTextAttribute(g_conOut,
                            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    const char* banner =
        "================================================================================\r\n"
        "  STORMWORKS C++ MODLOADER  -  TERMINAL DE DEBUG & LOGS EN TEMPS REEL\r\n"
        "  (Astuce : desactivez ce terminal dans MODS -> Settings & Debug si souhaité)\r\n"
        "================================================================================\r\n";
    WriteFile(g_conOut, banner, (DWORD)strlen(banner), &written, nullptr);
    SetConsoleTextAttribute(g_conOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// Déduit automatiquement le niveau si l'appelant utilise Log(...) générique
LogLevel InferLevel(const char* text) {
    if (!text) return LogLevel::Info;
    if (_strnicmp(text, "ERREUR", 6) == 0 || _strnicmp(text, "ERROR", 5) == 0 ||
        strstr(text, "ERREUR:") != nullptr || strstr(text, "ERROR:") != nullptr) {
        return LogLevel::Error;
    }
    if (_strnicmp(text, "WARN", 4) == 0 || _strnicmp(text, "ATTENTION", 9) == 0 ||
        strstr(text, "en attente de la dependance") != nullptr ||
        strstr(text, "ignore : dependance") != nullptr) {
        return LogLevel::Warn;
    }
    if (_strnicmp(text, "DEBUG", 5) == 0) {
        return LogLevel::Debug;
    }
    return LogLevel::Info;
}

// Caller owns g_cs. Shared by normal logs and progress checkpoints without
// recursively entering the logger/critical section.
void AppendEntryLocked(LogLevel level, const char* message, bool console = true) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);

    LogEntry entry;
    entry.id = g_nextLogId++;
    entry.timeStr = timeBuf;
    entry.level = level;
    entry.message = message;

    g_history.push_back(entry);
    while (g_history.size() > kMaxHistory) g_history.pop_front();

    // Écriture dans StormworksModLoader.log
    std::string fileLine = "[" + entry.timeStr + "] [" + LevelTag(level) + "] " +
        entry.message + "\r\n";
    if (!fileLine.empty()) {
        HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, fileLine.data(), (DWORD)fileLine.size(), &written, nullptr);
            CloseHandle(h);
        }
    }

    // Écriture dans le terminal de debug Windows s'il est ouvert
    if (console && g_conOut != INVALID_HANDLE_VALUE) {
        WriteEntryToConsole(entry);
    }
}

void LogV(LogLevel level, bool infer, const char* fmt, va_list ap) {
    if (!g_ready) return;
    if (level == LogLevel::Debug && !g_cfg.verboseDebug) return;

    char msgBuf[1024];
    int m = _vsnprintf_s(msgBuf, sizeof(msgBuf), _TRUNCATE, fmt, ap);
    if (m < 0) msgBuf[sizeof(msgBuf) - 1] = '\0';

    if (infer) level = InferLevel(msgBuf);
    if (level == LogLevel::Debug && !g_cfg.verboseDebug) return;

    EnterCriticalSection(&g_cs);
    AppendEntryLocked(level, msgBuf);
    LeaveCriticalSection(&g_cs);
}

}  // namespace

void LogInit() {
    if (g_ready) return;
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }

    DWORD n = GetModuleFileNameW(nullptr, g_logPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(g_logPath, L'\\');
    if (!slash) return;
    *slash = L'\0';

    swprintf_s(g_iniPath, L"%s\\mods", g_logPath);
    CreateDirectoryW(g_iniPath, nullptr);
    swprintf_s(g_iniPath, L"%s\\mods\\_modloader.ini", g_logPath);
    swprintf_s(g_logPath, L"%s\\StormworksModLoader.log", g_logPath);

    HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    g_ready = true;
    LoadConfigFromIni();

    if (g_cfg.openTerminalOnStartup) {
        OpenLogTerminal();
    }
}

ModLoaderConfig& GetConfig() { return g_cfg; }

void SaveConfig() {
    if (!g_ready) return;
    EnterCriticalSection(&g_cs);
    std::map<std::string, int> m = ReadIniMap();
    m["cfg.open_terminal_startup"] = g_cfg.openTerminalOnStartup ? 1 : 0;
    m["cfg.show_notifications"]    = g_cfg.showNotifications ? 1 : 0;
    m["cfg.notif_info"]            = g_cfg.notifInfo ? 1 : 0;
    m["cfg.notif_warn"]            = g_cfg.notifWarn ? 1 : 0;
    m["cfg.notif_error"]           = g_cfg.notifError ? 1 : 0;
    m["cfg.verbose_debug"]         = g_cfg.verboseDebug ? 1 : 0;
    m["cfg.hot_reload_mods"]       = g_cfg.hotReloadMods ? 1 : 0;
    m["cfg.auto_open_mods_menu"]   = g_cfg.autoOpenModsMenu ? 1 : 0;
    WriteIniMap(m);
    LeaveCriticalSection(&g_cs);
}

void OpenLogTerminal() {
    EnterCriticalSection(&g_cs);
    if (g_conOut != INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_cs);
        return;
    }

    if (AllocConsole()) {
        g_consoleAllocated = true;
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Stormworks ModLoader - Debug & Log Terminal");

    // Si user32.dll est déjà chargé et initialisé, désactive le bouton [X] de la console
    if (HWND hwnd = GetConsoleWindow()) {
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            using GetSystemMenuFn = HMENU(WINAPI*)(HWND, BOOL);
            using DeleteMenuFn = BOOL(WINAPI*)(HMENU, UINT, UINT);
            auto getSysMenu = reinterpret_cast<GetSystemMenuFn>(GetProcAddress(u32, "GetSystemMenu"));
            auto delMenu = reinterpret_cast<DeleteMenuFn>(GetProcAddress(u32, "DeleteMenu"));
            if (getSysMenu && delMenu) {
                if (HMENU hMenu = getSysMenu(hwnd, FALSE)) {
                    delMenu(hMenu, SC_CLOSE, MF_BYCOMMAND);
                }
            }
        }
    }

    g_conOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_conOut != INVALID_HANDLE_VALUE) {
        PrintConsoleBanner();
        // Ré-affiche tout l'historique déjà accumulé depuis le lancement du processus
        for (const LogEntry& e : g_history) {
            WriteEntryToConsoleRaw(e);
        }
        if (g_discovery.valid && !g_discovery.finished) DrawProgressLine();
    }
    LeaveCriticalSection(&g_cs);
}

void CloseLogTerminal() {
    EnterCriticalSection(&g_cs);
    if (g_conOut != INVALID_HANDLE_VALUE) {
        ClearProgressLine();
        CloseHandle(g_conOut);
        g_conOut = INVALID_HANDLE_VALUE;
    }
    if (g_consoleAllocated || GetConsoleWindow() != nullptr) {
        FreeConsole();
        g_consoleAllocated = false;
    }
    LeaveCriticalSection(&g_cs);
}

bool IsLogTerminalOpen() {
    return g_conOut != INVALID_HANDLE_VALUE && GetConsoleWindow() != nullptr;
}

void SetLogTerminalOpen(bool open) {
    if (open) OpenLogTerminal();
    else CloseLogTerminal();
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(LogLevel::Info, /*infer=*/true, fmt, ap);
    va_end(ap);
}

void LogLevelMsg(LogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(level, /*infer=*/false, fmt, ap);
    va_end(ap);
}

void LogDebug(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(LogLevel::Debug, /*infer=*/false, fmt, ap);
    va_end(ap);
}

void LogInfo(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(LogLevel::Info, /*infer=*/false, fmt, ap);
    va_end(ap);
}

void LogWarn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(LogLevel::Warn, /*infer=*/false, fmt, ap);
    va_end(ap);
}

void LogError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(LogLevel::Error, /*infer=*/false, fmt, ap);
    va_end(ap);
}

void LogDiscoveryProgress(const char* phase, size_t completed, size_t total,
                          const char* detail, bool failed) {
    if (!g_ready) return;
    EnterCriticalSection(&g_cs);
    // Keep symbol/phase results in terminal scrollback, in addition to the live
    // bar. Byte-scan checkpoints stay file/history-only to avoid flooding it.
    bool scanUpdate = detail && strncmp(detail, "scanning executable sections", 28) == 0;
    bool terminalEvent = !scanUpdate && (!g_discovery.valid ||
        g_discovery.detail != discovery_progress::SafeText(detail, 240) ||
        g_discovery.failed != failed || (total && completed >= total));
    bool checkpoint = g_discovery.Update(phase, completed, total, detail, failed);
    if (checkpoint) {
        std::string line = g_discovery.Line();
        AppendEntryLocked(failed ? LogLevel::Warn : LogLevel::Info, line.c_str(), terminalEvent);
    }
    ULONGLONG now = GetTickCount64();
    if (checkpoint || (!g_discovery.finished && now - g_progressLastDrawTick >= 50)) {
        ClearProgressLine();
        // Completed phases already have a permanent result line; drawing another
        // finished bar would print the same result twice (once truncated).
        if (!g_discovery.finished || !terminalEvent) DrawProgressLine();
        g_progressLastDrawTick = now;
    }
    LeaveCriticalSection(&g_cs);
}

void Notify(LogLevel level, const std::string& title, const std::string& detail,
            float durationSec) {
    // Journalise systématiquement la notification
    LogLevelMsg(level, "[NOTIF] %s — %s", title.c_str(), detail.c_str());

    if (!g_cfg.showNotifications) return;
    if (level == LogLevel::Info && !g_cfg.notifInfo) return;
    if (level == LogLevel::Warn && !g_cfg.notifWarn) return;
    if (level == LogLevel::Error && !g_cfg.notifError) return;

    if (durationSec <= 0.0f) {
        switch (level) {
            case LogLevel::Error: durationSec = 8.5f; break;
            case LogLevel::Warn:  durationSec = 6.5f; break;
            default:              durationSec = 4.5f; break;
        }
    }

    EnterCriticalSection(&g_cs);
    uint64_t now = GetTickCount64();
    NotificationItem item;
    item.id = g_nextNotifId++;
    item.level = level;
    item.title = title;
    item.detail = detail;
    item.expireTickMs = now + (uint64_t)(durationSec * 1000.0f);

    g_notifs.push_back(std::move(item));
    while (g_notifs.size() > 8) g_notifs.pop_front();
    ++g_notifRevision;
    LeaveCriticalSection(&g_cs);
}

uint64_t GetActiveNotifications(std::vector<NotificationItem>& out, size_t maxCount) {
    out.clear();
    if (!g_csInit) return 0;

    EnterCriticalSection(&g_cs);
    uint64_t now = GetTickCount64();
    bool removed = false;
    while (!g_notifs.empty() && g_notifs.front().expireTickMs <= now) {
        g_notifs.pop_front();
        removed = true;
    }
    for (auto it = g_notifs.begin(); it != g_notifs.end();) {
        if (it->expireTickMs <= now) {
            it = g_notifs.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) ++g_notifRevision;

    if (g_cfg.showNotifications) {
        for (const NotificationItem& n : g_notifs) {
            if (n.level == LogLevel::Info && !g_cfg.notifInfo) continue;
            if (n.level == LogLevel::Warn && !g_cfg.notifWarn) continue;
            if (n.level == LogLevel::Error && !g_cfg.notifError) continue;
            out.push_back(n);
            if (out.size() >= maxCount) break;
        }
    }
    uint64_t rev = g_notifRevision;
    LeaveCriticalSection(&g_cs);
    return rev;
}

void ClearNotifications() {
    if (!g_csInit) return;
    EnterCriticalSection(&g_cs);
    g_notifs.clear();
    ++g_notifRevision;
    LeaveCriticalSection(&g_cs);
}

std::vector<LogEntry> GetRecentLogs(size_t maxCount) {
    std::vector<LogEntry> res;
    if (!g_csInit) return res;
    EnterCriticalSection(&g_cs);
    size_t count = (std::min)(maxCount, g_history.size());
    res.reserve(count);
    // Du plus récent au plus ancien pour une lecture directe en haut de liste
    for (size_t i = 0; i < count; ++i) {
        res.push_back(g_history[g_history.size() - 1 - i]);
    }
    LeaveCriticalSection(&g_cs);
    return res;
}

void ClearLogs() {
    if (!g_csInit) return;
    EnterCriticalSection(&g_cs);
    g_history.clear();
    HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (g_conOut != INVALID_HANDLE_VALUE) {
        ClearProgressLine();
        PrintConsoleBanner();
        if (g_discovery.valid && !g_discovery.finished) DrawProgressLine();
    }
    LeaveCriticalSection(&g_cs);
    LogInfo("Journal d'evenements vide par l'utilisateur");
}

void OpenLogFile() {
    using ShellExecuteWFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    auto fn = shell ? reinterpret_cast<ShellExecuteWFn>(GetProcAddress(shell, "ShellExecuteW"))
                    : nullptr;
    if (!fn) return;
    fn(nullptr, L"open", g_logPath, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace swml
