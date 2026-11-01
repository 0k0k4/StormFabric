#include "custom_display_catalogue.h"

namespace swml {

CustomDisplayCatalogueClick ReadCustomDisplayCatalogueClick(
        const CustomDisplayCatalogueTarget& target, const void* owner, uint32_t thread,
        int32_t action, const CustomDisplayCatalogueLayout& layout,
        CustomDisplayDialogCopy copy, void* copyContext) {
    using Result = CustomDisplayCatalogueClick;
    // Unrelated actions/threads/owners never even dereference the catalogue.
    if (action != 0x61 || !owner || owner != target.owner || !thread || thread != target.thread ||
        !target.ui || !target.catalogue || !target.entry || !target.definition || !copy)
        return Result::PassThrough;
    for (uint32_t offset : {layout.stateUi, layout.uiCatalogue, layout.catalogueView,
            layout.viewList, layout.listSelectedItem, layout.itemEntry, layout.entryDefinition})
        if (!offset || offset >= 0x10000 || offset % alignof(void*)) return Result::Unreadable;
    const void *ui = nullptr, *catalogue = nullptr, *view = nullptr, *list = nullptr;
    const void *item = nullptr, *entry = nullptr, *definition = nullptr;
    auto pointer = [&](const void* base, uint32_t offset, const void*& out) {
        return base && copy(copyContext, base, offset, &out, sizeof(out));
    };
    if (!pointer(owner, layout.stateUi, ui)) return Result::Unreadable;
    if (ui != target.ui) return Result::PassThrough;
    if (!pointer(ui, layout.uiCatalogue, catalogue)) return Result::Unreadable;
    if (catalogue != target.catalogue) return Result::PassThrough;
    if (!pointer(catalogue, layout.catalogueView, view) || !pointer(view, layout.viewList, list) ||
        !pointer(list, layout.listSelectedItem, item)) return Result::Unreadable;
    if (!item) return Result::PassThrough;
    if (!pointer(item, layout.itemEntry, entry)) return Result::Unreadable;
    if (entry != target.entry) return Result::PassThrough;
    // Native case 0x61 reads the tagged record's first dword; only kind 0 is a
    // component definition. Kind 2 is a different path and must remain native.
    uint32_t kind = UINT32_MAX;
    if (!copy(copyContext, entry, 0, &kind, sizeof(kind))) return Result::Unreadable;
    if (kind != 0) return Result::PassThrough;
    if (!pointer(entry, layout.entryDefinition, definition)) return Result::Unreadable;
    return definition == target.definition ? Result::OpenRequested : Result::PassThrough;
}

} // namespace swml
