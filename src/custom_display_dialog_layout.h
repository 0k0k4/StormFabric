#pragma once
#include "engine_discovery.h"

namespace swml {

inline constexpr uint32_t CustomDisplayNativeInputStateGroup = 32768;
struct CustomDisplayNativeInputStateLayout {
    uint32_t hostWindow = 0, keyStates = 0, keyCount = 0, mouseStates = 0, mouseCount = 0;
    uint32_t rootRecipient = 0, rootHover = 0, rootContext = 0, rootPointerHeld = 0;
};
bool DiscoverCustomDisplayNativeInputStateLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayNativeInputStateLayout& out);

inline constexpr uint32_t CustomDisplayDialogInputGroup = 16384;
struct CustomDisplayDialogInputLayout {
    uint32_t stateRoot = 0, stateContext = 0, stateQueue = 0, contextFocus = 0;
    uint32_t escapeKind = 0, escapeCode = 0, submitKey = 0, keypadSubmitKey = 0;
    uint32_t rootHover = 0, rootPointerHeld = 0;
};
bool DiscoverCustomDisplayDialogInputLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogInputLayout& out);

inline constexpr uint32_t CustomDisplayDialogCallbackGroup = 8192;
struct CustomDisplayDialogCallbackLayout {
    uint32_t confirmButton = 0, cancelButton = 0, buttonBytes = 0;
    uint32_t callback = 0, userData = 0, releaseAction = 0, pressActions = 0;
};
bool DiscoverCustomDisplayDialogCallbackLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogCallbackLayout& out);

// Operand-derived widget layout only. Does not certify vehicle-editor lifetime,
// action routing, registration, file authentication or permission to install UI.
inline constexpr uint32_t CustomDisplayDialogWidgetGroup = 256;
struct CustomDisplayDialogWidgetLayout {
    uint32_t editboxContent = 0, initialText = 0, parent = 0;
    uint32_t liveText = 0, allowedText = 0, maxCharacters = 0;
    uint32_t contentAllocation = 0, dialogOpen = 0;
    uint32_t contextModal = 0, rootContext = 0, rootModal = 0;
    uint32_t openSlot = 0;
};

bool DiscoverCustomDisplayDialogWidgetLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogWidgetLayout& out);

inline constexpr uint32_t CustomDisplayVehicleObserverGroup = 512;
struct CustomDisplayVehicleObserverLayout {
    CustomDisplayDialogWidgetLayout widgets;
    uint32_t stateRoot = 0;
    uint32_t uiInitSlot = 0, uiUpdateSlot = 0, uiTeardownSlot = 0, uiDestroySlot = 0;
    uint32_t stateTeardownSlot = 0, stateActionsSlot = 0;
};
bool DiscoverCustomDisplayVehicleObserverLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayVehicleObserverLayout& out);

// Enter/leave are active-substate transitions, NOT construction/destruction.
inline constexpr uint32_t CustomDisplayVehicleActivityGroup = 1024;
struct CustomDisplayVehicleActivityLayout {
    CustomDisplayVehicleObserverLayout lifetime;
    uint32_t stateEnterSlot = 0, stateLeaveSlot = 0;
};
bool DiscoverCustomDisplayVehicleActivityLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayVehicleActivityLayout& out);

inline constexpr uint32_t CustomDisplayDialogConstructionGroup = 2048;
struct CustomDisplayDialogConstructionLayout {
    CustomDisplayDialogWidgetLayout widgets;
    uint32_t dialogBytes = 0, editboxBytes = 0;
};
bool DiscoverCustomDisplayDialogConstructionLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogConstructionLayout& out);

inline constexpr uint32_t CustomDisplayCatalogueGroup = 4096;
struct CustomDisplayCatalogueLayout {
    uint32_t stateUi = 0, uiCatalogue = 0, catalogueView = 0;
    uint32_t viewList = 0, listSelectedItem = 0, itemEntry = 0, entryDefinition = 0;
};
bool DiscoverCustomDisplayCatalogueLayout(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayCatalogueLayout& out);

} // namespace swml
