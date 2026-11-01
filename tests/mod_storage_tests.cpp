// Standalone Windows filesystem tests; no game, hooks, installation, or engine linkage.
#include "../src/mod_storage.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "test_temp.h"

namespace {
std::mutex ownerMutex;
std::set<const SwModHost*> owners;
void Check(bool ok, int line) {
    if (!ok) { std::cerr << "FAILED at line " << line << '\n'; std::exit(1); }
}
#define CHECK(x) Check(!!(x), __LINE__)
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    CHECK(n > 0);
    std::wstring w(n, L'\0');
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, &w[0], n) == n);
    w.pop_back(); return w;
}
void RawWrite(const std::string& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(Wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD done = 0;
    CHECK(WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr));
    CHECK(done == bytes.size()); CHECK(CloseHandle(h));
}
void AddOwner(const SwModHost* owner) { std::lock_guard<std::mutex> lock(ownerMutex); owners.insert(owner); }
void DropOwner(const SwModHost* owner) { std::lock_guard<std::mutex> lock(ownerMutex); owners.erase(owner); }

// Directory junctions require no symlink privilege. Construct a real mount-point
// reparse record so the safety tests run even without Developer Mode/admin rights.
void Junction(const std::string& link, const std::string& target, bool create = true) {
    if (create) CHECK(CreateDirectoryW(Wide(link).c_str(), nullptr));
    HANDLE h = CreateFileW(Wide(link).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    const std::wstring substitute = L"\\??\\" + Wide(target);
    const std::wstring print = Wide(target);
    struct MountHeader {
        DWORD tag;
        WORD length, reserved;
        WORD substituteOffset, substituteLength, printOffset, printLength;
    };
    static_assert(sizeof(MountHeader) == 16, "reparse mount header");
    const size_t namesBytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<uint8_t> data(sizeof(MountHeader) + namesBytes);
    auto* header = reinterpret_cast<MountHeader*>(data.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->length = static_cast<WORD>(8 + namesBytes);
    header->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    header->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    header->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    memcpy(data.data() + sizeof(MountHeader), substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    memcpy(data.data() + sizeof(MountHeader) + header->printOffset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    DWORD done = 0;
    CHECK(DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, data.data(), static_cast<DWORD>(data.size()), nullptr, 0, &done, nullptr));
    CHECK(CloseHandle(h));
}
}
namespace swml {
bool IsEngineApiOwner(const SwModHost* owner) {
    std::lock_guard<std::mutex> lock(ownerMutex);
    return owner && owners.count(owner) != 0;
}
}

int main() {
    const std::string base = TestTempRoot() + "\\p0-storage-tests";
    CHECK(CreateDirectoryW(Wide(base).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
    const std::string root = base + "\\run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    CHECK(CreateDirectoryW(Wide(root).c_str(), nullptr));
    const std::string modA = root + "\\mod-a", modB = root + "\\mod-b", outside = root + "\\outside";
    CHECK(CreateDirectoryW(Wide(modA).c_str(), nullptr));
    CHECK(CreateDirectoryW(Wide(modB).c_str(), nullptr));
    CHECK(CreateDirectoryW(Wide(outside).c_str(), nullptr));
    SwModHost a = {}, b = {};
    a.modDir = modA.c_str(); b.modDir = modB.c_str();
    const auto* api = swml::GetStorageApi();
    CHECK(api && api->structSize == sizeof(SwStorageApi));
    size_t size = 999;
    const uint8_t binary[] = {0, 1, 0xff, 0, 8};
    uint8_t out[8]; memset(out, 0xcc, sizeof(out));
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_INVALID_OWNER);
    CHECK(api->readConfig(nullptr, "key", out, sizeof(out), &size) == SW_STORAGE_INVALID_OWNER && size == 0);
    CHECK(api->writeConfig(&a, "key", binary, sizeof(binary)) == SW_STORAGE_INVALID_OWNER);
    AddOwner(&a); AddOwner(&b);
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_OK);
    CHECK(swml::RegisterModStorageOwner(&b) == SW_STORAGE_OK);
    CHECK(!MoveFileExW(Wide(modA).c_str(), Wide(root + "\\renamed").c_str(), 0)); // pinned root
    CHECK(api->readConfig(&a, "missing", out, sizeof(out), &size) == SW_STORAGE_NOT_FOUND && size == 0);
    const auto initialWrite = api->writeConfig(&a, "settings-1.v2", binary, sizeof(binary));
    if (initialWrite != SW_STORAGE_OK) std::cerr << "initial write status=" << initialWrite << " Win32=" << GetLastError() << '\n';
    CHECK(initialWrite == SW_STORAGE_OK);
    CHECK(api->readConfig(&a, "settings-1.v2", nullptr, 0, &size) == SW_STORAGE_BUFFER_TOO_SMALL && size == sizeof(binary));
    CHECK(api->readConfig(&a, "settings-1.v2", out, 2, &size) == SW_STORAGE_BUFFER_TOO_SMALL && size == sizeof(binary));
    for (uint8_t byte : out) CHECK(byte == 0xcc);
    CHECK(api->readConfig(&a, "settings-1.v2", out, sizeof(out), &size) == SW_STORAGE_OK);
    CHECK(size == sizeof(binary) && memcmp(out, binary, size) == 0 && out[size] == 0xcc);
    CHECK(api->readConfig(&b, "settings-1.v2", out, sizeof(out), &size) == SW_STORAGE_NOT_FOUND);
    CHECK(api->writeConfig(&a, "empty", nullptr, 0) == SW_STORAGE_OK);
    CHECK(api->readConfig(&a, "empty", nullptr, 0, &size) == SW_STORAGE_OK && size == 0);
    CHECK(api->writeData(&a, SW_STORAGE_MOD_DATA, "binary", binary, sizeof(binary)) == SW_STORAGE_OK);
    CHECK(api->readConfig(&a, "binary", out, sizeof(out), &size) == SW_STORAGE_NOT_FOUND);
    CHECK(api->readData(&a, SW_STORAGE_MOD_DATA, "binary", out, sizeof(out), &size) == SW_STORAGE_OK);
    CHECK(api->readData(&a, SW_STORAGE_CONFIG, "empty", nullptr, 0, &size) == SW_STORAGE_OK);

    const char* bad[] = {"", ".", "..", "a..b", "a.", "a ", "../escape", "a/b", "a\\b", "C:\\x", "a:b",
                         "CON", "con.txt", "PRN", "aux.bin", "NUL", "COM1", "lpt9.txt", "a\xc3\xa9"};
    for (const char* key : bad) {
        CHECK(api->writeConfig(&a, key, binary, sizeof(binary)) == SW_STORAGE_INVALID_KEY);
        CHECK(api->readConfig(&a, key, out, sizeof(out), &size) == SW_STORAGE_INVALID_KEY && size == 0);
    }
    CHECK(api->writeConfig(&a, nullptr, nullptr, 0) == SW_STORAGE_INVALID_KEY);
    const std::string longKey(65, 'a'), maxKey(64, 'a');
    CHECK(api->writeConfig(&a, longKey.c_str(), nullptr, 0) == SW_STORAGE_INVALID_KEY);
    CHECK(api->writeConfig(&a, maxKey.c_str(), nullptr, 0) == SW_STORAGE_OK);
    CHECK(api->writeConfig(&a, "null", nullptr, 1) == SW_STORAGE_INVALID_ARGUMENT);
    CHECK(api->writeConfig(&a, "huge", binary, SW_STORAGE_MAX_BYTES + size_t(1)) == SW_STORAGE_TOO_LARGE);
    CHECK(api->readConfig(&a, "empty", nullptr, 1, &size) == SW_STORAGE_INVALID_ARGUMENT && size == 0);
    CHECK(api->readConfig(&a, "empty", out, sizeof(out), nullptr) == SW_STORAGE_INVALID_ARGUMENT);
    CHECK(api->writeData(&a, static_cast<SwStorageScope>(42), "x", nullptr, 0) == SW_STORAGE_INVALID_ARGUMENT);
    CHECK(api->readData(&a, static_cast<SwStorageScope>(42), "x", out, sizeof(out), &size) == SW_STORAGE_INVALID_ARGUMENT);

    CHECK(swml::SetStorageWorld("") == SW_STORAGE_OK);
    CHECK(api->writeData(&a, SW_STORAGE_WORLD_DATA, "x", binary, sizeof(binary)) == SW_STORAGE_UNAVAILABLE);
    CHECK(api->readData(&a, SW_STORAGE_WORLD_DATA, "x", out, sizeof(out), &size) == SW_STORAGE_UNAVAILABLE && size == 0);
    CHECK(swml::SetStorageWorld("actual-world-A") == SW_STORAGE_OK);
    CHECK(api->writeData(&a, SW_STORAGE_WORLD_DATA, "x", binary, sizeof(binary)) == SW_STORAGE_OK);
    CHECK(swml::SetStorageWorld("actual-world-B") == SW_STORAGE_OK);
    CHECK(api->readData(&a, SW_STORAGE_WORLD_DATA, "x", out, sizeof(out), &size) == SW_STORAGE_NOT_FOUND);
    CHECK(swml::SetStorageWorld("actual-world-A") == SW_STORAGE_OK);
    CHECK(api->readData(&a, SW_STORAGE_WORLD_DATA, "x", out, sizeof(out), &size) == SW_STORAGE_OK);
    CHECK(swml::SetStorageWorld("../fake") == SW_STORAGE_INVALID_KEY);
    CHECK(api->readData(&a, SW_STORAGE_WORLD_DATA, "x", out, sizeof(out), &size) == SW_STORAGE_UNAVAILABLE);
    CHECK(swml::SetStorageWorld(std::string("x\0y", 3)) == SW_STORAGE_INVALID_KEY);

    // Snapshot root is authoritative even if plugin strings/host fields are mutated.
    a.modDir = outside.c_str();
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_OK);
    CHECK(api->writeConfig(&a, "snapshot", binary, sizeof(binary)) == SW_STORAGE_OK);
    CHECK(std::filesystem::exists(modA + "\\config\\snapshot.bin"));
    CHECK(!std::filesystem::exists(outside + "\\config"));
    a.modDir = modA.c_str();

    // Real oversized disk file, not an API mock; exact 4 MiB limit round trips.
    std::vector<uint8_t> large(SW_STORAGE_MAX_BYTES, 0x5a), largeOut(large.size());
    CHECK(api->writeData(&a, SW_STORAGE_MOD_DATA, "limit", large.data(), large.size()) == SW_STORAGE_OK);
    CHECK(api->readData(&a, SW_STORAGE_MOD_DATA, "limit", largeOut.data(), largeOut.size(), &size) == SW_STORAGE_OK);
    CHECK(size == large.size() && largeOut == large);
    large.push_back(0);
    RawWrite(modA + "\\config\\oversize.bin", large);
    CHECK(api->readConfig(&a, "oversize", out, sizeof(out), &size) == SW_STORAGE_TOO_LARGE && size == 0);

    // An outside hardlink cannot expose/overwrite another file through storage.
    RawWrite(outside + "\\sentinel.bin", std::vector<uint8_t>{7, 9});
    CHECK(CreateHardLinkW(Wide(modA + "\\config\\linked.bin").c_str(), Wide(outside + "\\sentinel.bin").c_str(), nullptr));
    CHECK(api->readConfig(&a, "linked", out, sizeof(out), &size) == SW_STORAGE_UNSAFE_PATH);
    CHECK(api->writeConfig(&a, "linked", binary, sizeof(binary)) == SW_STORAGE_UNSAFE_PATH);

    // A blocked rename cannot truncate or partially overwrite the old value.
    HANDLE held = CreateFileW(Wide(modA + "\\config\\settings-1.v2.bin").c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(api->writeConfig(&a, "settings-1.v2", binary, 1) == SW_STORAGE_IO_ERROR);
    CHECK(CloseHandle(held));
    CHECK(api->readConfig(&a, "settings-1.v2", out, sizeof(out), &size) == SW_STORAGE_OK && size == sizeof(binary));
    for (const auto& entry : std::filesystem::directory_iterator(modA + "\\config"))
        CHECK(entry.path().extension() != ".tmp");
    held = CreateFileW(Wide(modA + "\\config\\settings-1.v2.bin").c_str(), GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    memset(out, 0xcc, sizeof(out));
    CHECK(api->readConfig(&a, "settings-1.v2", out, sizeof(out), &size) == SW_STORAGE_IO_ERROR && size == 0);
    for (uint8_t byte : out) CHECK(byte == 0xcc);
    CHECK(CloseHandle(held));

    // Serialized concurrent calls see only complete old/new snapshots.
    const std::vector<uint8_t> first(4096, 0x11), second(2048, 0x22);
    CHECK(api->writeConfig(&a, "atomic", first.data(), first.size()) == SW_STORAGE_OK);
    std::atomic<bool> failed(false);
    std::thread writer([&] {
        for (int i = 0; i < 80; ++i) {
            const auto& data = i % 2 ? first : second;
            if (api->writeConfig(&a, "atomic", data.data(), data.size()) != SW_STORAGE_OK) failed = true;
        }
    });
    std::thread reader([&] {
        std::vector<uint8_t> snapshot(4096);
        for (int i = 0; i < 80; ++i) {
            size_t n = 0;
            if (api->readConfig(&a, "atomic", snapshot.data(), snapshot.size(), &n) != SW_STORAGE_OK ||
                (n != first.size() && n != second.size())) { failed = true; continue; }
            const uint8_t expected = n == first.size() ? 0x11 : 0x22;
            for (size_t j = 0; j < n; ++j) if (snapshot[j] != expected) failed = true;
        }
    });
    writer.join(); reader.join(); CHECK(!failed);

    // Engine-invalid owners are rejected even if the storage map still has them.
    DropOwner(&a);
    CHECK(api->readConfig(&a, "empty", out, sizeof(out), &size) == SW_STORAGE_INVALID_OWNER);
    AddOwner(&a);
    swml::ReleaseModStorageOwner(&a);
    CHECK(api->writeConfig(&a, "empty", nullptr, 0) == SW_STORAGE_INVALID_OWNER);
    CHECK(std::filesystem::exists(modA + "\\config\\empty.bin"));
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_OK);
    CHECK(api->readConfig(&a, "settings-1.v2", out, sizeof(out), &size) == SW_STORAGE_OK);
    swml::ReleaseModStorageOwner(&a); DropOwner(&a);
    const auto* stale = reinterpret_cast<const SwModHost*>(static_cast<uintptr_t>(1));
    CHECK(api->readConfig(stale, "empty", out, sizeof(out), &size) == SW_STORAGE_INVALID_OWNER);
    CHECK(swml::RegisterModStorageOwner(stale) == SW_STORAGE_INVALID_OWNER);

    // Reject existing junction roots and a junction in a storage subdirectory.
    Junction(root + "\\linked-root", outside);
    const std::string linkedRoot = root + "\\linked-root";
    a.modDir = linkedRoot.c_str(); AddOwner(&a);
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_UNSAFE_PATH);
    CHECK(RemoveDirectoryW(Wide(linkedRoot).c_str()));
    Junction(modB + "\\config", outside);
    CHECK(api->writeConfig(&b, "escape", binary, sizeof(binary)) == SW_STORAGE_UNSAFE_PATH);
    CHECK(api->readConfig(&b, "sentinel", out, sizeof(out), &size) == SW_STORAGE_UNSAFE_PATH);
    CHECK(!std::filesystem::exists(outside + "\\escape.bin"));
    CHECK(RemoveDirectoryW(Wide(modB + "\\config").c_str()));

    // A registered, initially empty root can acquire a junction later. Detect
    // that change, and never create config/data in the junction destination.
    const std::string changedRoot = root + "\\changed-root";
    CHECK(CreateDirectoryW(Wide(changedRoot).c_str(), nullptr));
    a.modDir = changedRoot.c_str();
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_OK);
    Junction(changedRoot, outside, false);
    CHECK(api->writeConfig(&a, "after-change", binary, sizeof(binary)) == SW_STORAGE_UNSAFE_PATH);
    CHECK(!std::filesystem::exists(outside + "\\config"));
    swml::ReleaseModStorageOwner(&a);
    CHECK(RemoveDirectoryW(Wide(changedRoot).c_str()));
    const std::string boundedRoot = root + "\\long-root-" + std::string(100, 'r');
    CHECK(CreateDirectoryW(Wide(boundedRoot).c_str(), nullptr));
    a.modDir = boundedRoot.c_str();
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_OK);
    CHECK(api->writeConfig(&a, maxKey.c_str(), binary, sizeof(binary)) == SW_STORAGE_UNSAFE_PATH);
    swml::ReleaseModStorageOwner(&a);

    for (const std::string& invalid : std::vector<std::string>{"relative", modA + "\\..\\outside", modA + ":ads", modA + "."}) {
        a.modDir = invalid.c_str();
        CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_UNSAFE_PATH);
    }
    std::string invalidUtf8 = modA + "\\\xff";
    a.modDir = invalidUtf8.c_str();
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_UNSAFE_PATH);
    const std::string hugeRoot = modA + "\\" + std::string(240, 'x');
    a.modDir = hugeRoot.c_str();
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_UNSAFE_PATH);
    a.modDir = nullptr;
    CHECK(swml::RegisterModStorageOwner(&a) == SW_STORAGE_INVALID_ARGUMENT);
    swml::ReleaseModStorageOwner(&a); DropOwner(&a);
    swml::ReleaseModStorageOwner(&b); DropOwner(&b);
    CHECK(swml::SetStorageWorld("") == SW_STORAGE_OK);
    // Only this process-created, unique test tree; all junctions removed above.
    CHECK(std::filesystem::remove_all(root) > 0);
    std::cout << "mod_storage_tests: passed\n";
}
