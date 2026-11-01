#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace swml {

// Experimental, engine-independent model. Not an SDK API or an active backend.
// Physical dimensions use whole voxel blocks in the monitor's X/Z plane.
// Pixel dimensions are independent; native reflection index is NOT part of this
// identity (the vehicle serializer already stores it separately).
struct CustomDisplaySpec {
    uint32_t widthBlocks = 0, heightBlocks = 0;
    uint32_t pixelWidth = 0, pixelHeight = 0;
};

inline bool operator==(const CustomDisplaySpec& a, const CustomDisplaySpec& b) {
    return a.widthBlocks == b.widthBlocks && a.heightBlocks == b.heightBlocks &&
        a.pixelWidth == b.pixelWidth && a.pixelHeight == b.pixelHeight;
}

inline bool CustomDisplayDimensionsValid(const CustomDisplaySpec& spec) {
    return spec.widthBlocks && spec.heightBlocks && spec.pixelWidth && spec.pixelHeight;
}

// Canonical ASCII identity: no path separators, locale dependence or hash
// collisions. Version 1 is a prototype persistence format, not yet installed in
// the native registry. Geometry changes may require a new version.
inline bool CustomDisplayDefinitionId(const CustomDisplaySpec& spec, std::string& out) {
    if (!CustomDisplayDimensionsValid(spec)) return false;
    std::string id = "swml_cd_v1_b" + std::to_string(spec.widthBlocks) + "x" +
        std::to_string(spec.heightBlocks) + "_p" + std::to_string(spec.pixelWidth) +
        "x" + std::to_string(spec.pixelHeight);
    out = std::move(id);
    return true;
}

inline bool ParseCustomDisplayDefinitionId(std::string_view id, CustomDisplaySpec& out) {
    constexpr std::string_view prefix = "swml_cd_v1_b";
    if (id.substr(0, prefix.size()) != prefix) return false;
    id.remove_prefix(prefix.size());
    auto number = [&id](uint32_t& value) {
        // Reject zero, signs, whitespace and noncanonical leading zeroes.
        if (id.empty() || id.front() < '1' || id.front() > '9') return false;
        uint32_t n = 0;
        size_t length = 0;
        while (length < id.size() && id[length] >= '0' && id[length] <= '9') {
            uint32_t digit = uint32_t(id[length] - '0');
            if (n > ((std::numeric_limits<uint32_t>::max)() - digit) / 10) return false;
            n = n * 10 + digit;
            ++length;
        }
        id.remove_prefix(length);
        value = n;
        return true;
    };
    auto separator = [&id](std::string_view expected) {
        if (id.substr(0, expected.size()) != expected) return false;
        id.remove_prefix(expected.size());
        return true;
    };
    CustomDisplaySpec spec;
    if (!number(spec.widthBlocks) || !separator("x") || !number(spec.heightBlocks) ||
        !separator("_p") || !number(spec.pixelWidth) || !separator("x") ||
        !number(spec.pixelHeight) || !id.empty()) return false;
    out = spec;
    return true;
}

// No guessed game/GPU limits: the caller must supply a certified policy. A zero
// policy is disabled. Native format 5 accounts for 8 colour bytes/pixel and
// depth format 1 for 4 more. This excludes driver overhead, other monitors and
// deferred old resources; it is NOT a measured GPU allocation or global budget.
inline constexpr uint32_t CustomDisplayColourBytesPerPixel = 8;
inline constexpr uint32_t CustomDisplayDepthBytesPerPixel = 4;
inline constexpr uint32_t CustomDisplayTargetBytesPerPixel =
    CustomDisplayColourBytesPerPixel + CustomDisplayDepthBytesPerPixel;
struct CustomDisplayLimits {
    uint32_t maxWidthBlocks = 0, maxHeightBlocks = 0;
    uint32_t maxPixelWidth = 0, maxPixelHeight = 0;
    uint64_t maxTargetBytes = 0;
};

struct CustomDisplayTargetPlan {
    uint32_t videoWidth = 0, videoHeight = 0;
    uint32_t overlayWidth = 0, overlayHeight = 0;
    uint64_t targetBytes = 0;
};

enum class CustomDisplayPlanResult {
    Ok, InvalidLimits, InvalidDimensions, PhysicalLimit, PixelLimit,
    InvalidSupersampling, NativeRange, NativeAccountingRange, TargetBudget
};

// supersampling is the resolved multiplier (1/2/4), NOT a native quality enum.
inline CustomDisplayPlanResult PlanCustomDisplayTargets(const CustomDisplaySpec& spec,
        const CustomDisplayLimits& limits, uint32_t supersampling, CustomDisplayTargetPlan& out) {
    using Result = CustomDisplayPlanResult;
    if (!limits.maxWidthBlocks || !limits.maxHeightBlocks || !limits.maxPixelWidth ||
        !limits.maxPixelHeight || !limits.maxTargetBytes) return Result::InvalidLimits;
    if (!CustomDisplayDimensionsValid(spec)) return Result::InvalidDimensions;
    if (spec.widthBlocks > limits.maxWidthBlocks || spec.heightBlocks > limits.maxHeightBlocks)
        return Result::PhysicalLimit;
    if (spec.pixelWidth > limits.maxPixelWidth || spec.pixelHeight > limits.maxPixelHeight)
        return Result::PixelLimit;
    if (supersampling != 1 && supersampling != 2 && supersampling != 4)
        return Result::InvalidSupersampling;
    constexpr auto maxNative = uint32_t((std::numeric_limits<int32_t>::max)());
    if (spec.widthBlocks > maxNative || spec.heightBlocks > maxNative ||
        spec.pixelWidth > maxNative / supersampling || spec.pixelHeight > maxNative / supersampling)
        return Result::NativeRange;
    CustomDisplayTargetPlan plan;
    plan.videoWidth = spec.pixelWidth * supersampling;
    plan.videoHeight = spec.pixelHeight * supersampling;
    plan.overlayWidth = spec.pixelWidth;
    plan.overlayHeight = spec.pixelHeight;
    // Each product fits uint64_t after the signed native dimension checks.
    uint64_t videoPixels = uint64_t(plan.videoWidth) * plan.videoHeight;
    uint64_t overlayPixels = uint64_t(plan.overlayWidth) * plan.overlayHeight;
    // The native resource accumulates colour + depth in a signed 32-bit field.
    // Reject before its arithmetic can overflow, even with an unlimited policy.
    if (videoPixels > maxNative / CustomDisplayTargetBytesPerPixel ||
        overlayPixels > maxNative / CustomDisplayTargetBytesPerPixel)
        return Result::NativeAccountingRange;
    uint64_t budgetPixels = limits.maxTargetBytes / CustomDisplayTargetBytesPerPixel;
    if (videoPixels > budgetPixels || overlayPixels > budgetPixels - videoPixels)
        return Result::TargetBudget;
    plan.targetBytes = (videoPixels + overlayPixels) * CustomDisplayTargetBytesPerPixel;
    out = plan;
    return Result::Ok;
}

} // namespace swml
