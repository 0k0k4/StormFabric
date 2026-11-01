#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

// UTF-8 path for the storage API; do not embed a developer's Windows username.
// Prefer the OpenCode temp directory, or an explicitly supplied test root.
inline std::string TestTempRoot() {
    auto environment = [](const wchar_t* name) {
        DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (!size) return std::wstring();
        std::wstring value(size, L'\0');
        DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
        if (!copied || copied >= size) throw std::runtime_error("Test environment changed");
        value.resize(copied);
        return value;
    };
    std::wstring path = environment(L"SWML_TEST_TEMP_ROOT");
    if (path.empty()) {
        path = environment(L"LOCALAPPDATA");
        if (path.empty()) throw std::runtime_error("Set SWML_TEST_TEMP_ROOT or LOCALAPPDATA");
        path += L"\\Temp\\opencode";
    }
    if (!std::filesystem::path(path).is_absolute())
        throw std::runtime_error("Test temp root must be absolute");
    std::filesystem::create_directories(std::filesystem::path(path));
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::runtime_error("Invalid test temp path");
    std::string utf8(bytes, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1,
                             utf8.data(), bytes, nullptr, nullptr))
        throw std::runtime_error("Cannot encode test temp path");
    utf8.pop_back();
    return utf8;
}
