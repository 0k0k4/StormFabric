#include "custom_display_dialog_callbacks.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <mutex>

namespace swml {
namespace {
using Callback = void (*)(void*, void*);
struct Ticket { CustomDisplayDialogCommand command = CustomDisplayDialogCommand::None; };
struct Record {
    const void* owner = nullptr;
    void* context = nullptr;
    const void* dialog = nullptr;
    uint32_t thread = 0;
    uint64_t token = 0;
    bool live = false, bound = false, armed = false;
    CustomDisplayDialogCommand pending = CustomDisplayDialogCommand::None;
    Ticket confirm{CustomDisplayDialogCommand::Confirm}, cancel{CustomDisplayDialogCommand::Cancel};
};
// Stable storage for the entire loaded-module lifetime, no per-owner cleanup or
// allocation in a callback. As with the loader's other native callbacks, live
// DLL unloading while native trees exist is not supported.
std::array<Record, 256> records;
size_t used = 0;
std::mutex mutex;
Record* Match(CustomDisplayDialogCallbackLease lease, const void* owner, uint32_t thread) {
    if (!lease || lease > used || !owner || !thread || thread != GetCurrentThreadId()) return nullptr;
    auto& record = records[lease - 1];
    return record.live && record.owner == owner && record.thread == thread ? &record : nullptr;
}
void Click(void* context, void* data) {
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t i = 0; i < used; ++i) {
        auto& record = records[i];
        // Compare addresses before accessing anything supplied by native code.
        const auto command = data == &record.confirm ? CustomDisplayDialogCommand::Confirm :
            data == &record.cancel ? CustomDisplayDialogCommand::Cancel : CustomDisplayDialogCommand::None;
        if (command == CustomDisplayDialogCommand::None) continue;
        if (!record.live || !record.bound || !record.armed || record.context != context ||
            record.thread != GetCurrentThreadId()) return;
        // Coalesce duplicates; cancellation wins until the deferred drain. Never
        // validate, close, select, attach widgets or call the controller here.
        if (record.pending != CustomDisplayDialogCommand::Cancel) record.pending = command;
        return;
    }
}
template<class T> bool Read(const CustomDisplayDialogCallbackAccess& access,
        const void* base, uint32_t offset, T& out) {
    return access.copy(access.copyContext, base, offset, &out, sizeof(out));
}
template<class T> void Write(void* base, uint32_t offset, const T& value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}
bool LayoutReady(const CustomDisplayDialogCallbackAccess& access) {
    const auto& b = access.buttons;
    const auto& w = access.widgets;
    return access.copy && access.dialogTable && access.buttonTable && access.dialogBytes &&
        access.dialogBytes < 0x10000 && b.buttonBytes && b.buttonBytes < 0x10000 &&
        b.confirmButton && b.cancelButton == b.confirmButton + sizeof(void*) &&
        uint64_t(b.cancelButton) + sizeof(void*) <= access.dialogBytes &&
        b.callback && b.userData == b.callback + sizeof(void*) &&
        uint64_t(b.userData) + sizeof(void*) <= b.buttonBytes &&
        b.releaseAction && b.pressActions == b.releaseAction + 4 &&
        uint64_t(b.pressActions) + 8 <= b.callback && w.parent &&
        uint64_t(w.parent) + sizeof(void*) <= b.buttonBytes &&
        w.dialogOpen && w.dialogOpen < access.dialogBytes;
}
} // namespace

CustomDisplayDialogCallbackLease ReserveCustomDisplayDialogCallbacks(
        const void* owner, uint32_t thread, void* context, uint64_t token) {
    if (!owner || !context || !token || !thread || thread != GetCurrentThreadId()) return 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (used == records.size()) return 0;
    auto& record = records[used++];
    record.owner = owner; record.thread = thread; record.context = context;
    record.token = token; record.live = true;
    return static_cast<CustomDisplayDialogCallbackLease>(used);
}

bool AttachCustomDisplayDialogCallbacks(CustomDisplayDialogCallbackLease lease,
        const void* owner, uint32_t thread, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogCallbackAccess& access) {
    { // Reject wrong/stale owners before touching any engine memory.
        std::lock_guard<std::mutex> lock(mutex);
        auto* record = Match(lease, owner, thread);
        if (!record || record->bound) return false;
    }
    if (!tree.complete || !tree.dialog || !LayoutReady(access)) return false;
    const auto& b = access.buttons;
    const auto& w = access.widgets;
    const void* table = nullptr;
    uint8_t open = 1;
    void* confirm = nullptr;
    void* cancel = nullptr;
    if (!Read(access, tree.dialog, 0, table) || table != access.dialogTable ||
        !Read(access, tree.dialog, w.dialogOpen, open) || open ||
        !Read(access, tree.dialog, b.confirmButton, confirm) || !confirm ||
        !Read(access, tree.dialog, b.cancelButton, cancel) || !cancel || confirm == cancel) return false;
    for (const auto* button : {confirm, cancel}) {
        const void* parent = nullptr;
        Callback callback = nullptr;
        void* data = nullptr;
        int32_t release = -1;
        uint64_t press = UINT64_MAX;
        if (!Read(access, button, 0, table) || table != access.buttonTable ||
            !Read(access, button, w.parent, parent) || parent != tree.dialog ||
            !Read(access, button, b.releaseAction, release) || release ||
            !Read(access, button, b.pressActions, press) || press ||
            !Read(access, button, b.callback, callback) || callback ||
            !Read(access, button, b.userData, data) || data) return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto* record = Match(lease, owner, thread);
    if (!record || record->bound) return false;
    // Certified owner thread, closed tree, outside traversal. All reads completed
    // BEFORE any writes; no fallible/external/native callback between these stores.
    Write(confirm, b.userData, static_cast<void*>(&record->confirm));
    Write(cancel, b.userData, static_cast<void*>(&record->cancel));
    Write(confirm, b.callback, Callback(&Click));
    Write(cancel, b.callback, Callback(&Click));
    record->dialog = tree.dialog;
    record->bound = true;
    return true;
}

bool ArmCustomDisplayDialogCallbacks(CustomDisplayDialogCallbackLease lease,
        const void* owner, uint32_t thread) {
    std::lock_guard<std::mutex> lock(mutex);
    auto* record = Match(lease, owner, thread);
    if (!record || !record->bound || record->armed || record->pending != CustomDisplayDialogCommand::None) return false;
    record->armed = true;
    return true;
}

bool TakeCustomDisplayDialogCallback(CustomDisplayDialogCallbackLease lease,
        const void* owner, uint32_t thread, CustomDisplayDialogCallbackEvent& out) {
    std::lock_guard<std::mutex> lock(mutex);
    auto* record = Match(lease, owner, thread);
    if (!record || !record->armed || record->pending == CustomDisplayDialogCommand::None) return false;
    out = {record->token, record->pending};
    record->pending = CustomDisplayDialogCommand::None;
    record->armed = false;
    return true;
}

bool QueueCustomDisplayDialogCommand(CustomDisplayDialogCallbackLease lease,
        const void* owner, uint32_t thread, uint64_t token, CustomDisplayDialogCommand command) {
    if (command != CustomDisplayDialogCommand::Confirm && command != CustomDisplayDialogCommand::Cancel) return false;
    std::lock_guard<std::mutex> lock(mutex);
    auto* record = Match(lease, owner, thread);
    if (!record || !record->bound || !record->armed || !token || token != record->token) return false;
    if (record->pending != CustomDisplayDialogCommand::Cancel) record->pending = command;
    return true;
}

bool CustomDisplayDialogCallbacksPrepared(CustomDisplayDialogCallbackLease lease,
        const void* owner, uint32_t thread, uint64_t token, const void* dialog, const void* context) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto* record = Match(lease, owner, thread);
    return record && record->bound && !record->armed && token && token == record->token &&
        dialog && dialog == record->dialog && context && context == record->context &&
        record->pending == CustomDisplayDialogCommand::None;
}

bool RetireCustomDisplayDialogCallbacks(CustomDisplayDialogCallbackLease lease,
        const void* owner, uint32_t thread) {
    std::lock_guard<std::mutex> lock(mutex);
    auto* record = Match(lease, owner, thread);
    if (!record) return false;
    record->live = record->armed = false;
    record->pending = CustomDisplayDialogCommand::None;
    return true;
}

CustomDisplayDialogDispatchResult DispatchCustomDisplayDialogCallback(
        CustomDisplayDialogCallbackLease lease, const void* owner, uint32_t thread,
        CustomDisplayDialogController& controller, const CustomDisplayLimits& limits,
        CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& request) {
    using Result = CustomDisplayDialogDispatchResult;
    CustomDisplayDialogCallbackEvent event;
    if (!TakeCustomDisplayDialogCallback(lease, owner, thread, event)) return Result::None;
    if (!controller.Current(owner, thread, event.token)) {
        RetireCustomDisplayDialogCallbacks(lease, owner, thread);
        return Result::Stale;
    }
    if (event.command == CustomDisplayDialogCommand::Cancel) {
        RetireCustomDisplayDialogCallbacks(lease, owner, thread);
        return controller.Cancel(owner, thread, event.token) ? Result::Cancelled : Result::Stale;
    }
    const auto confirmed = controller.Confirm(owner, thread, event.token, limits, validation, request);
    if (confirmed == CustomDisplayDialogConfirmResult::Ok) {
        RetireCustomDisplayDialogCallbacks(lease, owner, thread);
        return Result::Confirmed;
    }
    if (confirmed == CustomDisplayDialogConfirmResult::Stale ||
        !controller.Current(owner, thread, event.token)) {
        RetireCustomDisplayDialogCallbacks(lease, owner, thread);
        return Result::Stale;
    }
    if (!ArmCustomDisplayDialogCallbacks(lease, owner, thread)) {
        // A reentrant owner invalidation must not be undone by retry handling.
        return Result::Stale;
    }
    return confirmed == CustomDisplayDialogConfirmResult::InvalidValues ? Result::InvalidValues : Result::TextUnavailable;
}

} // namespace swml
