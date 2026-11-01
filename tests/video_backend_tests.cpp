// Exercise the real loader backend with fake native script/monitor calls. No executable hooks
// are installed and no OpenGL context/game process is created.
#include "../src/native_video.cpp"
#include <cassert>
#include <cstdio>
namespace swml { void LogInfo(const char*, ...) {} void LogError(const char*, ...) {} void LogDiscoveryProgress(const char*,size_t,size_t,const char*,bool) {} }
namespace {
using namespace swml;
alignas(16) uint8_t script[0x380] = {}, stateMemory[64] = {}, monitorA[0x600] = {}, monitorB[0x600] = {};
void* state = stateMemory + 8;
SwModHost host = {};
bool setFx = true, request = true, pixelReady = false, called = false;
SwVideoPixel result = {};
void FakeDraw(void*,void*,void*,void*,int*,float,float,int,void*,int*,void*) {
    called = true;
    script[0x304] = 1;
    const auto api = NativeVideoPrimitives();
    if (setFx) assert(api->setOffset(&host,state,4,8));
    if (request) pixelReady = api->getPixel(&host,state,0,0,&result);
    script[0x304] = 0;
}
void* FakeDestroy(void* p, uint32_t) { return p; }
void Invoke(void* monitor, void* target, int* size) {
    int error = 0;
    DrawMonitor(nullptr,nullptr,nullptr,target,size,1,1,1,
                static_cast<uint8_t*>(monitor) + video_profile::kMonitorSignal,&error,nullptr);
}
}
int main() {
    using namespace swml;
    g_installed = true; g_drawMonitor = &FakeDraw; g_destroy = &FakeDestroy;
    *reinterpret_cast<uintptr_t*>(stateMemory) = reinterpret_cast<uintptr_t>(script);
    *reinterpret_cast<void**>(script+8) = state;
    void* objectA = reinterpret_cast<void*>(0x12340000);
    void* objectB = reinterpret_cast<void*>(0x56780000);
    void* resourceA[2] = {nullptr,objectA}, *resourceB[2] = {nullptr,objectB};
    void* storageA = resourceA, *storageB = resourceB;
    assert(ResourceObject(&storageA) == objectA); // regression: must dereference storage -> wrapper -> object
    assert(!ResourceObject(nullptr));
    assert(NativeVideoPrimitives()->enable(&host));
    assert(!NativeVideoPrimitives()->setOffset(&host,state,1,1)); // outside onDraw
    int size[2] = {32,32};
    Invoke(monitorA,&storageA,size);
    assert(called && g_draw == nullptr && g_monitors.size() == 1 && !pixelReady);
    auto recordA = g_monitors.at(monitorA);
    assert(recordA->overlay == objectA && recordA->effects.offsetX == 4 && LeaseActive(recordA->effectOwner));
    auto lease = recordA->effectOwner;
    auto image = std::make_shared<VideoCapture>();
    image->width = image->height = 1; image->logicalWidth = image->logicalHeight = 32;
    image->sequence = 9; image->rgba = {10,20,30,255};
    recordA->latest = image;
    Invoke(monitorA,&storageA,size);
    assert(pixelReady && result.r == 10 && result.captureSequence == 9);
    Invoke(monitorB,&storageB,size);
    assert(!pixelReady && g_monitors.size() == 2); // never read another monitor's capture
    setFx = request = false;
    Invoke(monitorA,&storageA,size);
    assert(recordA->effects.identity() && !recordA->captureOwner); // no persistent settings/capture demand
    size[0] = 64;
    Invoke(monitorA,&storageA,size);
    assert(!recordA->active && g_monitors.at(monitorA) != recordA && !g_monitors.at(monitorA)->latest);
    MonitorDestroy(monitorA,0);
    assert(g_monitors.count(monitorA) == 0 && g_overlays.count(objectA) == 0);
    ReleaseNativeVideoOwner(&host);
    assert(!LeaseActive(lease) && g_monitors.empty() && g_overlays.empty());
    assert(NativeVideoPrimitives()->enable(&host));
    assert(!LeaseActive(lease)); // same host address cannot revive an old draw token
    ReleaseNativeVideoOwner(&host);
    puts("PASS: actual backend storage indirection, onDraw-only calls, monitor isolation, reset/resize/destruction, unload/reload leases");
}
