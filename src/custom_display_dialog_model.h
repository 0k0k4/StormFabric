#pragma once
#include "custom_display_model.h"
#include <array>

namespace swml {

// Offline workflow-B model, not a native widget or an authorization to import,
// allocate targets or select a definition. The future adapter must copy live
// editbox text on its owning thread and supply a separately certified policy.
enum class CustomDisplayNumberError { Ok, Empty, TooLong, InvalidCharacter, Zero, Overflow };

inline CustomDisplayNumberError ParseCustomDisplayDialogNumber(std::string_view text,
        uint32_t& out) {
    using Error = CustomDisplayNumberError;
    if (text.empty()) return Error::Empty;
    // A uint32 decimal needs at most ten characters. Bound work even for pasted
    // input; do not silently truncate, strip characters or use locale parsing.
    if (text.size() > 10) return Error::TooLong;
    uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return Error::InvalidCharacter;
        const uint32_t digit = uint32_t(c - '0');
        if (value > ((std::numeric_limits<uint32_t>::max)() - digit) / 10)
            return Error::Overflow;
        value = value * 10 + digit;
    }
    if (!value) return Error::Zero;
    out = value;
    return Error::Ok;
}

// Width/height in blocks, then width/height in pixels. Leading zeroes in the
// editor are accepted; the persisted identity is always canonicalized.
using CustomDisplayDialogFields = std::array<std::string_view, 4>;
struct CustomDisplayDialogValidation {
    size_t field = 4; // 0..3 for a text error; 4 when no individual field failed
    CustomDisplayNumberError number = CustomDisplayNumberError::Ok;
    CustomDisplayPlanResult plan = CustomDisplayPlanResult::Ok;
    bool Ok() const { return number == CustomDisplayNumberError::Ok &&
        plan == CustomDisplayPlanResult::Ok; }
};

struct CustomDisplayDialogRequest {
    CustomDisplaySpec spec;
    std::string definitionId;
    CustomDisplayTargetPlan largestPlan;
};

inline CustomDisplayDialogValidation ValidateCustomDisplayDialog(
        const CustomDisplayDialogFields& fields, const CustomDisplayLimits& limits,
        CustomDisplayDialogRequest& out) {
    CustomDisplayDialogValidation validation;
    std::array<uint32_t, 4> values{};
    for (size_t i = 0; i < fields.size(); ++i) {
        validation.number = ParseCustomDisplayDialogNumber(fields[i], values[i]);
        if (validation.number != CustomDisplayNumberError::Ok) {
            validation.field = i;
            return validation;
        }
    }
    CustomDisplayDialogRequest request;
    request.spec = {values[0], values[1], values[2], values[3]};
    // Never validate only the currently selected quality. The largest resolved
    // native multiplier covers 1/2/4, but this is not cumulative admission.
    validation.plan = PlanCustomDisplayTargets(request.spec, limits, 4, request.largestPlan);
    if (!validation.Ok()) return validation;
    CustomDisplayDefinitionId(request.spec, request.definitionId);
    out = std::move(request);
    return validation;
}

// Owner-thread-only draft lifetime. No engine pointers, callbacks, resources or
// selection changes are retained here. Invalidate BEFORE tearing down native
// children. Take yields a request for registration, never a placement permit.
class CustomDisplayDialogDraft {
public:
    using Token = uint64_t;
    CustomDisplayDialogDraft() = default;
    CustomDisplayDialogDraft(const CustomDisplayDialogDraft&) = delete;
    CustomDisplayDialogDraft& operator=(const CustomDisplayDialogDraft&) = delete;

    Token Open() {
        Invalidate();
        if (generation_ == (std::numeric_limits<Token>::max)()) return 0;
        open_ = true;
        return ++generation_;
    }
    bool Current(Token token) const { return open_ && token && token == generation_; }
    bool Validate(Token token, const CustomDisplayDialogFields& fields,
            const CustomDisplayLimits& limits, CustomDisplayDialogValidation& out) {
        if (!Current(token)) return false;
        ready_ = false;
        pending_ = {};
        out = ValidateCustomDisplayDialog(fields, limits, pending_);
        ready_ = out.Ok();
        return true;
    }
    bool Take(Token token, CustomDisplayDialogRequest& out) {
        if (!Current(token) || !ready_) return false;
        out = std::move(pending_);
        Invalidate();
        return true;
    }
    bool Cancel(Token token) {
        if (!Current(token)) return false;
        Invalidate();
        return true;
    }
    void Invalidate() {
        open_ = ready_ = false;
        pending_ = {};
    }
private:
    Token generation_ = 0;
    bool open_ = false, ready_ = false;
    CustomDisplayDialogRequest pending_;
};

} // namespace swml
