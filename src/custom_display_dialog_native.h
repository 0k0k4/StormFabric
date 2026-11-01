#pragma once
#include "custom_display_dialog_text.h"
#include "game.h"

namespace swml {

// Internal construction adapter, deliberately not installed by the loader.
// Action IDs must be reserved/routed by the owning vehicle adapter, not chosen
// from unused-looking stock values. Function bindings/sizes require discovery.
using CustomDisplayMakeBasicDialog = void* (*)(void*, sw::EStr*, sw::EStr*,
    uint8_t, uint8_t, int32_t, int32_t);
using CustomDisplayMakeEditbox = void* (*)(void*, float, float, float, float,
    int32_t, sw::EStr*, int32_t);
using CustomDisplaySetAllowed = void (*)(void*, sw::EStr*);
using CustomDisplaySetDialogOpen = void (*)(void*, uint8_t, uint8_t);

struct CustomDisplayDialogNativeApi {
    sw::Api engine;
    CustomDisplayMakeBasicDialog makeDialog = nullptr;
    CustomDisplayMakeEditbox makeEditbox = nullptr;
    CustomDisplaySetAllowed setAllowed = nullptr;
    CustomDisplaySetDialogOpen setOpen = nullptr;
    uint32_t dialogBytes = 0, editboxBytes = 0;
    bool Ready() const;
};

// Read-only binding for a mapped image. This is NOT an activation permit: the
// production owner must separately authenticate the executable and certify its
// thread, action routing and input isolation before calling the constructor.
bool BindCustomDisplayDialogNativeApi(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogNativeApi& out);

struct CustomDisplayDialogNativeTree {
    void* dialog = nullptr;
    std::array<const void*, 4> editboxes{};
    bool complete = false;
};
enum class CustomDisplayDialogBuildResult { Ok, InvalidArguments, AllocationFailed, StringFailed };

// Must run on the certified owner's runtime thread outside native traversal.
// Native add_child initializes and OWNS each child. Once attached, even a partial
// failed build stays CLOSED under its native parent until normal parent teardown.
// No remove_child rollback: it only clears exact focus pointers, not descendants.
// Do not retry a failed build with the same output or delete its children manually.
CustomDisplayDialogBuildResult BuildCustomDisplayNativeDialog(
    const CustomDisplayDialogNativeApi& api, void* parent, void* context,
    int32_t confirmAction, int32_t cancelAction, int32_t editAction,
    CustomDisplayDialogNativeTree& out);

// Closed callback-only variant: no invented action IDs. MUST bind immutable
// native callbacks and isolate vehicle input before showing. In particular the
// stock text handler queues even action ZERO on Enter/keypad Enter; those submit
// keys must be intercepted locally, not forwarded or treated as harmless IDs.
CustomDisplayDialogBuildResult BuildCustomDisplayCallbackDialog(
    const CustomDisplayDialogNativeApi& api, void* parent, void* context,
    CustomDisplayDialogNativeTree& out);

void UpdateCustomDisplayDialogEditboxes(const CustomDisplayDialogNativeTree& tree);

} // namespace swml
