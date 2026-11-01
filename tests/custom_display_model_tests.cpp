#include "../src/custom_display_model.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace swml;

int main() {
    const CustomDisplaySpec a{2, 1, 480, 272};
    std::string id;
    assert(CustomDisplayDefinitionId(a, id));
    assert(id == "swml_cd_v1_b2x1_p480x272");
    CustomDisplaySpec decoded;
    assert(ParseCustomDisplayDefinitionId(id, decoded) && decoded == a);

    std::string changed;
    for (const CustomDisplaySpec spec : {CustomDisplaySpec{3,1,480,272}, {2,2,480,272},
            {2,1,481,272}, {2,1,480,273}}) {
        assert(CustomDisplayDefinitionId(spec, changed) && changed != id);
    }
    // Several monitors can share a configuration without encoding a live pointer
    // or reflection index in the identity. Physical size never determines pixels.
    for (uint32_t w = 1; w <= 20; ++w) {
        for (uint32_t h = 1; h <= 20; ++h) {
            CustomDisplaySpec spec{w, h, 17 * h + 1, 13 * w + 3};
            assert(CustomDisplayDefinitionId(spec, changed));
            assert(ParseCustomDisplayDefinitionId(changed, decoded) && decoded == spec);
        }
    }
    const auto max32 = (std::numeric_limits<uint32_t>::max)();
    CustomDisplaySpec largest{max32, max32, max32, max32};
    assert(CustomDisplayDefinitionId(largest, changed));
    assert(ParseCustomDisplayDefinitionId(changed, decoded) && decoded == largest);

    const char* invalid[] = {"", "monitor_1x2", "SWML_cd_v1_b2x1_p480x272",
        "swml_cd_v2_b2x1_p480x272", "swml_cd_v1_b0x1_p480x272",
        "swml_cd_v1_b2x0_p480x272", "swml_cd_v1_b2x1_p0x272",
        "swml_cd_v1_b2x1_p480x0", "swml_cd_v1_b02x1_p480x272",
        "swml_cd_v1_b2x1_p0480x272", "swml_cd_v1_b+2x1_p480x272",
        "swml_cd_v1_b-2x1_p480x272", "swml_cd_v1_b2x1_p480x272.xml",
        "swml_cd_v1_b2x1_p480x272/../monitor_1x2", "swml_cd_v1_b2x1_p480x272 ",
        "swml_cd_v1_b2x1_p480x272x", "swml_cd_v1_b2x1_p480x",
        "swml_cd_v1_b4294967296x1_p1x1", "swml_cd_v1_b1x1_p1x4294967296",
        "swml_cd_v1_b999999999999999999999999x1_p1x1"};
    decoded = a;
    for (const char* text : invalid) {
        assert(!ParseCustomDisplayDefinitionId(text, decoded) && decoded == a);
    }
    std::string embedded = id + std::string(1, '\0');
    assert(!ParseCustomDisplayDefinitionId(embedded, decoded) && decoded == a);
    changed = "unchanged";
    for (const CustomDisplaySpec spec : {CustomDisplaySpec{0,1,1,1}, {1,0,1,1},
            {1,1,0,1}, {1,1,1,0}}) {
        assert(!CustomDisplayDefinitionId(spec, changed) && changed == "unchanged");
    }

    using Result = CustomDisplayPlanResult;
    // Test policy only, NOT advertised hardware or game limits.
    CustomDisplayLimits limits{16, 16, 4096, 4096, 64 * 1024 * 1024};
    CustomDisplayTargetPlan plan;
    for (uint32_t scale : {1u, 2u, 4u}) {
        assert(PlanCustomDisplayTargets(a, limits, scale, plan) == Result::Ok);
        assert(plan.videoWidth == 480 * scale && plan.videoHeight == 272 * scale);
        assert(plan.overlayWidth == 480 && plan.overlayHeight == 272);
        assert(plan.targetBytes == uint64_t(480) * 272 * (1 + scale * scale) * 12);
    }
    auto before = plan;
    auto rejected = [&](const CustomDisplaySpec& spec, const CustomDisplayLimits& policy,
                        uint32_t scale, Result result) {
        assert(PlanCustomDisplayTargets(spec, policy, scale, plan) == result);
        assert(plan.videoWidth == before.videoWidth && plan.videoHeight == before.videoHeight &&
            plan.overlayWidth == before.overlayWidth && plan.overlayHeight == before.overlayHeight &&
            plan.targetBytes == before.targetBytes);
    };
    rejected(a, {}, 1, Result::InvalidLimits);
    for (unsigned field = 0; field != 5; ++field) {
        auto disabled = limits;
        switch (field) {
        case 0: disabled.maxWidthBlocks = 0; break;
        case 1: disabled.maxHeightBlocks = 0; break;
        case 2: disabled.maxPixelWidth = 0; break;
        case 3: disabled.maxPixelHeight = 0; break;
        case 4: disabled.maxTargetBytes = 0; break;
        }
        rejected(a, disabled, 1, Result::InvalidLimits);
    }
    rejected({}, limits, 1, Result::InvalidDimensions);
    rejected({17,1,480,272}, limits, 1, Result::PhysicalLimit);
    rejected({1,17,480,272}, limits, 1, Result::PhysicalLimit);
    rejected({2,1,4097,272}, limits, 1, Result::PixelLimit);
    rejected({2,1,480,4097}, limits, 1, Result::PixelLimit);
    for (uint32_t scale : {0u, 3u, 5u, max32}) rejected(a, limits, scale, Result::InvalidSupersampling);

    auto small = limits;
    small.maxTargetBytes = before.targetBytes - 1;
    rejected(a, small, 4, Result::TargetBudget);
    small.maxTargetBytes = before.targetBytes;
    assert(PlanCustomDisplayTargets(a, small, 4, plan) == Result::Ok);
    assert(plan.targetBytes == small.maxTargetBytes);
    small.maxTargetBytes = 1;
    rejected(a, small, 1, Result::TargetBudget);

    const auto max64 = (std::numeric_limits<uint64_t>::max)();
    CustomDisplayLimits unlimited{max32, max32, max32, max32, max64};
    rejected(largest, unlimited, 1, Result::NativeRange);
    rejected({1,1,0x80000000u,1}, unlimited, 1, Result::NativeRange);
    rejected({1,1,0x20000000u,1}, unlimited, 4, Result::NativeRange);
    rejected({1,1,1,0x20000000u}, unlimited, 4, Result::NativeRange);
    rejected({1,1,0x7fffffffu,0x7fffffffu}, unlimited, 1, Result::NativeAccountingRange);
    rejected({1,1,0x1fffffffu,1}, unlimited, 4, Result::NativeAccountingRange);
    // Exact native per-resource accounting boundary; policy totals use uint64.
    constexpr uint32_t maxAccountedPixels = 0x7fffffffu / 12;
    rejected({1,1,maxAccountedPixels + 1,1}, unlimited, 1, Result::NativeAccountingRange);
    rejected({1,1,1,maxAccountedPixels + 1}, unlimited, 1, Result::NativeAccountingRange);
    assert(PlanCustomDisplayTargets({1,1,maxAccountedPixels,1}, unlimited, 1, plan) == Result::Ok);
    assert(plan.targetBytes == uint64_t(maxAccountedPixels) * 24);
    // Former four-byte budget must no longer authorize either native attachment pair.
    before = plan;
    small = limits; small.maxTargetBytes = uint64_t(480) * 272 * 2 * 4;
    rejected(a, small, 1, Result::TargetBudget);
    small.maxTargetBytes = uint64_t(480) * 272 * 2 * 12;
    for (uint32_t extra = 0; extra < 12; ++extra) {
        small.maxTargetBytes = uint64_t(480) * 272 * 2 * 12 + extra;
        assert(PlanCustomDisplayTargets(a, small, 1, plan) == Result::Ok);
        assert(plan.targetBytes == uint64_t(480) * 272 * 24);
    }
    puts("PASS: custom display canonical identity, independent dimensions, explicit policy, target budget, overflow and atomic rejection");
}
