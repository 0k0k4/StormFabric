#pragma once
#include "../include/sw_mod_sdk.h"
#include <cstdint>
#include <map>

namespace swml {
// Handles belong to the native tree; Forget never dereferences/destroys them.
class NotificationOverlay {
public:
    void Update(const SwUiApi& ui, void* context, void* parent);
    void Forget();
private:
    struct Slot {
        SwUiHandle root = nullptr;
        SwUiHandle bars[3] = {};
        SwUiHandle headings[3] = {};
        SwUiHandle detail = nullptr;
    };
    void* context_ = nullptr;
    SwUiHandle container_ = nullptr;
    Slot slots_[4] = {};
    uint64_t revision_ = UINT64_MAX;
};

// update receives application state, NOT the UI context. Only init may bind a context.
// Never create an entry from update: a cleared/destroyed tree must remain forgotten.
class NotificationInterfaces {
public:
    void Initialize(void* parent, void* context) {
        Forget(parent);
        if (parent && context) entries_[parent].context = context;
    }
    void Forget(void* parent) { entries_.erase(parent); }
    void Update(const SwUiApi& ui, void* parent) {
        auto entry = entries_.find(parent);
        if (entry != entries_.end()) entry->second.overlay.Update(ui, entry->second.context, parent);
    }
private:
    struct Entry { void* context = nullptr; NotificationOverlay overlay; };
    std::map<void*, Entry> entries_;
};
}
