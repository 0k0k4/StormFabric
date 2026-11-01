#pragma once
#include "custom_display_dialog_layout.h"
#include <array>
#include <string_view>

namespace swml {

// Copy primitive is injectable for offline tests. A future native adapter must
// supply a bounded, failure-reporting process-memory read, not a raw memcpy.
using CustomDisplayDialogCopy = bool (*)(void* context, const void* base,
    size_t offset, void* out, size_t bytes);
struct CustomDisplayDialogTextAccess {
    CustomDisplayDialogWidgetLayout layout;
    const void* editboxTable = nullptr;
    const void* contentTable = nullptr;
    CustomDisplayDialogCopy copy = nullptr;
    void* context = nullptr;
};

struct CustomDisplayDialogTextSnapshot {
    std::array<std::array<char, 11>, 4> text{};
    std::array<uint32_t, 4> length{};
    std::array<std::string_view, 4> Fields() const {
        return {{{text[0].data(), length[0]}, {text[1].data(), length[1]},
                 {text[2].data(), length[2]}, {text[3].data(), length[3]}}};
    }
};
enum class CustomDisplayDialogTextResult {
    Ok, Unavailable, StaleOwner, WrongThread, Unreadable, WrongType,
    WrongParent, InvalidString, TooLong
};

// Owner-thread-only, bounded reader. This is not a window installer. Bind only
// AFTER the native owning adapter has certified/created the attached widgets;
// call Invalidate BEFORE any ancestor teardown. Tokens belong to this instance,
// which must outlive all callbacks (do not recycle it to defeat stale detection).
class CustomDisplayDialogTextSession {
public:
    using Token = uint64_t;
    CustomDisplayDialogTextSession() = default;
    CustomDisplayDialogTextSession(const CustomDisplayDialogTextSession&) = delete;
    CustomDisplayDialogTextSession& operator=(const CustomDisplayDialogTextSession&) = delete;
    Token Bind(const void* owner, uint32_t thread, const void* parent,
        const std::array<const void*, 4>& editboxes, const CustomDisplayDialogTextAccess& access);
    void Invalidate();
    CustomDisplayDialogTextResult Read(const void* owner, Token token, uint32_t thread,
        CustomDisplayDialogTextSnapshot& out) const;
private:
    Token generation_ = 0;
    const void* owner_ = nullptr;
    const void* parent_ = nullptr;
    uint32_t thread_ = 0;
    std::array<const void*, 4> editboxes_{};
    CustomDisplayDialogTextAccess access_{};
};

} // namespace swml
