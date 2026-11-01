#include "custom_display_dialog_controller.h"

namespace swml {

CustomDisplayDialogController::Token CustomDisplayDialogController::Open(
        const void* owner, uint32_t thread, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogTextAccess& access, CustomDisplaySetDialogOpen setOpen) {
    const auto token = Prepare(owner, thread, tree, access, setOpen);
    return token && Show(owner, thread, token) ? token : 0;
}

CustomDisplayDialogController::Token CustomDisplayDialogController::Prepare(
        const void* owner, uint32_t thread, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogTextAccess& access, CustomDisplaySetDialogOpen setOpen) {
    if (retired_ || !owner || !thread || !tree.dialog || !tree.complete || !setOpen || token_ ||
        (owner_ && (owner_ != owner || thread_ != thread))) return 0;
    const auto textToken = text_.Bind(owner, thread, tree.dialog, tree.editboxes, access);
    if (!textToken) return 0;
    // Check all four attached child types/parents before opening.
    CustomDisplayDialogTextSnapshot snapshot;
    const auto read = text_.Read(owner, textToken, thread, snapshot);
    if (read != CustomDisplayDialogTextResult::Ok) {
        text_.Invalidate();
        return 0;
    }
    token_ = draft_.Open();
    if (!token_) { text_.Invalidate(); return 0; }
    owner_ = owner;
    thread_ = thread;
    dialog_ = tree.dialog;
    setOpen_ = setOpen;
    textToken_ = textToken;
    return token_;
}

bool CustomDisplayDialogController::Show(const void* owner, uint32_t thread, Token token) {
    if (shown_ || !token || token != token_ || owner != owner_ || !thread || thread != thread_) return false;
    shown_ = true;
    setOpen_(dialog_, 1, 1);
    return Current(owner, thread, token);
}

bool CustomDisplayDialogController::Current(const void* owner, uint32_t thread, Token token) const {
    return shown_ && owner == owner_ && thread && thread == thread_ && token && token == token_ && draft_.Current(token);
}

void CustomDisplayDialogController::Invalidate() {
    draft_.Invalidate();
    text_.Invalidate();
    textToken_ = token_ = 0;
    dialog_ = nullptr;
    setOpen_ = nullptr;
    shown_ = false;
    // Retain owner/thread identity until controller destruction; pointer reuse
    // or a different runtime thread cannot silently adopt this controller.
}

CustomDisplayDialogConfirmResult CustomDisplayDialogController::Confirm(
        const void* owner, uint32_t thread, Token token, const CustomDisplayLimits& limits,
        CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& out) {
    using Result = CustomDisplayDialogConfirmResult;
    if (!Current(owner, thread, token)) return Result::Stale;
    CustomDisplayDialogTextSnapshot snapshot;
    if (text_.Read(owner, textToken_, thread, snapshot) != CustomDisplayDialogTextResult::Ok)
        return Result::TextUnavailable;
    CustomDisplayDialogValidation checked;
    if (!draft_.Validate(token, snapshot.Fields(), limits, checked)) return Result::Stale;
    validation = checked;
    if (!checked.Ok()) return Result::InvalidValues; // Keep window open for corrections.
    CustomDisplayDialogRequest request;
    if (!draft_.Take(token, request)) return Result::Stale;
    auto close = setOpen_;
    void* dialog = dialog_;
    Invalidate(); // Reentrant/queued callbacks are stale BEFORE the native call.
    close(dialog, 0, 1);
    out = std::move(request); // Registration request only, NEVER a placement permit.
    return Result::Ok;
}

bool CustomDisplayDialogController::Cancel(const void* owner, uint32_t thread, Token token) {
    if (!Current(owner, thread, token)) return false;
    auto close = setOpen_;
    void* dialog = dialog_;
    Invalidate();
    close(dialog, 0, 1);
    return true;
}

bool CustomDisplayDialogController::OwnerLeaving(const void* owner, uint32_t thread) {
    if (!shown_ && token_ && owner == owner_ && thread && thread == thread_) {
        Invalidate();
        return true;
    }
    return Cancel(owner, thread, token_);
}

bool CustomDisplayDialogController::NativeClosed(const void* owner, uint32_t thread, Token token) {
    if (!Current(owner, thread, token)) return false;
    Invalidate(); // Escape may already have closed the basic without a cancel action.
    return true;
}

bool CustomDisplayDialogController::BeforeOwnerTeardown(const void* owner, uint32_t thread) {
    if (!owner_ || owner != owner_ || thread != thread_) return false;
    retired_ = true;
    Invalidate(); // No native call once teardown starts; the native parent owns deletion.
    return true;
}

} // namespace swml
