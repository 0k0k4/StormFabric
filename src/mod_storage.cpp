#include "mod_storage.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace swml {
namespace {
constexpr size_t kMaxPath = 240; // UTF-16 units, including temporary filename; below MAX_PATH
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(h.value) { h.value = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& h) noexcept {
        if (this != &h) {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = h.value; h.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};
struct OwnerStorage {
    std::wstring root;
    // Pin every ancestor, not just modDir. No delete sharing prevents renames.
    // Child operations are handle-relative: changing a reparse attribute cannot
    // redirect traversal through a previously opened directory object.
    std::vector<Handle> pins;
};
std::mutex g_mutex;
std::map<const SwModHost*, OwnerStorage> g_owners;
std::string g_world;
unsigned long long g_tempSequence = 0;

bool SafeKey(const char* key, std::string& out) {
    if (!key) return false;
    out.clear();
    for (size_t i = 0; i <= SW_STORAGE_MAX_KEY_BYTES; ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (!c) break;
        if (i == SW_STORAGE_MAX_KEY_BYTES || !((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_')) return false;
        out.push_back(static_cast<char>(c));
    }
    if (out.empty() || out.back() == '.' || out.find("..") != std::string::npos) return false;
    std::string base = out.substr(0, out.find('.'));
    for (char& c : base) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        base == "CONIN$" || base == "CONOUT$") return false;
    if (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
        base[3] >= '0' && base[3] <= '9') return false;
    return true;
}
SwStorageStatus LastStatus() {
    const DWORD e = GetLastError();
    if (e == ERROR_REPARSE_POINT_ENCOUNTERED || e == ERROR_CANT_ACCESS_FILE || e == ERROR_INVALID_REPARSE_DATA)
        return SW_STORAGE_UNSAFE_PATH;
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ?
        SW_STORAGE_NOT_FOUND : SW_STORAGE_IO_ERROR;
}
using NtCreate = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                  PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInfo = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
using NtToWin32 = ULONG (WINAPI*)(NTSTATUS);
Handle OpenRelative(HANDLE parent, const std::wstring& leaf, ACCESS_MASK access, ULONG share,
                    ULONG disposition, bool directory) {
    static auto create = reinterpret_cast<NtCreate>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    static auto error = reinterpret_cast<NtToWin32>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    if (!create || !error) { SetLastError(ERROR_NOT_SUPPORTED); return Handle(); }
    UNICODE_STRING name = {};
    name.Buffer = const_cast<wchar_t*>(leaf.data());
    name.Length = name.MaximumLength = static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr); attr.RootDirectory = parent; attr.ObjectName = &name;
    attr.Attributes = 0x40 /* OBJ_CASE_INSENSITIVE */ | 0x1000 /* OBJ_DONT_REPARSE */;
    IO_STATUS_BLOCK io = {};
    HANDLE h = INVALID_HANDLE_VALUE;
    // FILE_OPEN_REPARSE_POINT + synchronous I/O. Leaf only, never a caller path.
    const ULONG options = 0x00200000 | 0x20 | (directory ? 1 : 0);
    const NTSTATUS result = create(&h, access | SYNCHRONIZE, &attr, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                   share, disposition, options, nullptr, 0);
    if (result < 0) { SetLastError(error(result)); return Handle(); }
    return Handle(h);
}
SwStorageStatus PinDirectory(const std::wstring& path, std::vector<Handle>& pins, bool create) {
    if (path.size() >= kMaxPath) return SW_STORAGE_UNSAFE_PATH;
    // Only the drive root is opened by absolute path; all remaining components
    // are a single leaf relative to the previous checked directory handle.
    Handle h = pins.empty() ?
        Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) :
        OpenRelative(pins.back().value, path.substr(path.find_last_of(L'\\') + 1), GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, create ? 3 /* FILE_OPEN_IF */ : 1 /* FILE_OPEN */, true);
    if (h.value == INVALID_HANDLE_VALUE) return LastStatus();
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(h.value, &info)) return SW_STORAGE_IO_ERROR;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return SW_STORAGE_UNSAFE_PATH;
    pins.push_back(std::move(h));
    return SW_STORAGE_OK;
}
SwStorageStatus SnapshotRoot(const char* utf8, OwnerStorage& storage) {
    if (!utf8) return SW_STORAGE_INVALID_ARGUMENT;
    size_t n = 0;
    while (n < 4 * kMaxPath && utf8[n]) ++n;
    if (!n || n == 4 * kMaxPath) return SW_STORAGE_UNSAFE_PATH;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8,
                                        static_cast<int>(n), nullptr, 0);
    if (!count || count >= static_cast<int>(kMaxPath)) return SW_STORAGE_UNSAFE_PATH;
    std::wstring path(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, static_cast<int>(n), &path[0], count))
        return SW_STORAGE_UNSAFE_PATH;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    // Intentionally reject UNC/device/relative roots, ADS and ambiguous Win32 spellings.
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' || path[2] != L'\\')
        return SW_STORAGE_UNSAFE_PATH;
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    if (path.size() == 3) return SW_STORAGE_UNSAFE_PATH;
    SwStorageStatus status = PinDirectory(path.substr(0, 3), storage.pins, false);
    if (status != SW_STORAGE_OK) return status;
    for (size_t start = 3; start < path.size();) {
        size_t end = path.find(L'\\', start);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring part = path.substr(start, end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
            part.find_first_of(L":*?\"<>|") != std::wstring::npos) return SW_STORAGE_UNSAFE_PATH;
        for (wchar_t c : part) if (c < 32) return SW_STORAGE_UNSAFE_PATH;
        status = PinDirectory(path.substr(0, end), storage.pins, false);
        if (status != SW_STORAGE_OK) return status;
        start = end + 1;
    }
    wchar_t canonical[MAX_PATH + 4] = {};
    const DWORD length = GetFinalPathNameByHandleW(storage.pins.back().value, canonical,
                                                  MAX_PATH + 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= MAX_PATH + 4) return SW_STORAGE_UNSAFE_PATH;
    storage.root = canonical;
    if (storage.root.compare(0, 4, L"\\\\?\\") != 0) return SW_STORAGE_UNSAFE_PATH;
    storage.root.erase(0, 4);
    if (storage.root.size() >= kMaxPath) return SW_STORAGE_UNSAFE_PATH;
    return SW_STORAGE_OK;
}
SwStorageStatus MakePath(const SwModHost* owner, SwStorageScope scope, const char* key,
                         std::wstring& path, std::vector<Handle>& pins, bool create) {
    if (!owner || !IsEngineApiOwner(owner)) return SW_STORAGE_INVALID_OWNER;
    auto it = g_owners.find(owner);
    if (it == g_owners.end()) return SW_STORAGE_INVALID_OWNER;
    for (const auto& pin : it->second.pins) {
        BY_HANDLE_FILE_INFORMATION info = {};
        if (!GetFileInformationByHandle(pin.value, &info)) return SW_STORAGE_IO_ERROR;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return SW_STORAGE_UNSAFE_PATH;
    }
    std::string safe;
    if (!SafeKey(key, safe)) return SW_STORAGE_INVALID_KEY;
    if (scope != SW_STORAGE_CONFIG && scope != SW_STORAGE_MOD_DATA && scope != SW_STORAGE_WORLD_DATA)
        return SW_STORAGE_INVALID_ARGUMENT;
    if (scope == SW_STORAGE_WORLD_DATA && g_world.empty()) return SW_STORAGE_UNAVAILABLE;
    std::wstring dir = it->second.root + (scope == SW_STORAGE_CONFIG ? L"\\config" : L"\\data");
    std::vector<std::wstring> dirs = {dir};
    if (scope == SW_STORAGE_WORLD_DATA) {
        dir += L"\\world"; dirs.push_back(dir);
        dir += L"\\" + std::wstring(g_world.begin(), g_world.end()); dirs.push_back(dir);
    }
    path = dir + L"\\" + std::wstring(safe.begin(), safe.end()) + L".bin";
    // Reserve room for the independently generated atomic-write temporary name.
    if (path.size() >= kMaxPath || dir.size() + 55 >= kMaxPath) return SW_STORAGE_UNSAFE_PATH;
    // The first child must use the registered modDir, not an absolute path.
    // A temporary duplicate supplies the starting parent without transferring ownership.
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), it->second.pins.back().value,
                         GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return SW_STORAGE_IO_ERROR;
    pins.emplace_back(duplicate);
    for (const auto& d : dirs) {
        const auto status = PinDirectory(d, pins, create);
        if (status != SW_STORAGE_OK) return status;
    }
    return SW_STORAGE_OK;
}
SwStorageStatus CheckFile(HANDLE h, BY_HANDLE_FILE_INFORMATION& info) {
    if (!GetFileInformationByHandle(h, &info)) return SW_STORAGE_IO_ERROR;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
        info.nNumberOfLinks != 1 || GetFileType(h) != FILE_TYPE_DISK) return SW_STORAGE_UNSAFE_PATH;
    return SW_STORAGE_OK;
}
SwStorageStatus Read(const SwModHost* owner, SwStorageScope scope, const char* key,
                     uint8_t* buffer, size_t capacity, size_t* outSize) {
    if (outSize) *outSize = 0;
    if (!outSize || (!buffer && capacity)) return SW_STORAGE_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::wstring path;
        std::vector<Handle> pins;
        auto status = MakePath(owner, scope, key, path, pins, false);
        if (status != SW_STORAGE_OK) return status;
        // No write/delete sharing gives a stable file snapshot, including its size.
        Handle h = OpenRelative(pins.back().value, path.substr(path.find_last_of(L'\\') + 1),
                                GENERIC_READ, FILE_SHARE_READ, 1, false);
        if (h.value == INVALID_HANDLE_VALUE) return LastStatus();
        BY_HANDLE_FILE_INFORMATION info = {};
        status = CheckFile(h.value, info);
        if (status != SW_STORAGE_OK) return status;
        if (info.nFileSizeHigh || info.nFileSizeLow > SW_STORAGE_MAX_BYTES) return SW_STORAGE_TOO_LARGE;
        const size_t size = info.nFileSizeLow;
        if (capacity < size) { *outSize = size; return SW_STORAGE_BUFFER_TOO_SMALL; }
        // Read into a private snapshot: I/O failures must not modify caller bytes.
        std::vector<uint8_t> snapshot(size);
        DWORD done = 0;
        if (size && (!ReadFile(h.value, snapshot.data(), static_cast<DWORD>(size), &done, nullptr) || done != size))
            return SW_STORAGE_IO_ERROR;
        if (size) memcpy(buffer, snapshot.data(), size);
        *outSize = size;
        return SW_STORAGE_OK;
    } catch (...) { return SW_STORAGE_IO_ERROR; }
}
struct TempFile {
    Handle handle;
    bool remove = false;
    ~TempFile() {
        if (remove && handle.value != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO info = {TRUE};
            SetFileInformationByHandle(handle.value, FileDispositionInfo, &info, sizeof(info));
        }
    }
};
SwStorageStatus Write(const SwModHost* owner, SwStorageScope scope, const char* key,
                      const uint8_t* bytes, size_t size) {
    if (!bytes && size) return SW_STORAGE_INVALID_ARGUMENT;
    if (size > SW_STORAGE_MAX_BYTES) return SW_STORAGE_TOO_LARGE;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::wstring path;
        std::vector<Handle> pins;
        auto status = MakePath(owner, scope, key, path, pins, true);
        if (status != SW_STORAGE_OK) return status;
        {
            Handle target = OpenRelative(pins.back().value, path.substr(path.find_last_of(L'\\') + 1),
                                         FILE_READ_ATTRIBUTES, FILE_SHARE_READ, 1, false);
            if (target.value == INVALID_HANDLE_VALUE) {
                status = LastStatus();
                if (status != SW_STORAGE_NOT_FOUND) return status;
            } else {
                BY_HANDLE_FILE_INFORMATION info = {};
                status = CheckFile(target.value, info);
                if (status != SW_STORAGE_OK) return status;
            }
        }
        TempFile temp;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const std::wstring leaf = L".swml-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                      std::to_wstring(++g_tempSequence) + L".tmp";
            temp.handle = OpenRelative(pins.back().value, leaf, GENERIC_WRITE | DELETE, 0, 2 /* FILE_CREATE */, false);
            if (temp.handle.value != INVALID_HANDLE_VALUE) { temp.remove = true; break; }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return LastStatus();
        }
        if (temp.handle.value == INVALID_HANDLE_VALUE) return SW_STORAGE_IO_ERROR;
        DWORD done = 0;
        if (size && (!WriteFile(temp.handle.value, bytes, static_cast<DWORD>(size), &done, nullptr) || done != size))
            return SW_STORAGE_IO_ERROR;
        if (!FlushFileBuffers(temp.handle.value)) return SW_STORAGE_IO_ERROR;
        // Relative rename commits to the checked directory object, not a path that
        // could have acquired a reparse point. The source remains exclusively open.
        const std::wstring leaf = path.substr(path.find_last_of(L'\\') + 1);
        std::vector<uint8_t> renameBytes(offsetof(FILE_RENAME_INFO, FileName) + leaf.size() * sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameBytes.data());
        rename->ReplaceIfExists = TRUE;
        rename->RootDirectory = pins.back().value;
        rename->FileNameLength = static_cast<DWORD>(leaf.size() * sizeof(wchar_t));
        memcpy(rename->FileName, leaf.data(), rename->FileNameLength);
        static auto ntSetInfo = reinterpret_cast<NtSetInfo>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
        IO_STATUS_BLOCK io = {};
        const NTSTATUS result = ntSetInfo ? ntSetInfo(temp.handle.value, &io, rename, static_cast<ULONG>(renameBytes.size()),
                                                      static_cast<FILE_INFORMATION_CLASS>(10)) : -1;
        if (result < 0) return SW_STORAGE_IO_ERROR;
        temp.remove = false;
        return SW_STORAGE_OK;
    } catch (...) { return SW_STORAGE_IO_ERROR; }
}
SwStorageStatus ReadConfig(const SwModHost* owner, const char* key, uint8_t* out, size_t cap, size_t* size) {
    return Read(owner, SW_STORAGE_CONFIG, key, out, cap, size);
}
SwStorageStatus WriteConfig(const SwModHost* owner, const char* key, const uint8_t* bytes, size_t size) {
    return Write(owner, SW_STORAGE_CONFIG, key, bytes, size);
}
const SwStorageApi g_api = {sizeof(SwStorageApi), &ReadConfig, &WriteConfig, &Read, &Write};
}

const SwStorageApi* GetStorageApi() { return &g_api; }
SwStorageStatus RegisterModStorageOwner(const SwModHost* owner) {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!owner || !IsEngineApiOwner(owner)) return SW_STORAGE_INVALID_OWNER;
        if (g_owners.count(owner)) return SW_STORAGE_OK; // do not accept mutated modDir
        OwnerStorage storage;
        auto status = SnapshotRoot(owner->modDir, storage);
        if (status != SW_STORAGE_OK) return status;
        g_owners.emplace(owner, std::move(storage));
        return SW_STORAGE_OK;
    } catch (...) { return SW_STORAGE_IO_ERROR; }
}
void ReleaseModStorageOwner(const SwModHost* owner) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_owners.erase(owner);
}
SwStorageStatus SetStorageWorld(const std::string& worldId) {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_world.clear();
        if (worldId.empty()) return SW_STORAGE_OK;
        std::string safe;
        if (worldId.find('\0') != std::string::npos || !SafeKey(worldId.c_str(), safe))
            return SW_STORAGE_INVALID_KEY;
        g_world = std::move(safe);
        return SW_STORAGE_OK;
    } catch (...) { return SW_STORAGE_IO_ERROR; }
}
}
