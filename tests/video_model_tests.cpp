#include "../src/video_model.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    swml::VideoEffects fx;
    assert(fx.identity());
    assert(fx.offset(4,8) && fx.scale(2,.5) && fx.tint(1,.5,0,1) && fx.filter(.5,1));
    assert(!fx.identity());
    auto before = fx;
    assert(!fx.scale(0,1) && !fx.scale(1,101) && !fx.offset(1e30,0));
    assert(!fx.tint(-1,1,1,1) && !fx.filter(.5,2));
    double nan = (std::numeric_limits<double>::quiet_NaN)();
    assert(!fx.offset(nan,0) && !fx.scale(nan,1) && !fx.tint(1,nan,1,1) && !fx.filter(0,nan));
    assert(fx.offsetX == before.offsetX && fx.scaleX == before.scaleX && fx.g == before.g);
    fx = {}; assert(fx.identity());
    swml::VideoCapture image;
    image.width = image.height = 2;
    image.logicalWidth = image.logicalHeight = 4;
    image.sequence = 7;
    image.rgba = {10,0,0,255, 20,0,0,255, 30,0,0,255, 40,0,0,255};
    SwVideoPixel pixel = {};
    assert(image.pixel(0,0,&pixel) && pixel.r == 30 && pixel.captureSequence == 7);
    assert(image.pixel(3,0,&pixel) && pixel.r == 40);
    assert(image.pixel(0,3,&pixel) && pixel.r == 10);
    assert(image.pixel(3,3,&pixel) && pixel.r == 20);
    assert(!image.pixel(-1,0,&pixel) && !image.pixel(4,0,&pixel) && !image.pixel(0,4,&pixel));
    assert(!image.pixel(0,0,nullptr));
    image.rgba.pop_back(); assert(!image.pixel(0,0,&pixel));
    image.logicalWidth = 0; assert(!image.pixel(0,0,&pixel));
    puts("PASS: video defaults, finite/bounded transforms, no partial invalid update, pixel scaling/origin/bounds");
}
