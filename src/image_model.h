#pragma once
// Pure image-overlay rules (no Windows, no OpenGL): validation, placement, dirty regions.
// Kept header-only so tests/image_model_tests.cpp checks exactly what the loader runs.
#include "../include/sw_mod_sdk.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace swml { namespace image_model {

struct Box { float x = 0, y = 0, width = 0, height = 0; };
struct Dirty {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // half-open [x0,x1) x [y0,y1)
    bool Empty() const { return x0 >= x1 || y0 >= y1; }
};

inline bool ValidSize(uint32_t width, uint32_t height) {
    return width >= 1 && height >= 1 && width <= SW_IMAGE_MAX_DIMENSION && height <= SW_IMAGE_MAX_DIMENSION;
}
inline uint64_t Bytes(uint32_t width, uint32_t height) { return uint64_t(width) * height * 4; }
inline bool ValidFormat(int format) { return format == SW_IMAGE_BGRA8_PREMULTIPLIED || format == SW_IMAGE_RGBA8; }
inline bool ValidFlags(uint32_t flags) { return (flags & ~uint32_t(SW_IMAGE_FLAG_NEAREST)) == 0; }
inline bool ValidSurface(int surface) { return surface >= SW_SURFACE_MENU && surface <= SW_SURFACE_NONE; }

inline bool ValidRect(const SwImageRect* r) {
    return r && r->anchor >= 0 && r->anchor <= 8 && std::isfinite(r->x) && std::isfinite(r->y) &&
        std::isfinite(r->width) && std::isfinite(r->height) &&
        std::fabs(r->x) <= 65536 && std::fabs(r->y) <= 65536 && r->width <= 65536 && r->height <= 65536;
}

// Region inside the image, with a stride able to hold a row. NULL region = whole image.
inline bool ResolveRegion(const SwImageRegion* region, uint32_t width, uint32_t height,
                          uint32_t stride, SwImageRegion& out) {
    out = region ? *region : SwImageRegion{0, 0, width, height};
    if (out.width == 0 || out.height == 0) return false;
    if (out.x >= width || out.y >= height || out.width > width - out.x || out.height > height - out.y) return false;
    return uint64_t(stride) >= uint64_t(out.width) * 4;
}

inline void Merge(Dirty& dirty, const SwImageRegion& r) {
    if (dirty.Empty()) { dirty = {r.x, r.y, r.x + r.width, r.y + r.height}; return; }
    dirty.x0 = (std::min)(dirty.x0, r.x); dirty.y0 = (std::min)(dirty.y0, r.y);
    dirty.x1 = (std::max)(dirty.x1, r.x + r.width); dirty.y1 = (std::max)(dirty.y1, r.y + r.height);
}

// Copies `region` rows from `pixels` (rows `stride` bytes apart) into a tightly packed image.
inline void CopyRegion(uint8_t* image, uint32_t imageWidth, const SwImageRegion& r,
                       const void* pixels, uint32_t stride) {
    const auto* src = static_cast<const uint8_t*>(pixels);
    const size_t row = size_t(r.width) * 4;
    for (uint32_t y = 0; y < r.height; ++y)
        std::memcpy(image + (size_t(r.y + y) * imageWidth + r.x) * 4, src + size_t(y) * stride, row);
}

// Window-pixel box (+Y down) for an image of `w` x `h` placed with `rect` on a `sw` x `sh` window.
inline Box Place(const SwImageRect& rect, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh) {
    const float width = rect.width > 0 ? rect.width : float(w);
    const float height = rect.height > 0 ? rect.height : float(h);
    const int ax = rect.anchor % 3, ay = rect.anchor / 3;
    Box b;
    b.x = float(sw) * ax * .5f - width * ax * .5f + rect.x;
    b.y = float(sh) * ay * .5f - height * ay * .5f + rect.y;
    b.width = width; b.height = height;
    // Whole pixels keep 1:1 images sharp (centred anchors land on half pixels otherwise).
    if (rect.width <= 0 && rect.height <= 0) { b.x = std::floor(b.x + .5f); b.y = std::floor(b.y + .5f); }
    return b;
}

inline float ClampOpacity(float v) { return std::isfinite(v) ? (std::min)(1.f, (std::max)(0.f, v)) : -1.f; }

// A surface counts as active while it has been updated within this window.
constexpr uint64_t kSurfaceTimeoutMs = 250;
inline bool SurfaceActive(int surface, const uint64_t lastPulseMs[3], uint64_t nowMs) {
    if (surface == SW_SURFACE_NONE) return true;
    const uint64_t last = lastPulseMs[surface];
    return last != 0 && nowMs >= last && nowMs - last <= kSurfaceTimeoutMs;
}

} }
