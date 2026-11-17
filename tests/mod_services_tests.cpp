#include "../src/mod_services.h"
#include "../src/mod_runtime.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace swml {
void LogInfo(const char*, ...) {}
void LogWarn(const char*, ...) {}
void LogError(const char*, ...) {}
}

namespace {
struct DemoTable { uint32_t structSize; int (*add)(int, int); };
int Add(int a, int b) { return a + b; }
const DemoTable kTable = {sizeof(DemoTable), &Add};
const DemoTable kTable2 = {sizeof(DemoTable), &Add};
void LogStub(const char*, const char*) {}
}

int main() {
    const SwServiceApi* api = swml::ServicePrimitives();
    assert(api && api->structSize == sizeof(SwServiceApi));
    SwModHost provider = {}, consumer = {}, stranger = {}, indirect = {};
    provider.modId = "browser"; consumer.modId = "ui"; stranger.modId = "other"; indirect.modId = "ui_plus";
    provider.log = consumer.log = stranger.log = indirect.log = &LogStub;
    const uint32_t v12 = SW_SERVICE_VERSION(1, 2);
    assert(SW_SERVICE_MAJOR(v12) == 1 && SW_SERVICE_MINOR(v12) == 2);

    assert(api->publish(&provider, "browser", v12, &kTable, sizeof(kTable)) == SW_INVALID_OWNER);
    for (auto* host : {&provider, &consumer, &stranger, &indirect}) swml::RegisterRuntimeOwner(host);
    swml::SetServiceOwnerIdentity(&provider, "sf_browser", {});
    swml::SetServiceOwnerIdentity(&consumer, "ui_mod", {"sf_browser"});
    swml::SetServiceOwnerIdentity(&indirect, "ui_plus", {"ui_mod", "sf_browser"}); // transitive
    swml::SetServiceOwnerIdentity(&stranger, "other", {});

    // Names and arguments.
    assert(api->publish(&provider, "", v12, &kTable, sizeof(kTable)) == SW_INVALID_ARGUMENT);
    assert(api->publish(&provider, "Browser", v12, &kTable, sizeof(kTable)) == SW_INVALID_ARGUMENT);
    assert(api->publish(&provider, "a b", v12, &kTable, sizeof(kTable)) == SW_INVALID_ARGUMENT);
    assert(api->publish(&provider, std::string(65, 'a').c_str(), v12, &kTable, sizeof(kTable)) == SW_INVALID_ARGUMENT);
    assert(api->publish(&provider, std::string(64, 'a').c_str(), v12, &kTable, sizeof(kTable)) == SW_OK);
    assert(api->publish(&provider, "browser", v12, nullptr, sizeof(kTable)) == SW_INVALID_ARGUMENT);
    assert(api->publish(&provider, "browser", v12, &kTable, 2) == SW_INVALID_ARGUMENT);
    assert(api->publish(&provider, "browser", v12, &kTable, sizeof(kTable)) == SW_OK);
    assert(api->publish(&stranger, "browser", v12, &kTable2, sizeof(kTable2)) == SW_CONFLICT);

    // Consumer that depends on the provider.
    const void* table = nullptr; uint32_t version = 0; size_t size = 0;
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 0), &table, &version, &size) == SW_OK);
    assert(table == &kTable && version == v12 && size == sizeof(kTable));
    assert(static_cast<const DemoTable*>(table)->add(2, 3) == 5);
    assert(api->get(&indirect, "browser", SW_SERVICE_VERSION(1, 2), &table, nullptr, nullptr) == SW_OK);
    assert(api->get(&provider, "browser", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_OK); // itself

    // Versions: same major, minor at least the requested one.
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 3), &table, &version, nullptr) == SW_UNAVAILABLE);
    assert(table == nullptr && version == v12);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(2, 0), &table, nullptr, nullptr) == SW_UNAVAILABLE);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(0, 0), &table, nullptr, nullptr) == SW_UNAVAILABLE);

    // A mod that does not depend on the provider is refused.
    assert(api->get(&stranger, "browser", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_CONFLICT && !table);
    assert(api->get(&consumer, "missing", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_NOT_FOUND);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 0), nullptr, nullptr, nullptr) == SW_INVALID_ARGUMENT);

    // Republish (replace) by the owner, unpublish by a stranger refused.
    assert(api->publish(&provider, "browser", SW_SERVICE_VERSION(1, 5), &kTable2, sizeof(kTable2)) == SW_OK);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 5), &table, nullptr, nullptr) == SW_OK && table == &kTable2);
    assert(api->unpublish(&stranger, "browser") == SW_NOT_FOUND);
    assert(api->unpublish(&provider, "browser") == SW_OK);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_NOT_FOUND);

    // Unload withdraws every service of the owner; the name is free again.
    assert(api->publish(&provider, "browser", v12, &kTable, sizeof(kTable)) == SW_OK);
    swml::ReleaseServiceOwner(&provider);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_NOT_FOUND);
    assert(api->publish(&stranger, "browser", v12, &kTable2, sizeof(kTable2)) == SW_OK);
    // The consumer does not depend on "other": refused even though the name exists again.
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_CONFLICT);

    // An owner unknown to the loader (no identity) can publish but not consume others.
    swml::ReleaseServiceOwner(&consumer);
    assert(api->get(&consumer, "browser", SW_SERVICE_VERSION(1, 0), &table, nullptr, nullptr) == SW_CONFLICT);

    // Owner-release callbacks: providers hear about other mods unloading, never about themselves.
    {
        SwModHost a = {}, b = {}, c = {};
        a.modId = "a"; b.modId = "b"; c.modId = "c";
        for (auto* host : {&a, &b, &c}) swml::RegisterRuntimeOwner(host);
        static std::vector<const SwModHost*> heard;
        heard.clear();
        auto listen = [](const SwModHost* released, void* data) { heard.push_back(released); (void)data; };
        assert(api->setOwnerReleaseCallback(&a, listen, nullptr) == SW_OK);
        swml::ReleaseServiceOwner(&b);
        assert(heard.size() == 1 && heard[0] == &b);
        swml::ReleaseServiceOwner(&a); // the provider itself: callback removed, not called
        assert(heard.size() == 1);
        swml::ReleaseServiceOwner(&c);
        assert(heard.size() == 1);
        auto crash = [](const SwModHost*, void*) { *static_cast<volatile int*>(nullptr) = 1; };
        assert(api->setOwnerReleaseCallback(&c, crash, nullptr) == SW_OK);
        swml::ReleaseServiceOwner(&b); // crash is contained and the callback dropped
        swml::ReleaseServiceOwner(&b);
        assert(api->setOwnerReleaseCallback(&c, nullptr, nullptr) == SW_OK);
    }

    std::puts("mod_services_tests: OK");
    return 0;
}
