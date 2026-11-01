#pragma once
#include "custom_display_dialog_native.h"
#include "custom_display_dialog_controller.h"

namespace swml {

enum class CustomDisplayDialogCommand { None, Confirm, Cancel };
struct CustomDisplayDialogCallbackEvent {
    uint64_t token = 0;
    CustomDisplayDialogCommand command = CustomDisplayDialogCommand::None;
};
struct CustomDisplayDialogCallbackAccess {
    CustomDisplayDialogWidgetLayout widgets;
    CustomDisplayDialogCallbackLayout buttons;
    uint32_t dialogBytes = 0;
    const void* dialogTable = nullptr;
    const void* buttonTable = nullptr;
    CustomDisplayDialogCopy copy = nullptr;
    void* copyContext = nullptr;
};
bool BindCustomDisplayDialogCallbackAccess(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogCopy copy, void* copyContext,
    CustomDisplayDialogCallbackAccess& out);

// Internal, process-retained, bounded callback arena. IDs/userdata are NEVER
// recycled, including after owner deletion/pointer reuse. No engine pointers are
// dereferenced by the thunk. Each opening needs a fresh native tree and lease.
// Exhaustion disables new dialogs rather than reusing an old callback identity.
using CustomDisplayDialogCallbackLease = uint32_t;
CustomDisplayDialogCallbackLease ReserveCustomDisplayDialogCallbacks(
    const void* owner, uint32_t thread, void* context, uint64_t token);
bool AttachCustomDisplayDialogCallbacks(CustomDisplayDialogCallbackLease lease,
    const void* owner, uint32_t thread, const CustomDisplayDialogNativeTree& tree,
    const CustomDisplayDialogCallbackAccess& access);
// Arm after binding/showing and after the opening mouse/key release barrier.
// After Take, explicit rearm is needed ONLY for failed validation of this token.
bool ArmCustomDisplayDialogCallbacks(CustomDisplayDialogCallbackLease lease,
    const void* owner, uint32_t thread);
// Local keyboard route, never an action in the native integer queue. The exact
// immutable token must match the lease; disabled/closed/stale routes stay inert.
bool QueueCustomDisplayDialogCommand(CustomDisplayDialogCallbackLease lease,
    const void* owner, uint32_t thread, uint64_t token, CustomDisplayDialogCommand command);
// Read-only prepared-opening check: same closed tree/context/token, bound but
// not armed. Does not authenticate the owner or certify a hook safe point.
bool CustomDisplayDialogCallbacksPrepared(CustomDisplayDialogCallbackLease lease,
    const void* owner, uint32_t thread, uint64_t token, const void* dialog, const void* context);
bool TakeCustomDisplayDialogCallback(CustomDisplayDialogCallbackLease lease,
    const void* owner, uint32_t thread, CustomDisplayDialogCallbackEvent& out);
// Call BEFORE controller close/leave/native Escape/refresh/teardown. No native
// writes, reads or deletes: attached trees remain owned by the native parent.
bool RetireCustomDisplayDialogCallbacks(CustomDisplayDialogCallbackLease lease,
    const void* owner, uint32_t thread);

enum class CustomDisplayDialogDispatchResult { None, Confirmed, Cancelled, Stale, InvalidValues, TextUnavailable };
// Deferred owner-thread safe point ONLY, after observing native Escape closure.
// The thunk never calls this. Take disarms BEFORE controller calls/native close;
// failed validation rearms only the same still-current token. No registration or
// placement occurs; Confirmed returns a configuration request, not a permit.
CustomDisplayDialogDispatchResult DispatchCustomDisplayDialogCallback(
    CustomDisplayDialogCallbackLease lease, const void* owner, uint32_t thread,
    CustomDisplayDialogController& controller, const CustomDisplayLimits& limits,
    CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& request);

} // namespace swml
