// Tests for the component attribute primitives added to the SDK:
//   * the generated table matches the kit (120 fields, 80 attributes,
//     38 elements, 2 internal);
//   * Find/At return the proven offset and width (mass, flags, voxel_min);
//   * unknown names and out-of-bounds indices fail closed;
//   * the width test: writing a float (4 B) and a flag (8 B) into a 0xA5
//     sentinel buffer leaves every neighbouring byte untouched;
//   * NULL component, unreadable pointers, strings and vectors are refused.
// The test never touches game memory: the component is a local buffer.
#include "../src/native_prims.h"
#include "../src/component_attrs.generated.h"
#include "../include/sw_mod_sdk.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace swml {
void LogError(const char*, ...) {}
void LogWarn(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
}  // namespace swml

namespace {

SwModHost g_host = {};
const SwNativeApi* g_api = nullptr;

constexpr uint8_t kSentinel = 0xA5;
constexpr size_t kBufferSize = 0x2d00;

void FillSentinel(std::vector<uint8_t>& buffer) {
    buffer.assign(kBufferSize, kSentinel);
}

void ExpectBytes(const std::vector<uint8_t>& buffer, size_t begin, size_t end,
                 uint8_t expected, const char* what) {
    for (size_t i = begin; i < end; ++i) {
        if (buffer[i] != expected) {
            printf("FAIL: %s: byte 0x%zx is 0x%02x, expected 0x%02x\n", what, i,
                   buffer[i], expected);
            assert(false);
        }
    }
}

void TestTableMatchesKit() {
    // Counts documented by the kit README and the JSON stats block.
    assert(kComponentAttrCount == 120);
    assert(kComponentAttrAttributeCount == 80);
    assert(kComponentAttrElementCount == 38);
    assert(kComponentAttrInternalCount == 2);
    assert(kComponentAttrWidth1Count == 2);
    assert(kComponentAttrWidth4Count == 62);
    assert(kComponentAttrWidth8Count == 1);
    assert(kComponentAttrWidth12Count == 38);
    assert(kComponentAttrWidth16Count == 17);
    assert(kComponentAttrMaxEnd == 0x2cf8u);

    assert(g_api->componentAttrCount(&g_host) == 120);
    puts("PASS: generated table matches the kit (120/80/38/2)");
}

void TestFindAndAt() {
    const SwComponentAttrInfo* mass = g_api->componentAttrFind(&g_host, "mass");
    assert(mass != nullptr);
    assert(mass->offset == 0x2b0u);
    assert(mass->width == 4);
    assert(mass->type == SW_ATTR_FLOAT);
    assert(!mass->isElement);

    // The kit corrected flags from 4 to 8 bytes: the table must carry 8.
    const SwComponentAttrInfo* flags = g_api->componentAttrFind(&g_host, "flags");
    assert(flags != nullptr);
    assert(flags->offset == 0x2b8u);
    assert(flags->width == 8);
    assert(flags->type == SW_ATTR_UINT64);

    const SwComponentAttrInfo* voxel = g_api->componentAttrFind(&g_host, "voxel_min");
    assert(voxel != nullptr);
    assert(voxel->offset == 0xc0u);
    assert(voxel->width == 12);
    assert(voxel->type == SW_ATTR_INT3);
    assert(voxel->isElement);

    const SwComponentAttrInfo* name = g_api->componentAttrFind(&g_host, "name");
    assert(name != nullptr);
    assert(name->type == SW_ATTR_STRING);
    assert(name->width == 16);

    // Unknown name fails closed.
    assert(g_api->componentAttrFind(&g_host, "not_a_component_field") == nullptr);
    assert(g_api->componentAttrFind(&g_host, "") == nullptr);
    assert(g_api->componentAttrFind(&g_host, nullptr) == nullptr);

    // Out-of-bounds index fails closed; index 0 is valid.
    assert(g_api->componentAttrAt(&g_host, 0) != nullptr);
    assert(g_api->componentAttrAt(&g_host, 119) != nullptr);
    assert(g_api->componentAttrAt(&g_host, 120) == nullptr);
    assert(g_api->componentAttrAt(&g_host, 0xFFFFFFFFu) == nullptr);
    puts("PASS: Find/At offsets, widths and bounds");
}

void TestWidthSentinel() {
    std::vector<uint8_t> buffer;
    FillSentinel(buffer);

    // Float: 4 bytes at 0x2b0. Neighbours 0x2ac..0x2b0 and 0x2b4..0x2b8 stay 0xA5.
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "mass", 1.5) == SW_OK);
    ExpectBytes(buffer, 0x2ac, 0x2b0, kSentinel, "mass left neighbour");
    ExpectBytes(buffer, 0x2b4, 0x2b8, kSentinel, "mass right neighbour");
    float mass = 0.0f;
    memcpy(&mass, buffer.data() + 0x2b0, sizeof(mass));
    assert(mass == 1.5f);
    double readBack = 0.0;
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "mass", &readBack) == SW_OK);
    assert(readBack == 1.5);

    // Flags: 8 bytes at 0x2b8. The 4-byte gap 0x2b4..0x2b8 and the byte at
    // 0x2c0 must stay 0xA5; a 4-byte write here would corrupt 0x2bc..0x2c0.
    // The value stays below 2^53 so the double round-trip is exact.
    FillSentinel(buffer);
    const uint64_t flagValue = 0x0000000123456789ull;
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "flags",
                                     static_cast<double>(flagValue)) == SW_OK);
    ExpectBytes(buffer, 0x2b0, 0x2b8, kSentinel, "flags left neighbour");
    ExpectBytes(buffer, 0x2c0, 0x2c4, kSentinel, "flags right neighbour");
    uint64_t flags = 0;
    memcpy(&flags, buffer.data() + 0x2b8, sizeof(flags));
    assert(flags == flagValue);
    readBack = 0.0;
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "flags", &readBack) == SW_OK);
    assert(readBack == static_cast<double>(flagValue));

    // Bool: 1 byte at 0xe04. Only that byte changes.
    FillSentinel(buffer);
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "door_flipped", 1.0) == SW_OK);
    ExpectBytes(buffer, 0xe03, 0xe04, kSentinel, "bool left neighbour");
    ExpectBytes(buffer, 0xe05, 0xe0c, kSentinel, "bool right neighbour");
    assert(buffer[0xe04] == 1);
    readBack = 0.0;
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "door_flipped", &readBack) == SW_OK);
    assert(readBack == 1.0);
    puts("PASS: width sentinel (float 4 B, flags 8 B, bool 1 B)");
}

void TestFailClosed() {
    std::vector<uint8_t> buffer;
    FillSentinel(buffer);
    double out = 0.0;

    // Unknown name.
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "nope", &out) == SW_NOT_FOUND);
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "nope", 1.0) == SW_NOT_FOUND);

    // NULL component.
    assert(g_api->componentAttrRead(&g_host, nullptr, "mass", &out) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrWrite(&g_host, nullptr, "mass", 1.0) == SW_INVALID_ARGUMENT);

    // NULL name / NULL out.
    assert(g_api->componentAttrRead(&g_host, buffer.data(), nullptr, &out) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "mass", nullptr) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), nullptr, 1.0) == SW_INVALID_ARGUMENT);

    // Unreadable/unwritable pointer: VirtualQuery rejects it before any access.
    assert(g_api->componentAttrRead(&g_host, reinterpret_cast<const void*>(0x10), "mass", &out) ==
           SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrWrite(&g_host, reinterpret_cast<void*>(0x10), "mass", 1.0) ==
           SW_INVALID_ARGUMENT);

    // Non-scalar types are refused explicitly, never converted.
    FillSentinel(buffer);
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "name", &out) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "name", 1.0) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrRead(&g_host, buffer.data(), "voxel_min", &out) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "voxel_min", 1.0) == SW_INVALID_ARGUMENT);
    ExpectBytes(buffer, 0, kBufferSize, kSentinel, "refused writes leave the buffer untouched");

    // Out-of-range integer values are refused instead of wrapping.
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "value", 1e30) == SW_INVALID_ARGUMENT);
    assert(g_api->componentAttrWrite(&g_host, buffer.data(), "flags", -1.0) == SW_INVALID_ARGUMENT);
    puts("PASS: fail-closed reads/writes (unknown, NULL, unreadable, non-scalar)");
}

}  // namespace

int main() {
    swml::native::SetNativeOwnerCheck(nullptr);
    swml::native::InitializeNativePrimitives(nullptr);
    g_host.sdkVersion = SW_MOD_SDK_VERSION;
    g_host.modId = "component_attrs_tests";
    g_api = swml::native::NativePrimitives();
    assert(g_api != nullptr);
    assert(g_api->structSize == sizeof(SwNativeApi));

    TestTableMatchesKit();
    TestFindAndAt();
    TestWidthSentinel();
    TestFailClosed();
    puts("PASS: component attribute suites");
    return 0;
}
