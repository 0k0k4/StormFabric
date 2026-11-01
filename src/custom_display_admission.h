#pragma once
#include "custom_display_model.h"
#include <atomic>

namespace swml {
// Conservative contribution ledger, NOT the engine's global GPU accounting.
// The runtime observation proves monitor creation can enter on several native
// threads. A check-then-increment counter (or a mod-main-thread counter) is unsafe.
//
// Every admitted creation remains charged until PROCESS exit, including partial
// queue failures. Clearing handles, destroying a monitor, changing quality or
// observing an empty producer queue must NEVER return credit. Without certified
// backend completion, retaining credit is the only defensible conservative bound.
// This deliberately can exhaust during repeated reloads. It does not certify
// free memory, stock/native usage, driver overhead or actual GL allocation.
class CustomDisplayAdmission {
public:
    explicit CustomDisplayAdmission(uint64_t maxLifetimeTargetBytes = 0)
        : maximum_(maxLifetimeTargetBytes) {}
    CustomDisplayAdmission(const CustomDisplayAdmission&) = delete;
    CustomDisplayAdmission& operator=(const CustomDisplayAdmission&) = delete;

    enum class Result { Admitted, Disabled, InvalidPlan, LifetimeBudget };

    Result Reserve(const CustomDisplaySpec& spec, const CustomDisplayLimits& limits,
                   uint32_t supersampling, CustomDisplayTargetPlan& out) noexcept {
        // Defaults remain disabled. Even an approved per-resource policy cannot
        // accidentally activate a process without an explicit contribution cap.
        if (!maximum_) return Result::Disabled;
        CustomDisplayTargetPlan plan;
        if (PlanCustomDisplayTargets(spec, limits, supersampling, plan) != CustomDisplayPlanResult::Ok)
            return Result::InvalidPlan;
        uint64_t used = charged_.load(std::memory_order_relaxed);
        do {
            if (used > maximum_ || plan.targetBytes > maximum_ - used)
                return Result::LifetimeBudget;
            // No publication of engine resources here, so relaxed ordering is
            // sufficient: the CAS only arbitrates admission to this byte bound.
        } while (!charged_.compare_exchange_weak(used, used + plan.targetBytes,
                                                std::memory_order_relaxed));
        out = plan;
        return Result::Admitted;
    }

    uint64_t ChargedTargetBytes() const noexcept {
        return charged_.load(std::memory_order_relaxed);
    }

private:
    const uint64_t maximum_;
    std::atomic<uint64_t> charged_{0};
    // No Reset/Release/Refund API: instance must outlive ALL custom resources,
    // queued events and hooks. Production ownership must be process-lifetime.
};
} // namespace swml
