#include "../src/custom_display_dialog_model.h"
#include <cassert>
#include <cstdio>

using namespace swml;

int main() {
    using Number = CustomDisplayNumberError;
    uint32_t number = 123;
    for (const auto& item : {std::pair<std::string_view, Number>{"", Number::Empty},
            {"0", Number::Zero}, {"000", Number::Zero}, {"-1", Number::InvalidCharacter},
            {"+1", Number::InvalidCharacter}, {" 1", Number::InvalidCharacter},
            {"1 ", Number::InvalidCharacter}, {"1.5", Number::InvalidCharacter},
            {"1e2", Number::InvalidCharacter}, {"1\n", Number::InvalidCharacter},
            {"4294967296", Number::Overflow}, {"9999999999", Number::Overflow},
            {"00000000001", Number::TooLong}, {"12345678901", Number::TooLong},
            {std::string_view("1\0", 2), Number::InvalidCharacter},
            {"\xd9\xa1", Number::InvalidCharacter}}) {
        assert(ParseCustomDisplayDialogNumber(item.first, number) == item.second);
        assert(number == 123);
    }
    assert(ParseCustomDisplayDialogNumber("4294967295", number) == Number::Ok);
    assert(number == UINT32_MAX);
    assert(ParseCustomDisplayDialogNumber("0000000012", number) == Number::Ok && number == 12);
    for (uint32_t n = 1; n < 10000; ++n) {
        assert(ParseCustomDisplayDialogNumber(std::to_string(n), number) == Number::Ok && number == n);
    }

    // Test policy, NOT certified native limits. Validation does not install it.
    const CustomDisplayLimits limits{16, 16, 4096, 4096, 64 * 1024 * 1024};
    const CustomDisplayDialogFields fields{"02", "1", "480", "272"};
    CustomDisplayDialogRequest request;
    assert(ValidateCustomDisplayDialog(fields, limits, request).Ok());
    assert((request.spec == CustomDisplaySpec{2, 1, 480, 272}));
    assert(request.definitionId == "swml_cd_v1_b2x1_p480x272");
    assert(request.largestPlan.targetBytes == uint64_t(480) * 272 * 17 * 12);
    const auto saved = request;
    auto unchanged = [&] {
        assert(request.spec == saved.spec && request.definitionId == saved.definitionId &&
            request.largestPlan.targetBytes == saved.largestPlan.targetBytes);
    };
    for (size_t i = 0; i < fields.size(); ++i) {
        auto bad = fields;
        bad[i] = "";
        const auto result = ValidateCustomDisplayDialog(bad, limits, request);
        assert(!result.Ok() && result.field == i && result.number == Number::Empty);
        unchanged();
    }
    auto result = ValidateCustomDisplayDialog(fields, {}, request);
    assert(!result.Ok() && result.plan == CustomDisplayPlanResult::InvalidLimits);
    unchanged();
    result = ValidateCustomDisplayDialog({"17", "1", "480", "272"}, limits, request);
    assert(!result.Ok() && result.plan == CustomDisplayPlanResult::PhysicalLimit);
    unchanged();
    result = ValidateCustomDisplayDialog({"2", "1", "4097", "272"}, limits, request);
    assert(!result.Ok() && result.plan == CustomDisplayPlanResult::PixelLimit);
    unchanged();
    auto small = limits;
    small.maxTargetBytes = uint64_t(480) * 272 * 2 * 12; // Enough at x1 only.
    result = ValidateCustomDisplayDialog(fields, small, request);
    assert(!result.Ok() && result.plan == CustomDisplayPlanResult::TargetBudget);
    unchanged();
    small.maxTargetBytes = saved.largestPlan.targetBytes - 1;
    assert(!ValidateCustomDisplayDialog(fields, small, request).Ok());
    unchanged();
    small.maxTargetBytes++;
    assert(ValidateCustomDisplayDialog(fields, small, request).Ok());

    CustomDisplayDialogDraft draft;
    CustomDisplayDialogValidation validation;
    assert(!draft.Current(0) && !draft.Take(0, request) && !draft.Cancel(0));
    const auto first = draft.Open();
    assert(first && draft.Current(first) && !draft.Take(first, request));
    assert(draft.Validate(first, fields, limits, validation) && validation.Ok());
    assert(draft.Cancel(first));
    assert(!draft.Current(first) && !draft.Take(first, request));
    const auto second = draft.Open();
    assert(second != first && !draft.Cancel(first));
    assert(!draft.Validate(first, fields, limits, validation));
    assert(draft.Validate(second, fields, limits, validation) && validation.Ok());
    // A new failed validation must erase the previously valid draft.
    assert(draft.Validate(second, {"2", "1", "0", "272"}, limits, validation));
    assert(!validation.Ok() && !draft.Take(second, request));
    unchanged();
    assert(draft.Validate(second, fields, limits, validation) && validation.Ok());
    draft.Invalidate(); // Editor teardown before children are destroyed.
    assert(!draft.Take(second, request) && !draft.Cancel(second));
    auto third = draft.Open();
    assert(draft.Validate(third, fields, limits, validation) && validation.Ok());
    assert(draft.Take(third, request));
    assert(request.spec == saved.spec && request.definitionId == saved.definitionId);
    assert(!draft.Take(third, request)); // No double confirmation.
    for (int i = 0; i < 100; ++i) {
        auto old = draft.Open();
        assert(draft.Validate(old, fields, limits, validation) && validation.Ok());
        auto current = draft.Open(); // Reopening invalidates all old callbacks.
        assert(current != old && !draft.Take(old, request) && !draft.Cancel(old));
        assert(!draft.Take(current, request) && draft.Cancel(current));
    }
    puts("PASS: dialog numbers, four independent fields, worst-quality budget, cancellation, teardown and stale/double confirmation (offline only)");
}
