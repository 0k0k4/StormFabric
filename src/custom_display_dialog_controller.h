#pragma once
#include "custom_display_dialog_model.h"
#include "custom_display_dialog_native.h"

namespace swml {

enum class CustomDisplayDialogConfirmResult { Ok, Stale, TextUnavailable, InvalidValues };

// One controller per retained vehicle-owner lifetime. No owner-thread transfer,
// no placement/registration side effects and no external callbacks during close.
// The vehicle adapter must invalidate BEFORE leave/teardown, not on destruction
// alone, and observe native Escape-close before dispatching further actions.
class CustomDisplayDialogController {
public:
    using Token = CustomDisplayDialogDraft::Token;
    CustomDisplayDialogController() = default;
    CustomDisplayDialogController(const CustomDisplayDialogController&) = delete;
    CustomDisplayDialogController& operator=(const CustomDisplayDialogController&) = delete;

    Token Open(const void* owner, uint32_t thread, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogTextAccess& access, CustomDisplaySetDialogOpen setOpen);
    // Prepare while CLOSED, bind callbacks to the returned immutable token, then
    // Show only after input isolation. Prepared sessions cannot confirm/cancel.
    Token Prepare(const void* owner, uint32_t thread, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogTextAccess& access, CustomDisplaySetDialogOpen setOpen);
    bool Show(const void* owner, uint32_t thread, Token token);
    bool Current(const void* owner, uint32_t thread, Token token) const;
    CustomDisplayDialogConfirmResult Confirm(const void* owner, uint32_t thread, Token token,
        const CustomDisplayLimits& limits, CustomDisplayDialogValidation& validation,
        CustomDisplayDialogRequest& out);
    bool Cancel(const void* owner, uint32_t thread, Token token);
    bool OwnerLeaving(const void* owner, uint32_t thread);
    bool NativeClosed(const void* owner, uint32_t thread, Token token);
    bool BeforeOwnerTeardown(const void* owner, uint32_t thread);
private:
    void Invalidate();
    const void* owner_ = nullptr;
    uint32_t thread_ = 0;
    void* dialog_ = nullptr;
    CustomDisplaySetDialogOpen setOpen_ = nullptr;
    CustomDisplayDialogDraft draft_;
    CustomDisplayDialogTextSession text_;
    CustomDisplayDialogTextSession::Token textToken_ = 0;
    Token token_ = 0;
    bool retired_ = false;
    bool shown_ = false;
};

} // namespace swml
