#include "custom_display_registry.h"
#include "log.h"
#include <windows.h>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>

namespace swml {
namespace {

std::mutex g_mutex;
std::map<std::string, std::pair<int, int>> g_resolutions;
bool g_initialized = false;

int g_lastW = 1;
int g_lastH = 1;
int g_lastPx = 32;
int g_lastPy = 32;

std::wstring GetConfigPath() {
    wchar_t appData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) {
        return L"";
    }
    std::wstring path = appData;
    path += L"\\Stormworks\\data\\mods\\swml_custom_display\\resolutions.txt";
    return path;
}

void SaveToDiskLocked() {
    std::wstring path = GetConfigPath();
    if (path.empty()) return;

    std::ofstream file(path);
    if (!file.is_open()) return;

    file << "# StormworksModLoader Custom Display Resolutions\n";
    file << "# LastDialog=" << g_lastW << "," << g_lastH << "," << g_lastPx << "," << g_lastPy << "\n";
    for (const auto& entry : g_resolutions) {
        file << entry.first << "=" << entry.second.first << "," << entry.second.second << "\n";
    }
}

void LoadFromDiskLocked() {
    std::wstring path = GetConfigPath();
    if (path.empty()) return;

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            if (line.rfind("# LastDialog=", 0) == 0) {
                int w = 1, h = 1, px = 32, py = 32;
                if (sscanf_s(line.c_str() + 13, "%d,%d,%d,%d", &w, &h, &px, &py) == 4) {
                    g_lastW = w; g_lastH = h; g_lastPx = px; g_lastPy = py;
                }
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string defId = line.substr(0, eq);
        std::string rest = line.substr(eq + 1);

        size_t comma = rest.find(',');
        if (comma == std::string::npos) continue;

        try {
            int px = std::stoi(rest.substr(0, comma));
            int py = std::stoi(rest.substr(comma + 1));
            if (px >= 1 && px <= 8192 && py >= 1 && py <= 8192) {
                g_resolutions[defId] = {px, py};
            }
        } catch (...) {}
    }
}

} // namespace

void InitializeCustomDisplayRegistry() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;
    g_initialized = true;
    LoadFromDiskLocked();
    LogInfo("CDREG: Initialized custom display registry (%zu entries loaded)", g_resolutions.size());
}

constexpr int kNumDisplaySlots = 4;

static std::string MakeSlotId(int w, int h, int slot) {
    if (slot <= 1) {
        return "swml_cd_" + std::to_string(w) + "x" + std::to_string(h);
    }
    return "swml_cd_" + std::to_string(w) + "x" + std::to_string(h) + "_s" + std::to_string(slot);
}

void SetCustomDisplayResolution(const std::string& defId, int px, int py) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_resolutions[defId] = {px, py};
    SaveToDiskLocked();
    LogInfo("CDREG: Registered resolution %s -> %dx%d", defId.c_str(), px, py);
}

std::string GetOrAllocateDisplayDefinitionId(int w, int h, int px, int py) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_initialized = true;
        LoadFromDiskLocked();
    }

    // 1. Check if any slot for (w, h) already matches (px, py)
    for (int slot = 1; slot <= kNumDisplaySlots; ++slot) {
        std::string id = MakeSlotId(w, h, slot);
        auto it = g_resolutions.find(id);
        if (it != g_resolutions.end() && it->second.first == px && it->second.second == py) {
            LogInfo("CDREG: Using existing matching slot %s for %dx%d (%d,%d)",
                    id.c_str(), w, h, px, py);
            return id;
        }
    }

    // 2. Check if there is an unassigned slot
    for (int slot = 1; slot <= kNumDisplaySlots; ++slot) {
        std::string id = MakeSlotId(w, h, slot);
        if (g_resolutions.find(id) == g_resolutions.end()) {
            g_resolutions[id] = {px, py};
            SaveToDiskLocked();
            LogInfo("CDREG: Assigned new slot %s for %dx%d -> %dx%d",
                    id.c_str(), w, h, px, py);
            return id;
        }
    }

    // 3. All slots are assigned to different resolutions.
    // Cycle among slots 2..kNumDisplaySlots (keep slot 1 as default/primary)
    static std::map<std::pair<int, int>, int> s_cycle;
    int& current = s_cycle[{w, h}];
    if (current < 2 || current > kNumDisplaySlots) current = 2;
    std::string id = MakeSlotId(w, h, current);
    current = (current >= kNumDisplaySlots) ? 2 : current + 1;

    g_resolutions[id] = {px, py};
    SaveToDiskLocked();
    LogInfo("CDREG: Recycled slot %s for %dx%d -> %dx%d", id.c_str(), w, h, px, py);
    return id;
}

bool GetCustomDisplayResolution(const std::string& defId, int& px, int& py) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_initialized = true;
        LoadFromDiskLocked();
    }

    auto it = g_resolutions.find(defId);
    if (it != g_resolutions.end()) {
        px = it->second.first;
        py = it->second.second;
        return true;
    }
    return false;
}

void SetLastDialogValues(int w, int h, int px, int py) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lastW = w;
    g_lastH = h;
    g_lastPx = px;
    g_lastPy = py;
    SaveToDiskLocked();
}

void GetLastDialogValues(int& w, int& h, int& px, int& py) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_initialized = true;
        LoadFromDiskLocked();
    }
    w = g_lastW;
    h = g_lastH;
    px = g_lastPx;
    py = g_lastPy;
}

std::vector<std::pair<std::string, std::pair<int, int>>> GetAllRegisteredDisplays() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_initialized = true;
        LoadFromDiskLocked();
    }
    std::vector<std::pair<std::string, std::pair<int, int>>> result;
    for (const auto& entry : g_resolutions) {
        result.push_back(entry);
    }
    return result;
}

} // namespace swml
