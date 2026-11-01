#pragma once
#include "../include/sw_mod_sdk.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace swml {
struct VideoEffects {
    float offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1;
    float r = 1, g = 1, b = 1, a = 1, grayscale = 0, invert = 0;
    bool identity() const {
        return offsetX == 0 && offsetY == 0 && scaleX == 1 && scaleY == 1 &&
            r == 1 && g == 1 && b == 1 && a == 1 && grayscale == 0 && invert == 0;
    }
    bool offset(double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 65536 || std::abs(y) > 65536) return false;
        offsetX = float(x); offsetY = float(y); return true;
    }
    bool scale(double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y) || x < .01 || y < .01 || x > 100 || y > 100) return false;
        scaleX = float(x); scaleY = float(y); return true;
    }
    static bool unit(double x) { return std::isfinite(x) && x >= 0 && x <= 1; }
    bool tint(double red, double green, double blue, double alpha) {
        if (!unit(red) || !unit(green) || !unit(blue) || !unit(alpha)) return false;
        r = float(red); g = float(green); b = float(blue); a = float(alpha); return true;
    }
    bool filter(double gray, double inv) {
        if (!unit(gray) || !unit(inv)) return false;
        grayscale = float(gray); invert = float(inv); return true;
    }
};
struct VideoCapture {
    int width = 0, height = 0, logicalWidth = 0, logicalHeight = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> rgba; // GL rows, bottom first
    bool pixel(int x, int y, SwVideoPixel* out) const {
        if (!out || x < 0 || y < 0 || x >= logicalWidth || y >= logicalHeight ||
            width <= 0 || height <= 0 || rgba.size() != size_t(width) * height * 4) return false;
        int sx = int((int64_t(x) * width) / logicalWidth);
        int sy = height - 1 - int((int64_t(y) * height) / logicalHeight);
        size_t i = (size_t(sy) * width + sx) * 4;
        *out = {rgba[i], rgba[i+1], rgba[i+2], rgba[i+3], sequence};
        return true;
    }
};
}
