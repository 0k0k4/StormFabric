#include "custom_display_dialog_text.h"
#include "game.h"
#include <limits>

namespace swml {

void CustomDisplayDialogTextSession::Invalidate() {
    owner_ = parent_ = nullptr;
    thread_ = 0;
    editboxes_ = {};
    access_ = {};
}

CustomDisplayDialogTextSession::Token CustomDisplayDialogTextSession::Bind(
        const void* owner, uint32_t thread, const void* parent,
        const std::array<const void*, 4>& editboxes, const CustomDisplayDialogTextAccess& access) {
    Invalidate();
    if (generation_ == (std::numeric_limits<Token>::max)()) return 0;
    ++generation_; // Failed rebinding also invalidates the previous generation.
    const auto& l = access.layout;
    if (!owner || !thread || !parent || !access.copy || !access.editboxTable || !access.contentTable ||
        access.editboxTable == access.contentTable ||
        !l.parent || l.parent >= 0x10000 || !l.editboxContent || l.editboxContent >= 0x10000 ||
        !l.liveText || l.liveText >= 0x10000 || l.liveText % alignof(void*) ||
        l.parent % alignof(void*) || l.editboxContent % alignof(void*)) return 0;
    for (size_t i = 0; i < editboxes.size(); ++i) {
        if (!editboxes[i] || editboxes[i] == parent || editboxes[i] == owner) return 0;
        for (size_t j = 0; j < i; ++j) if (editboxes[i] == editboxes[j]) return 0;
    }
    owner_ = owner;
    parent_ = parent;
    thread_ = thread;
    editboxes_ = editboxes;
    access_ = access;
    return generation_;
}

CustomDisplayDialogTextResult CustomDisplayDialogTextSession::Read(const void* owner,
        Token token, uint32_t thread, CustomDisplayDialogTextSnapshot& out) const {
    using Result = CustomDisplayDialogTextResult;
    // Check ownership BEFORE making any read, even a vtable/type read.
    if (!owner_) return Result::Unavailable;
    if (owner != owner_ || !token || token != generation_) return Result::StaleOwner;
    if (thread != thread_) return Result::WrongThread;
    const auto& l = access_.layout;
    auto copy = [&](const void* base, size_t offset, void* destination, size_t bytes) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(base);
        if (!address || offset > UINTPTR_MAX - address || bytes > UINTPTR_MAX - (address + offset))
            return false;
        return access_.copy(access_.context, base, offset, destination, bytes);
    };
    auto pointer = [&](const void* base, size_t offset, const void*& value) {
        return copy(base, offset, &value, sizeof(value));
    };
    CustomDisplayDialogTextSnapshot snapshot;
    for (size_t i = 0; i < editboxes_.size(); ++i) {
        const void *table = nullptr, *parent = nullptr, *content = nullptr;
        if (!pointer(editboxes_[i], 0, table)) return Result::Unreadable;
        if (table != access_.editboxTable) return Result::WrongType;
        if (!pointer(editboxes_[i], l.parent, parent)) return Result::Unreadable;
        if (parent != parent_) return Result::WrongParent;
        if (!pointer(editboxes_[i], l.editboxContent, content) || !content ||
            !pointer(content, 0, table)) return Result::Unreadable;
        if (table != access_.contentTable) return Result::WrongType;
        if (!pointer(content, l.parent, parent)) return Result::Unreadable;
        if (parent != editboxes_[i]) return Result::WrongParent;
        sw::EStr string{};
        if (!copy(content, l.liveText, &string, sizeof(string))) return Result::Unreadable;
        if (string.len > 10) return Result::TooLong; // Never truncate pasted text.
        if (!string.data) {
            if (string.len || string.cap) return Result::InvalidString;
        } else {
            if (string.cap <= string.len) return Result::InvalidString;
            if (!copy(string.data, 0, snapshot.text[i].data(), size_t(string.len) + 1))
                return Result::Unreadable;
            if (snapshot.text[i][string.len] != '\0') return Result::InvalidString;
        }
        snapshot.length[i] = string.len;
    }
    out = snapshot; // Atomic output: no partial four-field snapshots on failure.
    return Result::Ok;
}

} // namespace swml
