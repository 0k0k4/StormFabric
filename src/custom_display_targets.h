#pragma once
#include "custom_display_model.h"
#include "custom_display_admission.h"

namespace swml {

// Experimental native queue adapter. Not exported or callable by a plugin.
// Only the separately opted-in fixed local 24x16 experiment currently uses it.
// A general bridge must certify the monitor, fields, queue, function
// ABI and engine thread BEFORE constructing this view. No GL calls or ownership
// transfer to the plugin: the stock engine owns both deferred resource handles.
struct CustomDisplayResourceHandle {
    void* object = nullptr;
    void* manager = nullptr;
};

struct CustomDisplayTargetView {
    uint32_t& logicalWidth;
    uint32_t& logicalHeight;
    uint32_t& supersampling;
    CustomDisplayResourceHandle& video;
    CustomDisplayResourceHandle& overlay;
};

// Reference ABI: native deferred target request. The sixth argument points to a
// two-pointer resource handle, not a texture ID, framebuffer or shared_ptr.
using CustomDisplayQueueTargetFn = void (*)(void*, int32_t, int32_t, int32_t, int32_t,
                                           CustomDisplayResourceHandle*);

enum class CustomDisplayQueueResult {
    Queued, MissingBackend, InvalidView, LiveResources, InvalidPlan,
    AdmissionDisabled, LifetimeBudget
};

// Native quality is NOT the multiplier: only 1 and 3 are special. Preserve the
// native default branch even for negative/future quality values.
inline uint32_t CustomDisplaySupersamplingForQuality(int32_t quality) {
    return quality == 1 ? 1u : quality == 3 ? 4u : 2u;
}

inline CustomDisplayQueueResult QueueCustomDisplayTargets(const CustomDisplaySpec& spec,
        const CustomDisplayLimits& limits, int32_t quality, void* manager,
        CustomDisplayQueueTargetFn request, CustomDisplayTargetView view,
        CustomDisplayAdmission* admission = nullptr) {
    if (!manager || !request) return CustomDisplayQueueResult::MissingBackend;
    if (&view.logicalWidth == &view.logicalHeight || &view.logicalWidth == &view.supersampling ||
        &view.logicalHeight == &view.supersampling || &view.video == &view.overlay)
        return CustomDisplayQueueResult::InvalidView;
    // A repeated create must not change dimensions while an old image is live.
    // Release/recreation must use the certified STOCK deferred destroy path.
    if (view.video.object || view.video.manager || view.overlay.object || view.overlay.manager)
        return CustomDisplayQueueResult::LiveResources;
    CustomDisplayTargetPlan plan;
    const uint32_t scale = CustomDisplaySupersamplingForQuality(quality);
    if (PlanCustomDisplayTargets(spec, limits, scale, plan) != CustomDisplayPlanResult::Ok)
        return CustomDisplayQueueResult::InvalidPlan;
    // Runtime experiments must supply a process-lifetime ledger. Null is only
    // retained for existing offline adapter tests; it is not a production policy.
    if (admission) {
        switch (admission->Reserve(spec, limits, scale, plan)) {
        case CustomDisplayAdmission::Result::Admitted: break;
        case CustomDisplayAdmission::Result::Disabled: return CustomDisplayQueueResult::AdmissionDisabled;
        case CustomDisplayAdmission::Result::LifetimeBudget: return CustomDisplayQueueResult::LifetimeBudget;
        default: return CustomDisplayQueueResult::InvalidPlan;
        }
    }
    view.logicalWidth = spec.pixelWidth;
    view.logicalHeight = spec.pixelHeight;
    view.supersampling = scale;
    // Exactly the stock colour format (5), depth format (1), handle order and queue provenance.
    // The native function has no success result: Queued does NOT mean that GPU
    // allocation is complete or that either texture is already usable.
    request(manager, int32_t(plan.videoWidth), int32_t(plan.videoHeight), 5, 1, &view.video);
    request(manager, int32_t(plan.overlayWidth), int32_t(plan.overlayHeight), 5, 1, &view.overlay);
    return CustomDisplayQueueResult::Queued;
}

} // namespace swml
