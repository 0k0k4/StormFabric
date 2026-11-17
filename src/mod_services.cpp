// Services between mods: a name -> function table registry. A consumer may only get a table
// whose provider is one of its declared (transitive) dependencies, so the loader's load and
// unload order keeps the table alive for as long as the consumer can use it.
#include "mod_services.h"
#include "mod_runtime.h"
#include "log.h"
#include <windows.h>
#include <map>
#include <mutex>
#include <vector>

namespace swml {
namespace {
struct Service { const SwModHost* owner = nullptr; uint32_t version = 0; const void* table = nullptr; size_t size = 0; };
struct Identity { std::string key; std::set<std::string> dependencies; };
std::mutex g_mutex;
std::map<std::string, Service> g_services;
std::map<const SwModHost*, Identity> g_identities;
struct ReleaseWatch { void (*fn)(const SwModHost*, void*) = nullptr; void* data = nullptr; };
std::map<const SwModHost*, ReleaseWatch> g_watchers; // provider -> callback

bool ValidName(const char* name, std::string& out) {
    if (!name) return false;
    out.clear();
    for (const char* p = name; *p; ++p) {
        const char c = *p;
        if (out.size() >= SW_SERVICE_MAX_NAME) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
        out.push_back(c);
    }
    return !out.empty();
}

SwResult Publish(const SwModHost* owner, const char* name, uint32_t version, const void* table, size_t size) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::string key;
    if (!ValidName(name, key) || !table || size < sizeof(uint32_t)) return SW_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_services.find(key);
    if (it != g_services.end() && it->second.owner != owner) return SW_CONFLICT;
    if (it == g_services.end() && g_services.size() >= 1024) return SW_LIMIT_REACHED;
    g_services[key] = {owner, version, table, size};
    LogInfo("Services: '%s' publishes '%s' v%u.%u", owner->modId ? owner->modId : "?", key.c_str(),
            SW_SERVICE_MAJOR(version), SW_SERVICE_MINOR(version));
    return SW_OK;
}
SwResult Unpublish(const SwModHost* owner, const char* name) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::string key;
    if (!ValidName(name, key)) return SW_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_services.find(key);
    if (it == g_services.end() || it->second.owner != owner) return SW_NOT_FOUND;
    g_services.erase(it);
    return SW_OK;
}
SwResult Get(const SwModHost* owner, const char* name, uint32_t minVersion, const void** table,
             uint32_t* version, size_t* size) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::string key;
    if (!ValidName(name, key) || !table) return SW_INVALID_ARGUMENT;
    *table = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_services.find(key);
    if (it == g_services.end()) return SW_NOT_FOUND;
    const Service& s = it->second;
    if (s.owner != owner) {
        auto consumer = g_identities.find(owner);
        auto provider = g_identities.find(s.owner);
        if (consumer == g_identities.end() || provider == g_identities.end() ||
            !consumer->second.dependencies.count(provider->second.key)) {
            LogWarn("Services: '%s' asked for '%s' but its provider '%s' is not in its mod.json dependencies",
                    owner->modId ? owner->modId : "?", key.c_str(), s.owner->modId ? s.owner->modId : "?");
            return SW_CONFLICT;
        }
    }
    if (version) *version = s.version;
    if (size) *size = s.size;
    if (SW_SERVICE_MAJOR(s.version) != SW_SERVICE_MAJOR(minVersion) ||
        SW_SERVICE_MINOR(s.version) < SW_SERVICE_MINOR(minVersion)) return SW_UNAVAILABLE;
    *table = s.table;
    return SW_OK;
}
SwResult SetOwnerReleaseCallback(const SwModHost* provider, void (*fn)(const SwModHost*, void*), void* data) {
    if (!RuntimeOwnerValid(provider)) return SW_INVALID_OWNER;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fn) g_watchers[provider] = {fn, data};
    else g_watchers.erase(provider);
    return SW_OK;
}
bool WatchCpp(void (*fn)(const SwModHost*, void*), const SwModHost* released, void* data) { try { fn(released, data); return true; } catch (...) { return false; } }
bool WatchSeh(void (*fn)(const SwModHost*, void*), const SwModHost* released, void* data) {
    __try { return WatchCpp(fn, released, data); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const SwServiceApi g_api = {sizeof(SwServiceApi), &Publish, &Unpublish, &Get, &SetOwnerReleaseCallback};
}

const SwServiceApi* ServicePrimitives() { return &g_api; }
void SetServiceOwnerIdentity(const SwModHost* owner, const std::string& key, const std::set<std::string>& dependencies) {
    if (!owner) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_identities[owner] = {key, dependencies};
}
void ReleaseServiceOwner(const SwModHost* owner) {
    // Tell the other providers first (outside the lock: they may call this API back).
    std::vector<std::pair<const SwModHost*, ReleaseWatch>> watchers;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_watchers.erase(owner);
        for (const auto& w : g_watchers) watchers.push_back(w);
    }
    for (const auto& w : watchers) {
        if (!WatchSeh(w.second.fn, owner, w.second.data)) {
            LogError("Services: owner-release callback of '%s' crashed; removed", w.first->modId ? w.first->modId : "?");
            std::lock_guard<std::mutex> lock(g_mutex);
            g_watchers.erase(w.first);
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_services.begin(); it != g_services.end();) {
        if (it->second.owner == owner) it = g_services.erase(it);
        else ++it;
    }
    g_identities.erase(owner);
}
}
