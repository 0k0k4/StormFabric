#include "custom_display_dialog_native.h"
#include "custom_display_engine_string.h"
#include "custom_display_registry.h"
#include <windows.h>
#include <string>

namespace swml {

bool CustomDisplayDialogNativeApi::Ready() const {
    return engine.op_new && engine.str_reserve && engine.str_free && engine.add_child &&
        makeDialog && makeEditbox && setAllowed && setOpen &&
        dialogBytes && editboxBytes;
}

namespace {
CustomDisplayDialogBuildResult Build(
        const CustomDisplayDialogNativeApi& api, void* parent, void* context,
        int32_t confirmAction, int32_t cancelAction, int32_t editAction,
        CustomDisplayDialogNativeTree& out, bool callbacks) {
    using Result = CustomDisplayDialogBuildResult;
    if (!api.Ready() || !parent || !context || out.dialog || out.complete ||
        (callbacks ? (confirmAction || cancelAction || editAction) :
         (confirmAction <= 0 || cancelAction <= 0 || editAction <= 0 ||
          confirmAction == cancelAction || confirmAction == editAction || cancelAction == editAction)))
        return Result::InvalidArguments;
    for (const auto* widget : out.editboxes) if (widget) return Result::InvalidArguments;
    CustomDisplayEngineString title(api.engine), body(api.engine);
    if (!title.Assign("Custom Display") || !body.Assign(
            "Physical size and resolution:\n\n"
            "Width (blocks)\n\n\n"
            "Height (blocks)\n\n\n"
            "Resolution (height px)\n\n\n"
            "(Width px is auto-calculated)\n\n\n"))
        return Result::StringFailed;
    void* memory = api.engine.op_new(api.dialogBytes);
    if (!memory) return Result::AllocationFailed;
    // Unlike editboxes, the native basic constructor copies but does not free
    // either input. Both RAII strings remain ours throughout this call.
    void* dialog = api.makeDialog(memory, title.Borrow(), body.Borrow(), 1, 1,
                                 confirmAction, cancelAction);
    
    // Enable background rendering on the dialog (offset 0x74 = show_bg flag, offset 0x75 = max alpha)
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(dialog) + 0x74) = 1;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(dialog) + 0x75) = 0xff;

    // Set view container bounds so the background rectangle covers the entire dialog
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(dialog) + 0x78) = 0.80f;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(dialog) + 0x7c) = -0.88f;

    // add_child calls init: the basic dialog closes itself in init. Do not init
    // a detached dialog then add_child (that would initialize it twice).
    api.engine.add_child(parent, context, dialog);
    out.dialog = dialog;

    // Re-enable background flag after init
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(dialog) + 0x74) = 1;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(dialog) + 0x75) = 0xff;

    api.setOpen(dialog, 0, 1);

    // Ensure the background rectangle at dialog + 0x58 is fully opaque and sized
    void* bgRect = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(dialog) + 0x58);
    if (bgRect) {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x08) = 0.0f;   // X
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x0c) = 0.0f;   // Y
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x10) = 0.80f;  // Width
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x14) = -0.88f; // Height
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(bgRect) + 0x58) = 0xff141618; // Solid dark grey (100% opaque)
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(bgRect) + 0x5c) = 0xff; // Full opacity
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(bgRect) + 0x1c) = 1;    // Visible
    }

    int lastW = 1, lastH = 1, lastPx = 32, lastPy = 32;
    GetLastDialogValues(lastW, lastH, lastPx, lastPy);
    if (lastW < 1) lastW = 1;
    if (lastW > 100) lastW = 100;
    if (lastH < 1) lastH = 1;
    if (lastH > 100) lastH = 100;
    if (lastPy < 1) lastPy = lastH * 32;
    if (lastPy > 8192) lastPy = 8192;
    std::string sW = std::to_string(lastW);
    std::string sH = std::to_string(lastH);
    std::string sPy = std::to_string(lastPy);
    const char* defaults[] = {sW.c_str(), sH.c_str(), sPy.c_str()};

    // 3 editboxes: Width (blocks), Height (blocks), Resolution (height px).
    // Width (pixels) is auto-calculated from physical block aspect ratio to keep pixels square.
    for (size_t i = 0; i < 3; ++i) {
        // Prepare both strings before allocating a widget; a failed preparation
        // leaves no unconstructed raw object to release with the wrong allocator.
        CustomDisplayEngineString initial(api.engine), digits(api.engine);
        if (!initial.Assign(defaults[i]) || !digits.Assign("0123456789"))
            return Result::StringFailed;
        memory = api.engine.op_new(api.editboxBytes);
        if (!memory) return Result::AllocationFailed;
        void* editbox = initial.Consume([&](sw::EStr* text) {
            return api.makeEditbox(memory, .06f, -.410f - float(i) * .114f,
                                  .28f, .050f, 0, text, editAction);
        });
        api.engine.add_child(dialog, context, editbox);
        out.editboxes[i] = editbox;
        // The live editbox child exists only AFTER add_child initialized it.
        digits.Consume([&](sw::EStr* text) { api.setAllowed(editbox, text); });
        auto setEditboxAlpha = reinterpret_cast<void (*)(void*, void*, uint8_t)>(api.engine.base + 0x24f7d0);
        if (setEditboxAlpha) {
            setEditboxAlpha(editbox, context, 0);
        } else {
            *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(editbox) + 0x1c) = false;
        }
    }
    out.editboxes[3] = nullptr;
    out.complete = true;
    return Result::Ok;
}
} // namespace

CustomDisplayDialogBuildResult BuildCustomDisplayNativeDialog(
        const CustomDisplayDialogNativeApi& api, void* parent, void* context,
        int32_t confirmAction, int32_t cancelAction, int32_t editAction,
        CustomDisplayDialogNativeTree& out) {
    return Build(api, parent, context, confirmAction, cancelAction, editAction, out, false);
}

CustomDisplayDialogBuildResult BuildCustomDisplayCallbackDialog(
        const CustomDisplayDialogNativeApi& api, void* parent, void* context,
        CustomDisplayDialogNativeTree& out) {
    return Build(api, parent, context, 0, 0, 0, out, true);
}

void UpdateCustomDisplayDialogEditboxes(const CustomDisplayDialogNativeTree& tree) {
    if (!tree.complete) return;
    int lastW = 1, lastH = 1, lastPx = 32, lastPy = 32;
    GetLastDialogValues(lastW, lastH, lastPx, lastPy);
    if (lastW < 1) lastW = 1;
    if (lastW > 100) lastW = 100;
    if (lastH < 1) lastH = 1;
    if (lastH > 100) lastH = 100;
    if (lastPy < 1) lastPy = lastH * 32;
    if (lastPy > 8192) lastPy = 8192;
    std::string vals[3] = {
        std::to_string(lastW),
        std::to_string(lastH),
        std::to_string(lastPy)
    };
    for (size_t i = 0; i < 3; ++i) {
        if (!tree.editboxes[i]) continue;
        const void* editbox = tree.editboxes[i];
        void* content = nullptr;
        SIZE_T read = 0;
        if (ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const uint8_t*>(editbox) + 0x78, &content, sizeof(content), &read) && content) {
            sw::EStr str{};
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(content) + 0xb8, &str, sizeof(str), &read) &&
                str.data && str.cap > 0) {
                size_t len = vals[i].size();
                if (len >= str.cap) len = str.cap - 1;
                SIZE_T written = 0;
                WriteProcessMemory(GetCurrentProcess(), str.data, vals[i].c_str(), len, &written);
                char nullTerm = '\0';
                WriteProcessMemory(GetCurrentProcess(), str.data + len, &nullTerm, 1, &written);
                uint32_t uLen = static_cast<uint32_t>(len);
                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(content) + 0xb8 + offsetof(sw::EStr, len), &uLen, sizeof(uLen), &written);
            }
        }
    }
}

} // namespace swml
