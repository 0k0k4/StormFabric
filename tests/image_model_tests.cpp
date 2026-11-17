#include "../src/image_model.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

using namespace swml::image_model;

int main() {
    assert(ValidSize(1, 1) && ValidSize(4096, 4096));
    assert(!ValidSize(0, 1) && !ValidSize(1, 0) && !ValidSize(4097, 1) && !ValidSize(1, 4097));
    assert(Bytes(4096, 4096) == 64ull * 1024 * 1024);
    assert(ValidFormat(SW_IMAGE_BGRA8_PREMULTIPLIED) && ValidFormat(SW_IMAGE_RGBA8) && !ValidFormat(2) && !ValidFormat(-1));
    assert(ValidFlags(0) && ValidFlags(SW_IMAGE_FLAG_NEAREST) && !ValidFlags(2));
    assert(ValidSurface(SW_SURFACE_MENU) && ValidSurface(SW_SURFACE_NONE) && !ValidSurface(4) && !ValidSurface(-1));

    // Regions: NULL = whole image; bounds and stride enforced without overflow.
    SwImageRegion r;
    assert(ResolveRegion(nullptr, 8, 4, 32, r) && r.x == 0 && r.y == 0 && r.width == 8 && r.height == 4);
    assert(!ResolveRegion(nullptr, 8, 4, 31, r));
    SwImageRegion in = {6, 3, 2, 1};
    assert(ResolveRegion(&in, 8, 4, 8, r) && r.x == 6 && r.width == 2);
    in = {6, 3, 3, 1}; assert(!ResolveRegion(&in, 8, 4, 64, r));
    in = {8, 0, 1, 1}; assert(!ResolveRegion(&in, 8, 4, 64, r));
    in = {1, 1, 0, 1}; assert(!ResolveRegion(&in, 8, 4, 64, r));
    in = {1, 1, 0xffffffffu, 1}; assert(!ResolveRegion(&in, 8, 4, 0xffffffffu, r)); // x + width overflow

    // Dirty rectangle union.
    Dirty d;
    assert(d.Empty());
    Merge(d, {2, 2, 2, 2});
    assert(!d.Empty() && d.x0 == 2 && d.y0 == 2 && d.x1 == 4 && d.y1 == 4);
    Merge(d, {0, 3, 1, 5});
    assert(d.x0 == 0 && d.y0 == 2 && d.x1 == 4 && d.y1 == 8);

    // Region copy honours the source stride and the destination position.
    std::vector<uint8_t> image(4 * 3 * 4, 0);
    const uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8, 99, 99, /* padding */
                           9, 10, 11, 12, 13, 14, 15, 16, 99, 99};
    CopyRegion(image.data(), 4, {1, 1, 2, 2}, src, 10);
    assert(image[(1 * 4 + 1) * 4] == 1 && image[(1 * 4 + 2) * 4 + 3] == 8);
    assert(image[(2 * 4 + 1) * 4] == 9 && image[(2 * 4 + 2) * 4 + 3] == 16);
    assert(image[0] == 0 && image[(1 * 4 + 3) * 4] == 0);

    // Placement on a 1920x1080 window.
    SwImageRect rect = {0, 0, 0, 0, 0};
    Box b = Place(rect, 100, 50, 1920, 1080);
    assert(b.x == 0 && b.y == 0 && b.width == 100 && b.height == 50);
    rect = {10, 20, 0, 0, 0};
    b = Place(rect, 100, 50, 1920, 1080);
    assert(b.x == 10 && b.y == 20);
    rect = {0, 0, 0, 0, 4};
    b = Place(rect, 101, 51, 1920, 1080); // centred, snapped to a whole pixel
    assert(b.x == 910 && b.y == 515 && b.width == 101);
    rect = {-5, -5, 200, 100, 8};
    b = Place(rect, 100, 50, 1920, 1080);
    assert(b.x == 1920 - 200 - 5 && b.y == 1080 - 100 - 5 && b.width == 200 && b.height == 100);
    rect = {0, 0, 0, 0, 2};
    b = Place(rect, 100, 50, 1920, 1080);
    assert(b.x == 1820 && b.y == 0);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    rect = {0, 0, 0, 0, 0}; assert(ValidRect(&rect));
    rect.anchor = 9; assert(!ValidRect(&rect));
    rect = {nan, 0, 0, 0, 0}; assert(!ValidRect(&rect));
    rect = {1e9f, 0, 0, 0, 0}; assert(!ValidRect(&rect));
    assert(!ValidRect(nullptr));

    assert(ClampOpacity(.5f) == .5f && ClampOpacity(2) == 1 && ClampOpacity(-1) == 0 && ClampOpacity(nan) < 0);

    // Surfaces: NONE always; others only while recently pulsed.
    uint64_t pulses[3] = {0, 1000, 0};
    assert(SurfaceActive(SW_SURFACE_NONE, pulses, 5));
    assert(!SurfaceActive(SW_SURFACE_MENU, pulses, 1000));
    assert(SurfaceActive(SW_SURFACE_HUD, pulses, 1000 + kSurfaceTimeoutMs));
    assert(!SurfaceActive(SW_SURFACE_HUD, pulses, 1001 + kSurfaceTimeoutMs));
    assert(!SurfaceActive(SW_SURFACE_HUD, pulses, 999)); // clock before pulse: not active

    std::puts("image_model_tests: OK");
    return 0;
}
