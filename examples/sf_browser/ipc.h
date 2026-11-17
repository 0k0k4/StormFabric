#pragma once
// Protocol between sf_browser.dll (in the game, pipe server) and sf_browser_host.exe (CEF,
// pipe client). Two byte-mode named pipes, one per direction (\\.\pipe\<base>_cmd: game ->
// host, \\.\pipe\<base>_evt: host -> game): a synchronous pipe handle serializes a write
// behind a pending read on another thread, so one duplex handle would deadlock.
// One message = Header + 8 int32 + string bytes.
// Frames travel through one shared-memory section per browser and size ("generation"):
//   name   Local\<base>_<browser>_<generation>    FrameHeader + width*height*4 BGRA bytes
//   mutex  Local\<base>_<browser>_<generation>_m  held while writing/reading pixels
//   event  Local\<base>_<browser>_<generation>_e  auto-reset, set after every painted frame
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sfb {

enum Type : uint16_t {
    // game -> host
    kCreate = 1,      // a: width, height, transparent; s: url
    kDestroy = 2,
    kLoadUrl = 3,     // s: url
    kExecJs = 4,      // s: code
    kPost = 5,        // s: text for window.sfBrowser.onmessage
    kNavigate = 6,    // a: action
    kResize = 7,      // a: width, height
    kMouseMove = 8,   // a: x, y, modifiers, buttons
    kMouseLeave = 9,  // a: x, y, modifiers, buttons
    kMouseClick = 10, // a: x, y, modifiers, buttons, button, up, clickCount
    kMouseWheel = 11, // a: x, y, modifiers, buttons, deltaX, deltaY (1/120 notch units)
    kKey = 12,        // a: kind (0 down, 1 up, 2 char), windowsKeyCode, nativeKeyCode, modifiers, systemKey
    kFocus = 13,      // a: focused
    kShutdown = 14,
    // host -> game
    kReady = 100,
    kFrameBuffer = 101, // a: generation, width, height
    kMessage = 102,     // s: text
    kLoadStart = 103,   // s: url
    kLoadEnd = 104,     // a: http status; s: url
    kLoadError = 105,   // a: error code; s: url
    kTitle = 106,       // s: title
    kUrl = 107,         // s: url
    kCrashed = 108,
    kLog = 109          // s: text (written to the game log)
};

// Modifier and button bits used on the wire (same values as the loader's SW_KEY_* / buttons).
enum { kModCtrl = 1, kModShift = 2, kModAlt = 4 };
enum { kButtonLeft = 1, kButtonRight = 2, kButtonMiddle = 4 };

#pragma pack(push, 1)
struct Header {
    uint32_t size;    // whole message, header included
    uint16_t type;
    uint16_t reserved;
    uint64_t browser;
    int32_t a[8];
    uint32_t textSize;
};
#pragma pack(pop)

struct FrameHeader {
    uint32_t width, height;
    uint32_t x0, y0, x1, y1; // dirty rectangle, half-open; empty when x0 >= x1
    uint32_t sequence;
    uint32_t reserved;
};

constexpr uint32_t kMaxMessage = 4u * 1024 * 1024;
constexpr uint32_t kMaxDimension = 4096;

struct Message {
    Header h = {};
    std::string text;
};

inline std::vector<uint8_t> Encode(uint16_t type, uint64_t browser, std::initializer_list<int32_t> values,
                                   const std::string& text = std::string()) {
    Header h = {};
    h.type = type;
    h.browser = browser;
    int i = 0;
    for (int32_t v : values) { if (i < 8) h.a[i++] = v; }
    h.textSize = uint32_t(text.size());
    h.size = uint32_t(sizeof(Header) + text.size());
    std::vector<uint8_t> out(h.size);
    std::memcpy(out.data(), &h, sizeof(h));
    if (!text.empty()) std::memcpy(out.data() + sizeof(h), text.data(), text.size());
    return out;
}

inline bool ReadAll(HANDLE pipe, void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size) {
        DWORD got = 0;
        if (!ReadFile(pipe, p, DWORD(size), &got, nullptr) || !got) return false;
        p += got;
        size -= got;
    }
    return true;
}
inline bool WriteAll(HANDLE pipe, const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    while (size) {
        DWORD put = 0;
        if (!WriteFile(pipe, p, DWORD(size), &put, nullptr) || !put) return false;
        p += put;
        size -= put;
    }
    return true;
}
inline bool Receive(HANDLE pipe, Message& m) {
    if (!ReadAll(pipe, &m.h, sizeof(Header))) return false;
    if (m.h.size != sizeof(Header) + uint64_t(m.h.textSize) || m.h.size > kMaxMessage) return false;
    m.text.assign(m.h.textSize, '\0');
    return !m.h.textSize || ReadAll(pipe, m.text.data(), m.h.textSize);
}

inline std::wstring PipeName(const std::wstring& base, const wchar_t* suffix) {
    return L"\\\\.\\pipe\\" + base + suffix;
}
inline std::wstring FrameName(const std::wstring& base, uint64_t browser, uint32_t generation, const wchar_t* suffix) {
    return L"Local\\" + base + L"_" + std::to_wstring(browser) + L"_" + std::to_wstring(generation) + suffix;
}

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace sfb
