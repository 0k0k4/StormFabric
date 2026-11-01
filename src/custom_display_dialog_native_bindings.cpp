#include "custom_display_dialog_native.h"
#include "custom_display_dialog_callbacks.h"
#include "custom_display_dialog_input.h"

namespace swml {

bool BindCustomDisplayNativeInputStateApi(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayNativeInputStateApi& out) {
    CustomDisplayNativeInputStateApi api;
    if (!DiscoverCustomDisplayNativeInputStateLayout(image, result, api.layout)) return false;
    api.retireRecipients = reinterpret_cast<CustomDisplayRetireNativeRecipients>(image.base + result.Address(0x1e2e20));
    out = api;
    return true;
}

bool BindCustomDisplayDialogInputApi(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogInputApi& out) {
    CustomDisplayDialogInputApi api;
    if (!DiscoverCustomDisplayDialogInputLayout(image, result, api.layout)) return false;
    auto address = [&](uint32_t key) { return image.base + result.Address(key); };
    api.dialogTable = address(0xb0b5c8);
    api.character = reinterpret_cast<CustomDisplayNativeCharacter>(address(0x24e5c0));
    api.key = reinterpret_cast<CustomDisplayNativeKey>(address(0x24e2e0));
    api.mouse = reinterpret_cast<CustomDisplayNativeMouse>(address(0x1e2d40));
    api.pointer = reinterpret_cast<CustomDisplayNativePointer>(address(0x1e3180));
    api.controls = reinterpret_cast<CustomDisplayGetControls>(address(0x21a200));
    out = api;
    return true;
}

bool BindCustomDisplayDialogCallbackAccess(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogCopy copy, void* copyContext,
        CustomDisplayDialogCallbackAccess& out) {
    if (!copy) return false;
    CustomDisplayDialogConstructionLayout construction;
    CustomDisplayDialogCallbackAccess access;
    if (!DiscoverCustomDisplayDialogConstructionLayout(image, result, construction) ||
        !DiscoverCustomDisplayDialogCallbackLayout(image, result, access.buttons)) return false;
    access.widgets = construction.widgets;
    access.dialogBytes = construction.dialogBytes;
    access.dialogTable = image.base + result.Address(0xb0b5c8);
    access.buttonTable = image.base + result.Address(0xbace08);
    access.copy = copy; access.copyContext = copyContext;
    out = access;
    return true;
}

bool BindCustomDisplayDialogNativeApi(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogNativeApi& out) {
    CustomDisplayDialogConstructionLayout layout;
    if (!DiscoverCustomDisplayDialogConstructionLayout(image, result, layout)) return false;
    CustomDisplayDialogNativeApi api;
    api.engine.base = const_cast<uint8_t*>(image.base);
    auto address = [&](uint32_t key) { return image.base + result.Address(key); };
    api.engine.op_new = reinterpret_cast<sw::fn_new_t>(address(0x957478));
    api.engine.str_reserve = reinterpret_cast<sw::fn_str_reserve_t>(address(0x17cb00));
    api.engine.str_free = reinterpret_cast<sw::fn_str_free_t>(address(0x9b15a0));
    api.engine.add_child = reinterpret_cast<sw::fn_add_child_t>(address(0x8f5680));
    api.makeDialog = reinterpret_cast<CustomDisplayMakeBasicDialog>(address(0x25a020));
    api.makeEditbox = reinterpret_cast<CustomDisplayMakeEditbox>(address(0x24f560));
    api.setAllowed = reinterpret_cast<CustomDisplaySetAllowed>(address(0x24f8a0));
    api.setOpen = reinterpret_cast<CustomDisplaySetDialogOpen>(address(0x250300));
    api.dialogBytes = layout.dialogBytes;
    api.editboxBytes = layout.editboxBytes;
    if (!api.Ready()) return false;
    out = api;
    return true;
}

} // namespace swml
