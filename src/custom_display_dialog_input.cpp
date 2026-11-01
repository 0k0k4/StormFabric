#include "custom_display_dialog_input.h"
#include <windows.h>
#include <cmath>

namespace swml {
namespace {
struct Busy {
    bool& value;
    explicit Busy(bool& flag) : value(flag) { value = true; }
    ~Busy() { value = false; }
};
void* At(const void* base, uint32_t offset) {
    const auto value = reinterpret_cast<uintptr_t>(base);
    return value && offset <= UINTPTR_MAX - value ? reinterpret_cast<void*>(value + offset) : nullptr;
}
}

bool CustomDisplayHeldInput::Neutral() const {
    if (!complete || mouseButtons) return false;
    for (auto word : keys) if (word) return false;
    return true;
}
bool CustomDisplayNativeHeldInputReader::Copy(const void* base, size_t offset, void* out, size_t bytes) const {
    const auto address = reinterpret_cast<uintptr_t>(base);
    return copy_ && address && offset <= UINTPTR_MAX - address && bytes <= UINTPTR_MAX - (address + offset) &&
        copy_(copyContext_, base, offset, out, bytes);
}
bool CustomDisplayNativeHeldInputReader::Snapshot(CustomDisplayHeldInput& out) const {
    std::array<uint8_t, 512> keys{}, keysAgain{};
    std::array<uint8_t, 8> mouse{}, mouseAgain{};
    const void* window = nullptr;
    if (!Copy(host_, layout_.hostWindow, &window, sizeof(window)) || window != window_ ||
        !Copy(window_, layout_.keyStates, keys.data(), layout_.keyCount) ||
        !Copy(window_, layout_.mouseStates, mouse.data(), layout_.mouseCount) ||
        !Copy(window_, layout_.keyStates, keysAgain.data(), layout_.keyCount) ||
        !Copy(window_, layout_.mouseStates, mouseAgain.data(), layout_.mouseCount) ||
        keys != keysAgain || mouse != mouseAgain ||
        !Copy(host_, layout_.hostWindow, &window, sizeof(window)) || window != window_) return false;
    CustomDisplayHeldInput snapshot;
    for (uint32_t i = 0; i < layout_.keyCount; ++i) {
        // Native stores 0 (released), 1 (held), 3 (sticky release pending).
        // Conservatively keep sticky states held; never mutate them to claim
        // neutrality. Unknown/transient values cannot yield a complete sample.
        if (keys[i] != 0 && keys[i] != 1 && keys[i] != 3) return false;
        if (keys[i]) snapshot.keys[i / 64] |= uint64_t(1) << (i % 64);
    }
    for (uint32_t i = 0; i < layout_.mouseCount; ++i) {
        if (mouse[i] != 0 && mouse[i] != 1 && mouse[i] != 3) return false;
        if (mouse[i]) snapshot.mouseButtons |= uint8_t(1u << i);
    }
    snapshot.complete = true;
    out = snapshot;
    return true;
}
bool CustomDisplayNativeHeldInputReader::Bind(const void* owner, uint32_t thread, const void* windowHost,
        const CustomDisplayNativeInputStateApi& api, CustomDisplayDialogCopy copy, void* copyContext) {
    const auto& l = api.layout;
    if (owner_ || retired_ || busy_ || !owner || !windowHost || !thread || thread != GetCurrentThreadId() || !copy ||
        !l.hostWindow || l.hostWindow >= 0x10000 || l.hostWindow % alignof(void*) ||
        !l.keyStates || !l.keyCount || l.keyCount > 512 || l.keyStates >= 0x10000 ||
        l.keyCount > 0x10000 - l.keyStates || !l.mouseStates || !l.mouseCount || l.mouseCount > 8 ||
        l.mouseStates >= 0x10000 || l.mouseCount > l.keyStates || l.mouseStates > l.keyStates - l.mouseCount) return false;
    Busy busy(busy_);
    host_ = windowHost; layout_ = l; copy_ = copy; copyContext_ = copyContext;
    CustomDisplayHeldInput initial;
    if (!Copy(host_, l.hostWindow, &window_, sizeof(window_)) || !window_ || !Snapshot(initial)) {
        host_ = window_ = nullptr; layout_ = {}; copy_ = nullptr; copyContext_ = nullptr;
        return false;
    }
    owner_ = owner; thread_ = thread;
    return true;
}
bool CustomDisplayNativeHeldInputReader::Read(const void* owner, uint32_t thread, CustomDisplayHeldInput& out) {
    out = {}; // A failed read must erase an old successful neutral sample.
    if (!owner_ || retired_ || busy_ || owner != owner_ || !thread || thread != thread_ || thread != GetCurrentThreadId()) return false;
    Busy busy(busy_);
    return Snapshot(out);
}
bool CustomDisplayNativeHeldInputReader::BeforeOwnerTeardown(const void* owner, uint32_t thread) {
    if (!owner_ || retired_ || busy_ || owner != owner_ || !thread || thread != thread_ || thread != GetCurrentThreadId()) return false;
    retired_ = true; host_ = window_ = nullptr; copy_ = nullptr; copyContext_ = nullptr;
    return true;
}
bool CustomDisplayDialogInputSession::Matches(const void* owner, uint32_t thread, uint64_t token) const {
    return phase_ != Phase::Unbound && phase_ != Phase::Retired && owner == owner_ &&
        thread && thread == thread_ && thread == GetCurrentThreadId() && token && token == token_;
}
bool CustomDisplayDialogInputSession::Copy(const void* base, size_t offset, void* out, size_t bytes) const {
    const auto address = reinterpret_cast<uintptr_t>(base);
    return address && offset <= UINTPTR_MAX - address && bytes <= UINTPTR_MAX - (address + offset) &&
        access_.copy(access_.context, base, offset, out, bytes);
}
bool CustomDisplayDialogInputSession::Content(const void* candidate) const {
    if (!candidate) return false;
    for (const auto* box : boxes_) {
        const void* table = nullptr, *parent = nullptr, *content = nullptr;
        if (!Copy(box, 0, &table, sizeof(table)) || table != access_.editboxTable ||
            !Copy(box, access_.layout.parent, &parent, sizeof(parent)) || parent != dialog_ ||
            !Copy(box, access_.layout.editboxContent, &content, sizeof(content))) return false;
        if (content != candidate) continue;
        return Copy(candidate, 0, &table, sizeof(table)) && table == access_.contentTable &&
            Copy(candidate, access_.layout.parent, &parent, sizeof(parent)) && parent == box;
    }
    return false;
}
bool CustomDisplayDialogInputSession::Focus(void*& out) const {
    return Copy(context_, api_.layout.contextFocus, &out, sizeof(out));
}
bool CustomDisplayDialogInputSession::Descendant(const void* candidate, const void* parent) const {
    // Bound both traversal length and cycles before letting native helpers walk.
    std::array<const void*, 32> seen{};
    for (size_t i = 0; candidate && i < seen.size(); ++i) {
        if (candidate == parent) return true;
        for (size_t j = 0; j < i; ++j) if (candidate == seen[j]) return false;
        seen[i] = candidate;
        if (!Copy(candidate, access_.layout.parent, &candidate, sizeof(candidate))) return false;
    }
    return false;
}

bool CustomDisplayDialogInputSession::Begin(const void* owner, uint32_t thread, uint64_t token,
        CustomDisplayDialogCallbackLease lease, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogInputApi& api, const CustomDisplayDialogTextAccess& access,
        const CustomDisplayHeldInput& held) {
    const auto& l = api.layout;
    if (phase_ != Phase::Unbound || !owner || !thread || thread != GetCurrentThreadId() || !token || !lease ||
        !held.Neutral() || !tree.complete || !tree.dialog || !api.dialogTable ||
        !api.character || !api.key || !api.mouse || !api.pointer || !api.controls ||
        !l.stateRoot || !l.stateContext || !l.stateQueue || !l.contextFocus ||
        l.stateRoot >= 0x10000 || l.stateContext >= 0x10000 || l.stateQueue >= 0x10000 ||
        l.contextFocus >= 0x10000 || !l.escapeKind || !l.escapeCode ||
        l.escapeKind >= 0x10000 || l.escapeCode >= 0x10000 || !l.submitKey ||
        l.submitKey >= 256 || !l.keypadSubmitKey || l.keypadSubmitKey >= 256 ||
        l.submitKey == l.keypadSubmitKey || !l.rootHover || l.rootHover >= 0x10000 ||
        !l.rootPointerHeld || l.rootPointerHeld >= 0x10000) return false;
    CustomDisplayDialogTextSession text;
    CustomDisplayDialogTextSnapshot snapshot;
    auto textToken = text.Bind(owner, thread, tree.dialog, tree.editboxes, access);
    if (!textToken || text.Read(owner, textToken, thread, snapshot) != CustomDisplayDialogTextResult::Ok) return false;
    // Temporarily bind read access only; no engine calls/writes during Begin.
    access_ = access; api_ = api; boxes_ = tree.editboxes;
    dialog_ = tree.dialog; root_ = At(owner, l.stateRoot);
    context_ = At(owner, l.stateContext); queue_ = At(owner, l.stateQueue);
    const void* table = nullptr;
    uint8_t open = 1;
    uint8_t pointerHeld = 1;
    void* focus = nullptr;
    const void* hover = nullptr;
    if (!root_ || !context_ || !queue_ || !access.layout.dialogOpen || !access.layout.contextModal ||
        !Copy(dialog_, 0, &table, sizeof(table)) || table != api.dialogTable ||
        !Copy(dialog_, access.layout.dialogOpen, &open, sizeof(open)) || open ||
        !Copy(root_, l.rootPointerHeld, &pointerHeld, sizeof(pointerHeld)) || pointerHeld ||
        !Focus(focus) || (focus && !Descendant(focus, dialog_)) ||
        !Copy(root_, l.rootHover, &hover, sizeof(hover)) || (hover && !Descendant(hover, dialog_)) ||
        !Descendant(dialog_, root_) ||
        !CustomDisplayDialogCallbacksPrepared(lease, owner, thread, token, dialog_, context_)) {
        access_ = {}; api_ = {}; boxes_ = {}; dialog_ = root_ = context_ = queue_ = nullptr;
        return false;
    }
    owner_ = owner; thread_ = thread; token_ = token; lease_ = lease;
    phase_ = Phase::Opening;
    return true;
}

bool CustomDisplayDialogInputSession::Activate(const void* owner, uint32_t thread, uint64_t token,
        const CustomDisplayHeldInput& held) {
    if (!Matches(owner, thread, token) || busy_ || phase_ != Phase::Opening || !held.Neutral()) return false;
    const void* table = nullptr, *modal = nullptr, *hover = nullptr;
    void* focus = nullptr;
    uint8_t open = 0, pointerHeld = 1;
    if (!Copy(dialog_, 0, &table, sizeof(table)) || table != api_.dialogTable ||
        !Copy(dialog_, access_.layout.dialogOpen, &open, sizeof(open)) || !open ||
        !Copy(context_, access_.layout.contextModal, &modal, sizeof(modal)) || modal != dialog_ ||
        !Copy(root_, api_.layout.rootPointerHeld, &pointerHeld, sizeof(pointerHeld)) || pointerHeld ||
        !Focus(focus) || (focus && !Descendant(focus, dialog_)) ||
        !Copy(root_, api_.layout.rootHover, &hover, sizeof(hover)) || (hover && !Descendant(hover, dialog_)) ||
        !CustomDisplayDialogCallbacksPrepared(lease_, owner, thread, token, dialog_, context_) ||
        !ArmCustomDisplayDialogCallbacks(lease_, owner, thread)) return false;
    phase_ = Phase::Active;
    return true;
}

CustomDisplayInputResult CustomDisplayDialogInputSession::Consume(const void* owner, uint32_t thread, uint64_t token) const {
    return Matches(owner, thread, token) ? CustomDisplayInputResult::Consumed : CustomDisplayInputResult::PassThrough;
}
CustomDisplayInputResult CustomDisplayDialogInputSession::Fault() {
    phase_ = Phase::Faulted;
    RetireCustomDisplayDialogCallbacks(lease_, owner_, thread_);
    return CustomDisplayInputResult::Fault; // Never fall through to vehicle handlers.
}
CustomDisplayInputResult CustomDisplayDialogInputSession::Guard(const void* owner, uint32_t thread, uint64_t token) {
    if (!Matches(owner, thread, token)) return CustomDisplayInputResult::PassThrough;
    if (phase_ != Phase::Active || busy_) return CustomDisplayInputResult::Consumed;
    const void* table = nullptr, *modal = nullptr;
    uint8_t open = 0;
    if (!Copy(dialog_, 0, &table, sizeof(table)) || table != api_.dialogTable ||
        !Copy(dialog_, access_.layout.dialogOpen, &open, sizeof(open))) return Fault();
    if (!open) {
        BeforeClose(owner, thread, token);
        return CustomDisplayInputResult::NativeClosed;
    }
    if (!Copy(context_, access_.layout.contextModal, &modal, sizeof(modal)) || modal != dialog_) return Fault();
    return CustomDisplayInputResult::Consumed;
}

CustomDisplayInputResult CustomDisplayDialogInputSession::Character(const void* owner, uint32_t thread, uint64_t token, uint8_t value) {
    auto result = Guard(owner, thread, token);
    if (result != CustomDisplayInputResult::Consumed || phase_ != Phase::Active || busy_) return result;
    void* focus = nullptr;
    if (!Focus(focus)) return Fault();
    if (focus && Content(focus)) {
        Busy busy(busy_);
        api_.character(focus, context_, queue_, value); // Native digit filter, not wrapper's initial string.
    }
    return result;
}
CustomDisplayInputResult CustomDisplayDialogInputSession::Key(const void* owner, uint32_t thread, uint64_t token,
        void* application, uint32_t key, uint32_t phase) {
    auto result = Guard(owner, thread, token);
    if (result != CustomDisplayInputResult::Consumed || phase_ != Phase::Active || busy_) return result;
    if (key >= keysDown_.size() || phase > 1) return result; // Explicit bounded policy, no guessed native enum.
    const bool firstPress = phase == 1 && !keysDown_[key];
    keysDown_[key] = phase == 1;
    if (key == api_.layout.submitKey || key == api_.layout.keypadSubmitKey) {
        if (firstPress) QueueCustomDisplayDialogCommand(lease_, owner, thread, token, CustomDisplayDialogCommand::Confirm);
        return result; // NEVER let native text handler queue action zero on Enter.
    }
    if (!application) return Fault();
    void* focus = nullptr;
    if (!Focus(focus)) return Fault();
    if (!phase) {
        // Release the original recipient even if focus changed while a modifier
        // was held. Closed trees are never reused for a later opening.
        auto* previous = keyRecipients_[key]; keyRecipients_[key] = nullptr;
        Busy busy(busy_);
        if (previous && Content(previous)) api_.key(previous, application, context_, queue_, key, 0);
        if (focus && focus != previous && Content(focus)) api_.key(focus, application, context_, queue_, key, 0);
    } else if (focus && Content(focus)) {
        if (firstPress) keyRecipients_[key] = focus;
        if (keyRecipients_[key] != focus) return result; // No held-key repeat into another field.
        Busy busy(busy_);
        api_.key(focus, application, context_, queue_, key, 1);
    }
    return result;
}
CustomDisplayInputResult CustomDisplayDialogInputSession::Command(const void* owner, uint32_t thread, uint64_t token,
        uint32_t kind, uint32_t code, uint32_t phase) {
    auto result = Guard(owner, thread, token);
    if (result != CustomDisplayInputResult::Consumed || phase_ != Phase::Active || busy_ || phase != 1) return result;
    Busy busy(busy_);
    void* controls = api_.controls();
    uint32_t escapeKind = 0, escapeCode = 0;
    if (!Copy(controls, api_.layout.escapeKind, &escapeKind, sizeof(escapeKind)) ||
        !Copy(controls, api_.layout.escapeCode, &escapeCode, sizeof(escapeCode))) return Fault();
    if (kind == escapeKind && code == escapeCode)
        QueueCustomDisplayDialogCommand(lease_, owner, thread, token, CustomDisplayDialogCommand::Cancel);
    return result; // Consume even when stock basic would close and return false.
}

CustomDisplayInputResult CustomDisplayDialogInputSession::Mouse(const void* owner, uint32_t thread, uint64_t token,
        void* application, void* nativeInput, const float* point, int32_t button, int32_t phase) {
    auto result = Guard(owner, thread, token);
    if (result != CustomDisplayInputResult::Consumed || phase_ != Phase::Active || busy_) return result;
    std::array<float, 2> position{};
    void* focus = nullptr;
    if (!application || button < 0 || button >= 8 || phase < 0 || phase > 1 ||
        !Copy(point, 0, position.data(), sizeof(position)) || !std::isfinite(position[0]) || !std::isfinite(position[1]) ||
        !Focus(focus)) return Fault();
    // Native root calls its retained recipient BEFORE hit-testing, even for a
    // new press. Never deliver either phase to an old catalogue/stock widget.
    if (focus && !Descendant(focus, dialog_)) return result;
    Busy busy(busy_);
    api_.mouse(root_, application, nativeInput, position.data(), button, phase);
    return result; // Ignore false: no placement fallback behind modal background.
}
CustomDisplayInputResult CustomDisplayDialogInputSession::Pointer(const void* owner, uint32_t thread, uint64_t token,
        void* application, const float* point, float dx, float dy, int32_t flags) {
    auto result = Guard(owner, thread, token);
    if (result != CustomDisplayInputResult::Consumed || phase_ != Phase::Active || busy_) return result;
    std::array<float, 2> position{};
    uint8_t held = 0;
    const void* hover = nullptr;
    if (!application || !Copy(point, 0, position.data(), sizeof(position)) ||
        !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(dx) || !std::isfinite(dy) ||
        !Copy(root_, api_.layout.rootPointerHeld, &held, sizeof(held)) ||
        !Copy(root_, api_.layout.rootHover, &hover, sizeof(hover))) return Fault();
    // The root may invoke hover-leave on an old recipient even without a held
    // pointer. No callback into a stock catalogue widget is allowed here.
    if (hover && !Descendant(hover, dialog_)) return Fault();
    Busy busy(busy_);
    // Root's third argument is scratch coordinate bits, not an owned pointer;
    // the retained body clears its low half before walking native parents.
    api_.pointer(root_, application, 0, position.data(), dx, dy, flags);
    return result;
}

CustomDisplayDialogDispatchResult CustomDisplayDialogInputSession::Drain(const void* owner, uint32_t thread,
        uint64_t token, CustomDisplayDialogController& controller, const CustomDisplayLimits& limits,
        CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& request) {
    using R = CustomDisplayDialogDispatchResult;
    if (!Matches(owner, thread, token) || busy_ || phase_ != Phase::Active) return R::None;
    const auto guarded = Guard(owner, thread, token);
    if (guarded == CustomDisplayInputResult::NativeClosed)
        return controller.NativeClosed(owner, thread, token) ? R::Cancelled : R::Stale;
    if (guarded != CustomDisplayInputResult::Consumed) return R::Stale;
    // Block ALL routed native calls while the controller can invalidate/close.
    // No external callback or hook installation is used for this transition.
    phase_ = Phase::Draining;
    Busy busy(busy_);
    const auto result = DispatchCustomDisplayDialogCallback(lease_, owner, thread, controller, limits, validation, request);
    if ((result == R::None || result == R::InvalidValues || result == R::TextUnavailable) &&
        controller.Current(owner, thread, token)) {
        phase_ = Phase::Active;
    } else {
        RetireCustomDisplayDialogCallbacks(lease_, owner, thread);
        keyRecipients_ = {};
    }
    return result;
}

bool CustomDisplayDialogInputSession::BeforeClose(const void* owner, uint32_t thread, uint64_t token) {
    if (!Matches(owner, thread, token) || busy_ || phase_ == Phase::Faulted) return false;
    RetireCustomDisplayDialogCallbacks(lease_, owner_, thread_);
    phase_ = Phase::Draining;
    keyRecipients_ = {};
    return true;
}
CustomDisplayDialogDispatchResult CustomDisplayDialogInputSession::Advance(const void* owner, uint32_t thread,
        uint64_t token, CustomDisplayNativeHeldInputReader& source, const CustomDisplayNativeInputStateApi& native,
        CustomDisplayDialogController& controller, const CustomDisplayLimits& limits,
        CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& request) {
    using R = CustomDisplayDialogDispatchResult;
    if (!Matches(owner, thread, token) || busy_ || phase_ == Phase::Faulted) return R::None;
    CustomDisplayHeldInput held;
    {
        Busy busy(busy_);
        if (!source.Read(owner, thread, held)) return R::None;
    }
    if (phase_ == Phase::Opening) {
        Activate(owner, thread, token, held);
        return R::None; // No command drain in the same publication/arming transition.
    }
    const auto result = Drain(owner, thread, token, controller, limits, validation, request);
    if (phase_ == Phase::Draining && RetireRecipients(owner, thread, token, native, held)) {
        // Native close/retirement callbacks may have changed window input state.
        // Never reuse the pre-callback sample to release capture.
        CustomDisplayHeldInput released;
        {
            Busy busy(busy_);
            if (!source.Read(owner, thread, released)) return result;
        }
        Release(owner, thread, token, released);
    }
    return result;
}
bool CustomDisplayDialogInputSession::Release(const void* owner, uint32_t thread, uint64_t token, const CustomDisplayHeldInput& held) {
    if (!Matches(owner, thread, token) || busy_ || phase_ != Phase::Draining || !held.Neutral()) return false;
    const void* table = nullptr, *modal = nullptr, *hover = nullptr;
    void* focus = nullptr;
    uint8_t open = 1, pointerHeld = 1;
    if (!Copy(dialog_, 0, &table, sizeof(table)) || table != api_.dialogTable ||
        !Copy(dialog_, access_.layout.dialogOpen, &open, sizeof(open)) || open ||
        !Copy(context_, access_.layout.contextModal, &modal, sizeof(modal)) || modal == dialog_ ||
        !Copy(root_, api_.layout.rootPointerHeld, &pointerHeld, sizeof(pointerHeld)) || pointerHeld ||
        !Focus(focus) || focus ||
        !Copy(root_, api_.layout.rootHover, &hover, sizeof(hover)) || hover) return false;
    phase_ = Phase::Retired; // One opening only; native text/modifier state is never reused.
    return true;
}
bool CustomDisplayDialogInputSession::RetireRecipients(const void* owner, uint32_t thread, uint64_t token,
        const CustomDisplayNativeInputStateApi& native, const CustomDisplayHeldInput& held) {
    const auto& l = native.layout;
    if (!Matches(owner, thread, token) || busy_ || phase_ != Phase::Draining || !held.Neutral() ||
        !native.retireRecipients || l.rootHover != api_.layout.rootHover ||
        l.rootPointerHeld != api_.layout.rootPointerHeld ||
        l.rootContext != access_.layout.rootContext ||
        uint64_t(l.rootContext) + api_.layout.contextFocus != l.rootRecipient) return false;
    const void* table = nullptr, *modal = nullptr, *hover = nullptr;
    void* focus = nullptr;
    uint8_t open = 1, pointerHeld = 1;
    if (!Copy(dialog_, 0, &table, sizeof(table)) || table != api_.dialogTable ||
        !Copy(dialog_, access_.layout.dialogOpen, &open, sizeof(open)) ||
        !Copy(context_, access_.layout.contextModal, &modal, sizeof(modal)) ||
        !Copy(root_, api_.layout.rootPointerHeld, &pointerHeld, sizeof(pointerHeld)) ||
        !Focus(focus) || !Copy(root_, api_.layout.rootHover, &hover, sizeof(hover))) {
        Fault(); return false;
    }
    if (open || modal == dialog_ || pointerHeld) return false; // Wait; never reset a native latch.
    if ((focus && !Descendant(focus, dialog_)) || (hover && !Descendant(hover, dialog_))) return false;
    Busy busy(busy_); // Native notifications can reenter; keep every input boundary captured.
    if (focus || hover) native.retireRecipients(root_);
    if (!Focus(focus) || focus || !Copy(root_, api_.layout.rootHover, &hover, sizeof(hover)) || hover ||
        !Copy(root_, api_.layout.rootPointerHeld, &pointerHeld, sizeof(pointerHeld)) || pointerHeld) {
        Fault(); return false;
    }
    return true;
}
bool CustomDisplayDialogInputSession::BeforeOwnerTeardown(const void* owner, uint32_t thread) {
    if (!Matches(owner, thread, token_) || busy_) return false;
    RetireCustomDisplayDialogCallbacks(lease_, owner_, thread_);
    phase_ = Phase::Retired;
    return true; // No read/write/native call against a potentially dismantling UI.
}

} // namespace swml
