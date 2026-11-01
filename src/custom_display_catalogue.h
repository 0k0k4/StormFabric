#pragma once
#include "custom_display_dialog_text.h"

namespace swml {

// Identity supplied by the future registrar, never recognized by display name,
// archive MD5 alone or a stock monitor's shared definition pointer. Pin BOTH
// native catalogue entry and definition plus their enclosing owner/UI/catalogue.
// This structure is not an authentication or a registration API.
struct CustomDisplayCatalogueTarget {
    const void* owner = nullptr;
    uint32_t thread = 0;
    const void* ui = nullptr;
    const void* catalogue = nullptr;
    const void* entry = nullptr;
    const void* definition = nullptr;
};
enum class CustomDisplayCatalogueClick { PassThrough, OpenRequested, Unreadable };

// Read-only catalogue-specific decision. Does not call stock selection, create
// widgets or consume an input event. Future action wrapper may consume ONLY
// OpenRequested and enqueue opening outside native child/action traversal.
CustomDisplayCatalogueClick ReadCustomDisplayCatalogueClick(
    const CustomDisplayCatalogueTarget& target, const void* owner, uint32_t thread,
    int32_t action, const CustomDisplayCatalogueLayout& layout,
    CustomDisplayDialogCopy copy, void* copyContext);

} // namespace swml
