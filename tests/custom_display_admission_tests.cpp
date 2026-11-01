#include "../src/custom_display_admission.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    using namespace swml;
    using Result = CustomDisplayAdmission::Result;
    const CustomDisplaySpec spec{2, 1, 480, 272};
    const CustomDisplayLimits limits{2, 1, 480, 272, 26634240};
    CustomDisplayTargetPlan expected;
    assert(PlanCustomDisplayTargets(spec, limits, 4, expected) == CustomDisplayPlanResult::Ok);
    assert(expected.targetBytes == 26634240);
    CustomDisplayTargetPlan out{7,8,9,10,11};
    CustomDisplayAdmission disabled;
    assert(disabled.Reserve(spec, limits, 4, out) == Result::Disabled);
    assert(disabled.ChargedTargetBytes() == 0 && out.targetBytes == 11 && out.videoWidth == 7);
    CustomDisplayAdmission exact(expected.targetBytes);
    assert(exact.Reserve(spec, limits, 4, out) == Result::Admitted);
    assert(out.videoWidth == 1920 && out.videoHeight == 1088 && out.overlayWidth == 480);
    out.targetBytes = 17;
    assert(exact.Reserve(spec, limits, 4, out) == Result::LifetimeBudget);
    assert(out.targetBytes == 17 && exact.ChargedTargetBytes() == expected.targetBytes);
    CustomDisplayAdmission shortByOne(expected.targetBytes - 1);
    assert(shortByOne.Reserve(spec, limits, 4, out) == Result::LifetimeBudget);
    assert(shortByOne.ChargedTargetBytes() == 0);
    for (auto scale : {0u, 3u, UINT32_MAX})
        assert(shortByOne.Reserve(spec, limits, scale, out) == Result::InvalidPlan);
    assert(shortByOne.Reserve(spec, {}, 4, out) == Result::InvalidPlan);
    assert(shortByOne.Reserve({}, limits, 4, out) == Result::InvalidPlan);
    assert(shortByOne.ChargedTargetBytes() == 0 && out.targetBytes == 17);

    // A native release/reload does not imply deletion completion: no refunds.
    // Charge every quality generation, even if prior handles have been cleared.
    CustomDisplayTargetPlan low, medium;
    assert(PlanCustomDisplayTargets(spec, limits, 1, low) == CustomDisplayPlanResult::Ok);
    assert(PlanCustomDisplayTargets(spec, limits, 2, medium) == CustomDisplayPlanResult::Ok);
    const uint64_t generations = expected.targetBytes + medium.targetBytes + low.targetBytes;
    CustomDisplayAdmission reloads(generations);
    for (auto scale : {4u, 2u, 1u}) assert(reloads.Reserve(spec, limits, scale, out) == Result::Admitted);
    assert(reloads.ChargedTargetBytes() == generations);
    assert(reloads.Reserve(spec, limits, 1, out) == Result::LifetimeBudget);

    // Contended native producer threads must never over-admit. Non-divisible
    // capacity catches implementations that compare AFTER incrementing.
    const CustomDisplaySpec tiny{1,1,1,1};
    const CustomDisplayLimits tinyLimits{1,1,1,1,24};
    for (unsigned round = 0; round < 8; ++round) {
        constexpr uint64_t count = 997;
        CustomDisplayAdmission concurrent(count * 24 + 23);
        std::atomic<unsigned> admitted{0}, ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> producers;
        for (unsigned i = 0; i < 12; ++i) producers.emplace_back([&] {
            ++ready;
            while (!start.load()) std::this_thread::yield();
            for (unsigned request = 0; request < 200; ++request) {
                CustomDisplayTargetPlan plan{1,2,3,4,5};
                const auto result = concurrent.Reserve(tiny, tinyLimits, 1, plan);
                if (result == Result::Admitted) {
                    ++admitted; assert(plan.targetBytes == 24);
                } else {
                    assert(result == Result::LifetimeBudget && plan.targetBytes == 5);
                }
            }
        });
        while (ready.load() != 12) std::this_thread::yield();
        start = true;
        for (auto& producer : producers) producer.join();
        assert(admitted == count && concurrent.ChargedTargetBytes() == count * 24);
    }
    // UINT64_MAX cap arithmetic must not wrap. Invalid giant plans cannot charge.
    CustomDisplayAdmission maximum(UINT64_MAX);
    assert(maximum.Reserve({1,1,UINT32_MAX,UINT32_MAX}, {1,1,UINT32_MAX,UINT32_MAX,UINT64_MAX},
                           4, out) == Result::InvalidPlan);
    assert(maximum.ChargedTargetBytes() == 0);
    puts("PASS: disabled admission, atomic cumulative attachment budget, no deferred-release credit, concurrent native producers (model only)");
}
