#include "../src/discovery_progress.h"
#include "../src/log.h"

#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

int main(int argc, char** argv) {
    using swml::discovery_progress::State;
    State s;
    assert(s.Update(nullptr, 0, 0, nullptr, false));
    assert(!s.finished && s.Line().find("--% 0/?") != std::string::npos);
    assert(s.Update("scan", 0, 1000, "symbol", false));
    unsigned checkpoints = 1;
    for (size_t i = 1; i <= 1000; ++i)
        if (s.Update("scan", i, 1000, "symbol", false)) ++checkpoints;
    assert(checkpoints == 11);
    assert(s.finished && s.percent == 100);
    assert(s.Line().find("compatibility not implied") != std::string::npos);
    assert(!s.Update("scan", 1000, 1000, "symbol", false));
    assert(s.Update("scan", 1000, 1000, "missing symbol", true));
    assert(s.Line().find("FAILED") != std::string::npos);
    assert(s.Update("scan", 0, 1000, "symbol", false));
    assert(s.Update("scan", 1, 1000, "next symbol", false));
    const size_t max = (std::numeric_limits<size_t>::max)();
    s.Update("large", max - 1, max, "", false);
    assert(!s.finished && s.percent <= 99);
    s.Update("large", max, max, "", false);
    assert(s.finished && s.percent == 100);
    s.Update("bad\r\nphase", 2, 1, "\x1b[31m\ttext", true);
    assert(s.Line().find('\r') == std::string::npos);
    assert(s.Line().find('\n') == std::string::npos);
    assert(s.Line().find('\x1b') == std::string::npos);
    assert(swml::discovery_progress::SafeText(std::string(300, 'a').c_str(), 160).size() == 160);

    // This executable must be built into an isolated temporary directory: LogInit
    // writes next to the executable, never into a live game's directory.
    wchar_t exe[MAX_PATH] = {};
    assert(GetModuleFileNameW(nullptr, exe, MAX_PATH));
    auto dir = std::filesystem::path(exe).parent_path();
    std::filesystem::create_directories(dir / "mods");
    std::ofstream(dir / "mods" / "_modloader.ini") << "cfg.open_terminal_startup=0\n";
    HWND originalConsole = GetConsoleWindow();
    swml::LogDiscoveryProgress("before init", 0, 1, "safe no-op");
    swml::LogInit();
    assert(!swml::GetConfig().openTerminalOnStartup);
    assert(!swml::IsLogTerminalOpen());
    for (size_t i = 0; i <= 10000; ++i)
        swml::LogDiscoveryProgress("scan", i, 10000, "symbol");
    assert(swml::GetRecentLogs(250).size() == 11);
    swml::LogDiscoveryProgress("scan", 10000, 10000, "missing symbol", true);
    auto logs = swml::GetRecentLogs();
    assert(logs.front().level == swml::LogLevel::Warn);
    assert(logs.front().message.find("100%") != std::string::npos);
    std::thread progress([] {
        for (size_t i = 0; i <= 10000; ++i)
            swml::LogDiscoveryProgress("threaded", i, 10000, "symbol");
    });
    std::thread ordinary([] {
        for (int i = 0; i < 40; ++i) swml::LogInfo("ordinary %d", i);
    });
    progress.join();
    ordinary.join();
    logs = swml::GetRecentLogs(250);
    assert(logs.size() == 63);
    for (size_t i = 1; i < logs.size(); ++i) assert(logs[i - 1].id > logs[i].id);
    assert(!swml::IsLogTerminalOpen() && GetConsoleWindow() == originalConsole);
    std::ifstream file(dir / "StormworksModLoader.log");
    std::string line;
    size_t lines = 0;
    while (std::getline(file, line)) {
        assert(!line.empty() && line.front() == '[');
        ++lines;
    }
    assert(lines == logs.size());

    if (argc > 1 && std::strcmp(argv[1], "--console") == 0) {
        // Explicit terminal test only: production progress must never open it.
        swml::OpenLogTerminal();
        HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
        if (console == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(console, &consoleInfo)) {
            if (console != INVALID_HANDLE_VALUE) CloseHandle(console);
            swml::CloseLogTerminal();
            std::puts("discovery progress tests passed; console buffer test unavailable on this host");
            return 0;
        }
        auto currentRow = [&](SHORT offset) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            assert(GetConsoleScreenBufferInfo(console, &info));
            std::string row(static_cast<size_t>(info.dwSize.X), ' ');
            COORD start = {0, static_cast<SHORT>(info.dwCursorPosition.Y + offset)};
            DWORD read = 0;
            assert(ReadConsoleOutputCharacterA(console, &row[0],
                static_cast<DWORD>(row.size()), start, &read));
            return row;
        };
        swml::LogDiscoveryProgress("console", 0, 100, "first symbol");
        assert(currentRow(0).find("[Discovery]") == 0);
        swml::LogInfo("ordinary console marker");
        assert(currentRow(0).find("[Discovery]") == 0);
        assert(currentRow(-1).find("ordinary console marker") != std::string::npos);
        assert(GetConsoleScreenBufferInfo(console, &consoleInfo));
        SHORT completionStart = consoleInfo.dwCursorPosition.Y;
        swml::LogDiscoveryProgress("console", 100, 100, "missing symbol", true);
        assert(GetConsoleScreenBufferInfo(console, &consoleInfo));
        std::string completion;
        for (SHORT row = completionStart; row < consoleInfo.dwCursorPosition.Y; ++row)
            completion += currentRow(static_cast<SHORT>(row - consoleInfo.dwCursorPosition.Y));
        assert(completion.find("100%") != std::string::npos);
        assert(completion.find("FAILED") != std::string::npos);
        size_t first = completion.find("[Discovery]");
        assert(first != std::string::npos && completion.find("[Discovery]", first + 11) == std::string::npos);
        swml::LogInfo("after progress marker");
        assert(currentRow(-1).find("after progress marker") != std::string::npos);
        assert(currentRow(0).find("[Discovery]") == std::string::npos);
        CloseHandle(console);
        swml::CloseLogTerminal();
    }
    std::puts("discovery progress tests passed");
}
