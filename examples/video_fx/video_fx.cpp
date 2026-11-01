// No game addresses, structure offsets, OpenGL imports or rendering hooks in this plugin.
#include "../../include/sw_mod_sdk.h"
#include <cmath>
#include <limits>

static const SwModHost* g_host = nullptr;
static const SwVideoApi* Video() { return g_host->api->video; }
static const SwLuaApi* Lua() { return g_host->api->lua; }
static int Result(void* L, bool ok) { Lua()->pushBool(L,ok); return 1; }
static int Offset(void* L, void*) {
    return Result(L,Video()->setOffset(g_host,L,Lua()->argNumber(L,1,0),Lua()->argNumber(L,2,0)));
}
static int Scale(void* L, void*) {
    double x = Lua()->argNumber(L,1,1);
    return Result(L,Video()->setScale(g_host,L,x,Lua()->argNumber(L,2,x)));
}
static int Tint(void* L, void*) {
    return Result(L,Video()->setTint(g_host,L,Lua()->argNumber(L,1,255)/255,
        Lua()->argNumber(L,2,255)/255,Lua()->argNumber(L,3,255)/255,Lua()->argNumber(L,4,255)/255));
}
static int Filter(void* L, void*) {
    return Result(L,Video()->setFilter(g_host,L,Lua()->argNumber(L,1,0),Lua()->argNumber(L,2,0)));
}
static int Reset(void* L, void*) { return Result(L,Video()->reset(g_host,L)); }
static int Request(void* L, void*) {
    return Result(L,Video()->requestCapture(g_host,L,Lua()->argBool(L,1,true)));
}
static int Pixel(void* L, void*) {
    if (!Lua()->checkStack(L,6)) return 0;
    double x = Lua()->argNumber(L,1,-1), y = Lua()->argNumber(L,2,-1);
    SwVideoPixel pixel = {};
    bool valid = std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 &&
        x <= (std::numeric_limits<int>::max)() && y <= (std::numeric_limits<int>::max)();
    bool ready = valid && Video()->getPixel(g_host,L,int(x),int(y),&pixel);
    Lua()->pushNumber(L,pixel.r); Lua()->pushNumber(L,pixel.g);
    Lua()->pushNumber(L,pixel.b); Lua()->pushNumber(L,pixel.a);
    Lua()->pushBool(L,ready); Lua()->pushNumber(L,pixel.captureSequence);
    return 6;
}
static bool Reject(const SwModHost* host, const char* reason) {
    if (host && host->log) host->log(host->modId,reason);
    return false;
}
SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2) return Reject(host,"ERROR: video_fx requires loader SDK v2");
    if (!host->api || host->api->abiVersion != SW_ENGINE_API_VERSION)
        return Reject(host,"ERROR: missing/incompatible loader engine API");
    if (host->api->structSize < offsetof(SwEngineApi,video) + sizeof(host->api->video))
        return Reject(host,"ERROR: loader engine API has no video extension; update iphlpapi.dll");
    if (!host->api->lua) return Reject(host,"ERROR: central Lua profile unavailable");
    if (!host->api->video || host->api->video->structSize < sizeof(SwVideoApi))
        return Reject(host,"ERROR: native video API/profile unavailable; see loader diagnostics");
    if (!host->api->video->enable(host))
        return Reject(host,"ERROR: native video service startup rejected; see preceding Lua/video diagnostics (legacy mc_lua_qol is incompatible)");
    g_host = host;
    const SwLuaFunction functions[] = {
        {"setOffset",&Offset,nullptr},{"setScale",&Scale,nullptr},{"setTint",&Tint,nullptr},
        {"setFilter",&Filter,nullptr},{"reset",&Reset,nullptr},
        {"requestCapture",&Request,nullptr},{"getPixel",&Pixel,nullptr}
    };
    if (!Lua()->registerLibrary(host,"video",functions,sizeof(functions)/sizeof(functions[0]))) {
        Video()->disable(host); g_host = nullptr;
        return Reject(host,"ERROR: unable to register Lua library video (service unavailable or name already owned)");
    }
    if (host->log) host->log(host->modId,"video library registered through the loader API; recreate Lua VMs to use it (experimental)");
    return true;
}
SW_MOD_EXPORT void SwModUnload(void) {
    if (g_host) Video()->disable(g_host);
    g_host = nullptr;
}
