#pragma once
#include "game.h"
#include <cstring>
#include <limits>
#include <string_view>

namespace swml {

// Engine-owned buffers only. A basic dialog BORROWS its two input strings;
// editbox constructor/filter/set-text CONSUME theirs (and may leave data dangling).
// Keep those operations distinct; never infer ownership from a returned pointer.
class CustomDisplayEngineString {
public:
    explicit CustomDisplayEngineString(const sw::Api& api) : api_(api) {}
    ~CustomDisplayEngineString() { Reset(); }
    CustomDisplayEngineString(const CustomDisplayEngineString&) = delete;
    CustomDisplayEngineString& operator=(const CustomDisplayEngineString&) = delete;

    bool Assign(std::string_view text) {
        if (!api_.str_reserve || !api_.str_free ||
            text.size() >= (std::numeric_limits<uint32_t>::max)() ||
            text.find('\0') != std::string_view::npos) return false;
        Reset();
        api_.str_reserve(&value_, uint32_t(text.size() + 1));
        if (!value_.data || value_.cap < text.size() + 1) { Reset(); return false; }
        if (!text.empty()) std::memcpy(value_.data, text.data(), text.size());
        value_.data[text.size()] = '\0';
        value_.len = uint32_t(text.size());
        return true;
    }
    // Native borrowing signatures are non-const even though the caller owns it.
    sw::EStr* Borrow() { return &value_; }
    template<class Consumer> auto Consume(Consumer consumer) {
        sw::EStr transferred = value_;
        value_ = {}; // Disarm BEFORE entry; no double-free after a native consumer.
        return consumer(&transferred);
    }
    void Reset() {
        if (value_.data) api_.str_free(value_.data);
        value_ = {};
    }
private:
    const sw::Api& api_;
    sw::EStr value_{};
};

} // namespace swml
