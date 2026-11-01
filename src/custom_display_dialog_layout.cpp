#include "custom_display_dialog_layout.h"
#include <cstring>

namespace swml {
namespace {
using namespace discovery;
#include "engine_discovery.generated.h"
}

bool DiscoverCustomDisplayDialogCallbackLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogCallbackLayout& out) {
    constexpr uint32_t group = CustomDisplayDialogCallbackGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key)) return false;
    for (const auto& table : kDiscoveryTables)
        if ((table.group & group) && current.Address(table.key) != result.Address(table.key)) return false;
    auto operand = [&](uint32_t key, uint32_t relative, uint32_t& value) {
        const uint64_t at = uint64_t(current.Address(key)) + relative;
        if (!current.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), 4)) return false;
        std::memcpy(&value, image.base + at, 4);
        return value && value < 0x10000;
    };
    CustomDisplayDialogCallbackLayout layout;
    uint32_t callbackAgain = 0, dataAgain = 0, actionAgain = 0;
    // Retained callback_route/fields assembly. These are instruction operand
    // positions, NOT fallback object offsets or published function addresses.
    if (!operand(0x25a190, 0x6b2, layout.confirmButton) ||
        !operand(0x25a190, 0x4d8, layout.cancelButton) ||
        !operand(0x25a190, 0x427, layout.buttonBytes) ||
        !operand(0x249200, 0x17c, layout.callback) ||
        !operand(0x249200, 0x183, layout.userData) ||
        !operand(0x249200, 0xfc, layout.releaseAction) ||
        !operand(0x249200, 0x103, layout.pressActions) ||
        !operand(0x249900, 0x14d, callbackAgain) ||
        !operand(0x249900, 0x159, dataAgain) ||
        !operand(0x249900, 0xdf, actionAgain)) return false;
    if (layout.callback != callbackAgain || layout.userData != dataAgain ||
        layout.releaseAction != actionAgain || layout.callback + sizeof(void*) != layout.userData ||
        layout.releaseAction + 4 != layout.pressActions || layout.pressActions + 8 > layout.callback ||
        layout.userData + sizeof(void*) > layout.buttonBytes ||
        layout.confirmButton % alignof(void*) || layout.cancelButton % alignof(void*) ||
        layout.cancelButton != layout.confirmButton + sizeof(void*)) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayNativeInputStateLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayNativeInputStateLayout& out) {
    constexpr uint32_t group = CustomDisplayNativeInputStateGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    CustomDisplayDialogInputLayout input;
    CustomDisplayDialogWidgetLayout widgets;
    if (!DiscoverCustomDisplayDialogInputLayout(image, result, input) ||
        !DiscoverCustomDisplayDialogWidgetLayout(image, result, widgets)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key)) return false;
    auto operand = [&](uint32_t key, uint32_t relative, size_t bytes, uint32_t& value) {
        const uint64_t at = uint64_t(current.Address(key)) + relative;
        if (!current.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), bytes)) return false;
        value = 0; std::memcpy(&value, image.base + at, bytes);
        return value && value < 0x10000;
    };
    CustomDisplayNativeInputStateLayout layout;
    uint32_t keyLast = 0, keyAgain = 0, mouseLast = 0, mouseAgain = 0, recipientAgain = 0, hoverAgain = 0;
    // Native focus-loss loops visit every window key/button, including GLFW
    // key codes above 255. Their bounds are NOT vehicle text-key codes.
    if (!operand(0x1bf810, 0x14, 4, layout.hostWindow) ||
        !operand(0x97090, 0x40, 4, layout.keyStates) ||
        !operand(0x97090, 0x51, 4, keyLast) || !operand(0x97090, 0x89, 4, keyAgain) ||
        !operand(0x97090, 0x97, 1, layout.mouseStates) ||
        !operand(0x97090, 0xa7, 1, mouseLast) || !operand(0x97090, 0xd6, 1, mouseAgain) ||
        !operand(0x1e2e20, 0x10, 4, layout.rootRecipient) ||
        !operand(0x1e2e20, 0x30, 4, recipientAgain) ||
        !operand(0x1e2e20, 0x3b, 4, layout.rootHover) ||
        !operand(0x1e2e20, 0x5c, 4, hoverAgain) ||
        !operand(0x1e2e20, 0x26, 1, layout.rootContext) ||
        !operand(0x1e1bc0, 0x4c, 4, layout.rootPointerHeld)) return false;
    if (keyLast != keyAgain || mouseLast != mouseAgain || keyLast >= 512 || mouseLast >= 8 ||
        layout.hostWindow % alignof(void*) || layout.rootRecipient != recipientAgain ||
        layout.rootHover != hoverAgain || layout.rootHover != input.rootHover ||
        layout.rootPointerHeld != input.rootPointerHeld || layout.rootContext != widgets.rootContext ||
        layout.rootRecipient != widgets.rootContext + input.contextFocus) return false;
    layout.keyCount = keyLast + 1; layout.mouseCount = mouseLast + 1;
    if (layout.mouseStates + layout.mouseCount > layout.keyStates ||
        layout.keyStates + layout.keyCount >= 0x10000) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayDialogInputLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogInputLayout& out) {
    constexpr uint32_t group = CustomDisplayDialogInputGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    CustomDisplayVehicleObserverLayout vehicle;
    if (!DiscoverCustomDisplayVehicleObserverLayout(image, result, vehicle)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key)) return false;
    for (const auto& table : kDiscoveryTables)
        if ((table.group & group) && current.Address(table.key) != result.Address(table.key)) return false;
    auto operand = [&](uint32_t key, uint32_t relative, size_t bytes, uint32_t& value) {
        const uint64_t at = uint64_t(current.Address(key)) + relative;
        if (!current.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), bytes)) return false;
        value = 0; std::memcpy(&value, image.base + at, bytes);
        return value && value < 0x10000;
    };
    CustomDisplayDialogInputLayout layout;
    uint32_t focus = 0, focusAgain = 0, contextAgain = 0, queueAgain = 0;
    uint32_t heldAgain = 0, rootFocus = 0;
    layout.stateRoot = vehicle.stateRoot;
    // Vehicle character/raw-key assembly, basic configured-Escape comparisons,
    // and text submission comparisons; all retained in vehicle_input/boundary.
    if (!operand(0x7f09d0, 0x74, 4, focus) ||
        !operand(0x7f09d0, 0x8f, 4, layout.stateContext) ||
        !operand(0x7f09d0, 0x83, 4, layout.stateQueue) ||
        !operand(0x7f0af0, 0x1ba, 4, focusAgain) ||
        !operand(0x7f0af0, 0x1d0, 4, contextAgain) ||
        !operand(0x7f0af0, 0x1c9, 4, queueAgain) ||
        !operand(0x25a9e0, 0x31, 4, layout.escapeCode) ||
        !operand(0x25a9e0, 0x3d, 4, layout.escapeKind) ||
        !operand(0x1df720, 0x29a, 1, layout.submitKey) ||
        !operand(0x1df720, 0x29f, 1, layout.keypadSubmitKey) ||
        !operand(0x1e3180, 0xf7, 4, layout.rootHover) ||
        !operand(0x1e3180, 0xbb, 4, layout.rootPointerHeld) ||
        !operand(0x1e3180, 0x21c, 4, heldAgain) ||
        !operand(0x1e3180, 0x244, 4, rootFocus)) return false;
    if (focus != focusAgain || layout.stateContext != contextAgain || layout.stateQueue != queueAgain ||
        layout.stateRoot + vehicle.widgets.rootContext != layout.stateContext || focus <= layout.stateContext ||
        layout.stateContext % alignof(void*) || layout.stateQueue % alignof(void*) || focus % alignof(void*) ||
        layout.escapeKind + 8 != layout.escapeCode || layout.submitKey == layout.keypadSubmitKey) return false;
    layout.contextFocus = focus - layout.stateContext;
    if (layout.rootPointerHeld != heldAgain || rootFocus != focus - layout.stateRoot ||
        layout.rootHover + sizeof(void*) != rootFocus || layout.rootHover % alignof(void*)) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayDialogWidgetLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogWidgetLayout& out) {
    using namespace discovery;
    constexpr uint32_t group = CustomDisplayDialogWidgetGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    // Recheck CURRENT full bodies, call relationships, RTTI and table slots.
    // A stale or manufactured Result is not evidence. No native code is called.
    const auto current = Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key))
            return false;
    for (const auto& table : kDiscoveryTables)
        if ((table.group & group) && current.Address(table.key) != result.Address(table.key))
            return false;

    auto operand = [&](uint32_t key, uint32_t relative, size_t size, uint32_t& value) {
        const uint64_t at = uint64_t(current.Address(key)) + relative;
        if (!current.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), size))
            return false;
        value = 0;
        std::memcpy(&value, image.base + at, size);
        return value && value < 0x10000; // Extraction bound, not a native fallback.
    };
    CustomDisplayDialogWidgetLayout layout;
    uint32_t contentAgain = 0, liveLength = 0, allowedLength = 0;
    uint32_t modalAgain = 0, openAgain = 0;
    // Locations within authenticated instructions, never object offsets/RVAs to
    // use on a failed scan. Retained assembly: dialog_input/dispatch_complete.
    if (!operand(0x24f690, 0x120, 1, layout.parent) ||
        !operand(0x24f690, 0x124, 1, layout.editboxContent) ||
        !operand(0x24f8a0, 0x15, 1, contentAgain) ||
        !operand(0x24f690, 0x7b, 1, layout.initialText) ||
        !operand(0x1df640, 0x43, 4, layout.liveText) ||
        !operand(0x1df640, 0x3c, 4, liveLength) ||
        !operand(0x24f8a0, 0x1c, 4, layout.allowedText) ||
        !operand(0x24f8a0, 0x46, 4, allowedLength) ||
        !operand(0x1df640, 0x49, 4, layout.maxCharacters) ||
        !operand(0x24f690, 0x13, 4, layout.contentAllocation) ||
        !operand(0x25a990, 0x17, 4, layout.dialogOpen) ||
        !operand(0x250300, 0x16, 4, openAgain) ||
        !operand(0x25a990, 0x21, 4, layout.contextModal) ||
        !operand(0x25a990, 0x33, 4, modalAgain) ||
        !operand(0x1e1c20, 0x12, 1, layout.rootContext) ||
        !operand(0x1e20b0, 0x0b, 4, layout.rootModal)) return false;
    if (layout.editboxContent != contentAgain || layout.dialogOpen != openAgain ||
        layout.contextModal != modalAgain || layout.liveText + 8 != liveLength ||
        layout.allowedText + 8 != allowedLength ||
        layout.rootContext + layout.contextModal != layout.rootModal ||
        layout.parent % alignof(void*) || layout.parent + sizeof(void*) > layout.initialText ||
        layout.initialText % alignof(void*) || layout.liveText % alignof(void*) ||
        layout.allowedText % alignof(void*) || layout.editboxContent % alignof(void*) ||
        layout.contextModal % alignof(void*) || layout.rootContext % alignof(void*) ||
        layout.initialText + 16 > layout.editboxContent ||
        layout.liveText + 16 > layout.contentAllocation ||
        layout.allowedText + 16 > layout.contentAllocation ||
        layout.maxCharacters + 4 > layout.contentAllocation) return false;
    bool found = false;
    for (const auto& table : kDiscoveryTables) {
        if (table.key != 0xb0b5c8 || !(table.group & group)) continue;
        for (size_t i = 0; i < table.count; ++i) {
            if (table.slots[i].target != 0x250300) continue;
            if (found) return false;
            layout.openSlot = table.slots[i].index;
            found = true;
        }
    }
    if (!found) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayVehicleObserverLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayVehicleObserverLayout& out) {
    constexpr uint32_t group = CustomDisplayVehicleObserverGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    CustomDisplayVehicleObserverLayout layout;
    if (!DiscoverCustomDisplayDialogWidgetLayout(image, result, layout.widgets)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key))
            return false;
    for (const auto& table : kDiscoveryTables)
        if ((table.group & group) && current.Address(table.key) != result.Address(table.key))
            return false;
    // ADD RCX, imm32 before the native root teardown call; retained boundary A.
    const uint64_t at = uint64_t(current.Address(0x7cf750)) + 0x18;
    if (at > UINT32_MAX || !image.Executable(uint32_t(at), sizeof(layout.stateRoot))) return false;
    std::memcpy(&layout.stateRoot, image.base + at, sizeof(layout.stateRoot));
    if (!layout.stateRoot || layout.stateRoot >= 0x10000 || layout.stateRoot % alignof(void*)) return false;
    auto slot = [&](uint32_t key, uint32_t function, uint32_t& destination) {
        bool found = false;
        for (const auto& table : kDiscoveryTables) {
            if (table.key != key || !(table.group & group)) continue;
            for (size_t i = 0; i < table.count; ++i) {
                if (table.slots[i].target != function) continue;
                if (found) return false;
                destination = table.slots[i].index;
                found = true;
            }
        }
        return found;
    };
    if (!slot(0xb99940, 0x62ac80, layout.uiInitSlot) ||
        !slot(0xb99940, 0x62c530, layout.uiUpdateSlot) ||
        !slot(0xb99940, 0x241bc0, layout.uiTeardownSlot) ||
        !slot(0xb99940, 0x62ed20, layout.uiDestroySlot) ||
        !slot(0xb44838, 0x7cf750, layout.stateTeardownSlot) ||
        !slot(0xb44838, 0x7fa7a0, layout.stateActionsSlot)) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayVehicleActivityLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayVehicleActivityLayout& out) {
    constexpr uint32_t group = CustomDisplayVehicleActivityGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    CustomDisplayVehicleActivityLayout layout;
    if (!DiscoverCustomDisplayVehicleObserverLayout(image, result, layout.lifetime)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key))
            return false;
    for (const auto& table : kDiscoveryTables)
        if ((table.group & group) && current.Address(table.key) != result.Address(table.key))
            return false;
    auto slot = [&](uint32_t relative, uint32_t target, uint32_t& destination) {
        // CALL [RAX+disp8] in the authenticated substate switch, recut from
        // operands rather than reusing the unrelated UI slot convention.
        const uint64_t at = uint64_t(current.Address(0x848d20)) + relative;
        if (!current.Address(0x848d20) || at > UINT32_MAX || !image.Executable(uint32_t(at), 1)) return false;
        const uint8_t displacement = image.base[at];
        if (!displacement || displacement % sizeof(void*)) return false;
        destination = displacement / sizeof(void*);
        for (const auto& table : kDiscoveryTables) {
            if (table.key != 0xb44838 || !(table.group & group)) continue;
            for (size_t i = 0; i < table.count; ++i)
                if (table.slots[i].index == destination && table.slots[i].target == target) return true;
        }
        return false;
    };
    if (!slot(0xb5, 0x7cfce0, layout.stateEnterSlot) ||
        !slot(0x73, 0x7d0770, layout.stateLeaveSlot) ||
        layout.stateEnterSlot == layout.stateLeaveSlot) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayDialogConstructionLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayDialogConstructionLayout& out) {
    constexpr uint32_t group = CustomDisplayDialogConstructionGroup;
    if (!result.validImage || !result.reviewedBuild || !result.GroupReady(group)) return false;
    CustomDisplayDialogConstructionLayout layout;
    if (!DiscoverCustomDisplayDialogWidgetLayout(image, result, layout.widgets)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.validImage || !current.reviewedBuild || !current.GroupReady(group)) return false;
    for (const auto& contract : kDiscoveryContracts)
        if ((contract.groups & group) && current.Address(contract.key) != result.Address(contract.key))
            return false;
    for (const auto& table : kDiscoveryTables)
        if ((table.group & group) && current.Address(table.key) != result.Address(table.key)) return false;
    auto allocation = [&](uint32_t key, uint32_t relative, uint32_t& size) {
        const uint64_t at = uint64_t(current.Address(key)) + relative;
        if (!current.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), 4)) return false;
        std::memcpy(&size, image.base + at, 4);
        return size && size < 0x10000 && size % alignof(void*) == 0;
    };
    // MOV ECX,size before native operator-new in authenticated stock callers.
    // No size is supplied if either body or its call relationships fails.
    if (!allocation(0x62ac80, 0x88c, layout.dialogBytes) ||
        !allocation(0x627370, 0x2fc, layout.editboxBytes) ||
        layout.dialogBytes <= layout.widgets.dialogOpen ||
        layout.editboxBytes < layout.widgets.editboxContent + sizeof(void*)) return false;
    out = layout;
    return true;
}

bool DiscoverCustomDisplayCatalogueLayout(const discovery::Image& image,
        const discovery::Result& result, CustomDisplayCatalogueLayout& out) {
    if (!result.GroupReady(CustomDisplayCatalogueGroup)) return false;
    CustomDisplayVehicleObserverLayout lifetime;
    if (!DiscoverCustomDisplayVehicleObserverLayout(image, result, lifetime)) return false;
    const auto current = discovery::Discover(image.base, image.size);
    if (!current.reviewedBuild || !current.GroupReady(CustomDisplayCatalogueGroup) ||
        !current.GroupReady(CustomDisplayVehicleObserverGroup) ||
        !current.Address(0x611050) || current.Address(0x611050) != result.Address(0x611050)) return false;
    auto operand = [&](uint32_t key, uint32_t relative, size_t bytes, uint32_t& value) {
        const uint64_t at = uint64_t(current.Address(key)) + relative;
        if (!current.Address(key) || at > UINT32_MAX || !image.Executable(uint32_t(at), bytes)) return false;
        value = 0;
        std::memcpy(&value, image.base + at, bytes);
        return value && value < 0x10000 && value % alignof(void*) == 0;
    };
    CustomDisplayCatalogueLayout layout;
    // Case 0x61 reads its catalogue; getter follows selected list-item -> entry.
    // Neither hotbar nor shared selection function is hooked or called here.
    if (!operand(0x7fa7a0, 0x4770, 4, layout.stateUi) ||
        !operand(0x7fa7a0, 0x4777, 4, layout.uiCatalogue) ||
        !operand(0x611050, 0x03, 1, layout.catalogueView) ||
        !operand(0x611050, 0x07, 4, layout.viewList) ||
        !operand(0x611050, 0x0e, 4, layout.listSelectedItem) ||
        !operand(0x611050, 0x1a, 1, layout.itemEntry) ||
        !operand(0x7fa7a0, 0x4792, 1, layout.entryDefinition)) return false;
    out = layout;
    return true;
}

} // namespace swml
