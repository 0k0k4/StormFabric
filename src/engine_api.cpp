#include "engine_api.h"
#include "engine_profile.h"
#include "native_lua.h"
#include "native_video.h"
#include "mod_runtime.h"
#include "mod_native_ui.h"
#include "mod_storage.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <windows.h>

namespace swml {
namespace {
sw::Api g_game;
std::set<const SwModHost*> g_owners;
std::mutex g_ownerMutex;
std::set<const SwModHost*> g_qolOwners;
SwModHost g_serviceHost = {}; // never points into a plugin's instance or strings
bool g_luaAvailable = false;
bool g_luaStarted = false;
bool g_videoAvailable = false;

struct ClickToken {
    const SwModHost* owner;
    SwUiClickFn callback;
    void* userData;
};
// A native tree can outlive a plugin. Its callback always points into this DLL, and its
// loader-owned token remains valid even after ReleaseEngineApiOwner invalidates the plugin.
std::vector<std::unique_ptr<ClickToken>> g_clickTokens;

template<class T> T& Field(void* p, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset);
}
bool ValidOwner(const SwModHost* owner) { std::lock_guard<std::mutex> lock(g_ownerMutex);return owner && g_owners.count(owner) != 0; }

bool SafeClick(SwUiClickFn fn, void* ctx, void* data) {
    __try { fn(ctx, data); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ClickThunk(void* ctx, void* data) {
    auto* token = static_cast<ClickToken*>(data);
    if (!token || !token->owner || !token->callback) return;
    if (!SafeClick(token->callback, ctx, token->userData)) {
        token->callback = nullptr;
        LogError("Engine API: plugin UI callback crashed; disabled");
    }
}
void BindClick(void* element, const SwModHost* owner, SwUiClickFn callback, void* data, bool round) {
    if (!callback) return;
    auto token = std::make_unique<ClickToken>();
    token->owner = owner;
    token->callback = callback;
    token->userData = data;
    Field<void*>(element, round ? sw::off::kBtnCallback : sw::off::kRectBtnCallback) = reinterpret_cast<void*>(&ClickThunk);
    Field<void*>(element, round ? sw::off::kBtnUserData : sw::off::kRectBtnUserData) = token.get();
    g_clickTokens.push_back(std::move(token));
}
void Add(void* ctx, void* parent, void* child) { if (ctx && parent && child) g_game.add_child(parent, ctx, child); }
bool LayoutValid(const SwUiLayout* l) { return l && l->anchor >= 0 && l->anchor <= 8; }
void SetLayout(void* element, const SwUiLayout* l) {
    if (!element || !LayoutValid(l)) return;
    Field<float>(element, sw::off::kX) = l->x;
    Field<float>(element, sw::off::kY) = l->y;
    Field<float>(element, sw::off::kW) = l->width;
    Field<float>(element, sw::off::kH) = l->height;
    Field<int>(element, sw::off::kAnchor) = l->anchor;
}
bool GetLayout(void* element, SwUiLayout* l) {
    if (!element || !l) return false;
    *l = {Field<float>(element, sw::off::kX), Field<float>(element, sw::off::kY),
          Field<float>(element, sw::off::kW), Field<float>(element, sw::off::kH), Field<int>(element, sw::off::kAnchor)};
    return true;
}
void* Composite(void* ctx, void* parent, const SwUiLayout* l) {
    if (!ctx || !LayoutValid(l)) return nullptr;
    void* p = g_game.op_new(0x58);
    if (!p) return nullptr;
    memset(p, 0, 0x58);
    *static_cast<void**>(p) = g_game.composite_vt;
    SetLayout(p, l);
    Field<uint8_t>(p, sw::off::kVisible) = 1;
    Field<float>(p, sw::off::kScale) = 1.f;
    Field<uint8_t>(p, 0x48) = l->width < 0;
    Field<uint8_t>(p, 0x49) = l->height < 0;
    Add(ctx, parent, p);
    return p;
}
void* Text(void* ctx, void* parent, const SwUiLayout* l, const char* text, int align,
           uint32_t color, float size, float spacing) {
    if (!ctx || !LayoutValid(l) || align < 0 || align > 8) return nullptr;
    sw::EStr s = {};
    sw::StrMake(g_game, s, text ? text : "");
    void* p = g_game.make_text(g_game.op_new(0xc8), l->x, l->y, l->width, l->height,
                              l->anchor, &s, align, color, size, spacing, true);
    Add(ctx, parent, p);
    return p;
}
void* Rectangle(void* ctx, void* parent, const SwUiLayout* l, uint32_t color) {
    if (!ctx || !LayoutValid(l)) return nullptr;
    void* p = g_game.make_rect(g_game.op_new(0x60), l->x, l->y, l->width, l->height, l->anchor, color);
    Add(ctx, parent, p);
    return p;
}
void* Button(const SwModHost* owner, void* ctx, void* parent, float x, float y, int anchor,
             const char* icon, const char* label, float size, SwUiClickFn callback, void* data) {
    if (!ValidOwner(owner) || !ctx || anchor < 0 || anchor > 8 || size <= 0) return nullptr;
    sw::EStr ic = {}, lb = {};
    sw::StrMake(g_game, ic, icon ? icon : "");
    sw::StrMake(g_game, lb, label ? label : "");
    void* p = g_game.make_button(ctx, x, y, anchor, &ic, &lb, 1, 0, .42f, size);
    Add(ctx, parent, p);
    BindClick(p, owner, callback, data, true);
    return p;
}
void* RectangleButton(const SwModHost* owner, void* ctx, void* parent, const SwUiLayout* l,
                      uint32_t normal, uint32_t hover, uint32_t pressed, bool rounded,
                      SwUiClickFn callback, void* data) {
    if (!ValidOwner(owner) || !ctx || !LayoutValid(l)) return nullptr;
    void* p = g_game.make_rect_button(g_game.op_new(0xc8), l->x, l->y, l->width, l->height,
                                     l->anchor, 0, normal, hover, pressed, rounded);
    Add(ctx, parent, p);
    BindClick(p, owner, callback, data, false);
    return p;
}
void* Checkbox(void* ctx, void* parent, float x, float y, float size, int anchor) {
    if (!ctx || anchor < 0 || anchor > 8 || size <= 0) return nullptr;
    void* p = g_game.make_checkbox(g_game.op_new(0x78), x, y, size, anchor);
    Add(ctx, parent, p);
    return p;
}
void* Heading(void* ctx, void* parent, float x, float y, float width, int anchor,
              const char* text, float size, float spacing) {
    if (!ctx || anchor < 0 || anchor > 8) return nullptr;
    sw::EStr s = {};
    sw::StrMake(g_game, s, text ? text : "");
    void* p = g_game.make_heading(g_game.op_new(0x80), x, y, width, anchor, &s, size, spacing);
    Add(ctx, parent, p);
    return p;
}
void* Scroll(void* ctx, void* parent, const SwUiLayout* l, float spacing) {
    if (!ctx || !LayoutValid(l)) return nullptr;
    void* p = g_game.make_scroll(g_game.op_new(0xb0), l->x, l->y, l->width, l->height, l->anchor, spacing);
    Add(ctx, parent, p);
    return p;
}
void ScrollAdd(void* ctx, void* scroll, void* item) { if (ctx && scroll && item) g_game.scroll_add(scroll, ctx, item); }
void Remove(void* ctx, void* parent, void* child) {
    if (!ctx || !parent || !child) return;
    Field<void*>(ctx, sw::off::kCtxHovered) = nullptr;
    Field<void*>(ctx, sw::off::kCtxFocused) = nullptr;
    g_game.remove_child(parent, ctx, child);
}
void SetText(void* ctx, void* element, const char* text) {
    if (!ctx || !element) return;
    sw::EStr s = {};
    sw::StrMake(g_game, s, text ? text : "");
    g_game.set_text(element, ctx, &s);
    sw::StrRelease(g_game, s);
}
void Visible(void* element, bool visible) { if (element) Field<uint8_t>(element, sw::off::kVisible) = visible; }
void Alpha(void* ctx, void* element, uint8_t alpha) {
    if (!ctx || !element) return;
    auto** vt = *static_cast<void***>(element);
    reinterpret_cast<void (*)(void*, void*, uint8_t)>(vt[sw::slot::kSetAlpha])(element, ctx, alpha);
}
void Active(void* ctx, void* element, bool active) {
    if (!ctx || !element) return;
    auto** vt = *static_cast<void***>(element);
    reinterpret_cast<void (*)(void*, void*, bool)>(vt[37])(element, ctx, active);
}
bool Checked(void* p) { return p && Field<uint8_t>(p, sw::off::kCheckboxState) != 0; }
void SetChecked(void* p, bool checked) { if (p) Field<uint8_t>(p, sw::off::kCheckboxState) = checked; }
size_t Localized(int id, char* out, size_t cap) {
    // Unknown localization IDs have no proven bounds-safe engine behavior; reject negatives.
    if (id < 0) { if (out && cap) out[0] = 0; return 0; }
    sw::EStr s = {};
    g_game.localized(&s, id);
    size_t n = s.len;
    if (out && cap) { size_t copy = (std::min)(n, cap - 1); if (copy) memcpy(out, s.data, copy); out[copy] = 0; }
    sw::StrRelease(g_game, s);
    return n;
}

bool StartLua(const SwModHost* owner) {
    if (!ValidOwner(owner) || !g_luaAvailable) return false;
    if (!g_luaStarted) {
        g_serviceHost.gameBase = g_game.base;
        g_serviceHost.log = owner->log;
        g_serviceHost.modId = "loader.lua";
        if (!EnableNativeLua(&g_serviceHost)) {
            g_luaAvailable = false;
            LogError("Engine API: Lua service startup rejected; native profile/hooks changed since initialization (check for legacy plugins installing their own Lua hooks)");
            return false;
        }
        g_luaStarted = true;
    }
    SetNativeLuaEnabled(!g_qolOwners.empty());
    return true;
}
bool EnableQol(const SwModHost* owner) {
    if (!StartLua(owner)) return false;
    g_qolOwners.insert(owner);
    SetNativeLuaEnabled(true);
    return true;
}
void DisableQol(const SwModHost* owner) {
    g_qolOwners.erase(owner);
    SetNativeLuaEnabled(!g_qolOwners.empty());
}
bool RegisterLibrary(const SwModHost* owner, const char* name, const SwLuaFunction* funcs, size_t count) {
    return StartLua(owner) && NativeLuaPrimitives()->registerLibrary(owner, name, funcs, count);
}
SwResult SubscribeRuntime(const SwModHost* owner,uint64_t mask,SwEventCallback fn,void* data,SwHandle* out) {
    if(!ValidOwner(owner))return SW_INVALID_OWNER;
    if(!fn||!out||!mask)return SW_INVALID_ARGUMENT;
    const uint64_t allowed=SW_EVENT_MASK(SW_EVENT_UI_UPDATE)|SW_EVENT_MASK(SW_EVENT_UI_READY)|
        SW_EVENT_MASK(SW_EVENT_UI_CLOSED)|SW_EVENT_MASK(SW_EVENT_ACTION)|
        SW_EVENT_MASK(SW_EVENT_LUA_CREATED)|SW_EVENT_MASK(SW_EVENT_LUA_DESTROYED);
    if(mask&~allowed)return SW_UNAVAILABLE;
    if(mask & (SW_EVENT_MASK(SW_EVENT_LUA_CREATED)|SW_EVENT_MASK(SW_EVENT_LUA_DESTROYED))) {
        if(!StartLua(owner))return SW_UNAVAILABLE;
    }
    return RuntimePrimitives()->subscribe(owner,mask,fn,data,out);
}
bool EnableVideo(const SwModHost* owner) {
    if (!ValidOwner(owner)) { LogError("Video: enable rejected, unregistered plugin owner"); return false; }
    if (!g_videoAvailable) { LogError("Video: enable rejected, native video profile unavailable"); return false; }
    if (!StartLua(owner)) { LogError("Video: enable rejected, central Lua service unavailable"); return false; }
    if (!InstallNativeVideo(g_game.base)) { LogError("Video: enable rejected, native hook installation failed"); return false; }
    if (!NativeVideoPrimitives()->enable(owner)) { LogError("Video: enable rejected, native owner activation failed"); return false; }
    return true;
}
void DisableVideo(const SwModHost* owner) {
    ReleaseNativeVideoOwner(owner);
}

const SwUiApi g_ui = {sizeof(SwUiApi), &Composite, &Text, &Rectangle, &Button, &RectangleButton,
    &Checkbox, &Heading, &Scroll, &ScrollAdd, &Add, &Remove, &SetText, &Visible, &Alpha, &Active,
    &Checked, &SetChecked, &GetLayout, &SetLayout, &Localized};
SwLuaApi g_lua = {};
SwVideoApi g_video = {};
SwRuntimeApi g_runtime = {};
SwEngineApi g_engine = {SW_ENGINE_API_VERSION, sizeof(SwEngineApi), &g_ui, nullptr, nullptr};
}

void InitializeEngineApi(const sw::Api& api) {
    g_game = api;
    g_luaAvailable = ValidateLuaProfile(api.base);
    g_lua = *NativeLuaPrimitives();
    g_lua.extras = NativeLuaExtras();
    g_lua.enableQol = &EnableQol;
    g_lua.disableQol = &DisableQol;
    g_lua.registerLibrary = &RegisterLibrary;
    g_engine.lua = g_luaAvailable ? &g_lua : nullptr;
    g_videoAvailable = g_luaAvailable && ValidateVideoProfile(api.base);
    g_video = *NativeVideoPrimitives();
    g_video.enable = &EnableVideo;
    g_video.disable = &DisableVideo;
    g_engine.video = g_videoAvailable ? &g_video : nullptr;
    g_runtime = *RuntimePrimitives();
    g_runtime.subscribe = &SubscribeRuntime;
    g_engine.runtime = &g_runtime;
    g_engine.input = InputPrimitives();
    g_engine.nativeUi = NativeUiPrimitives();
    g_engine.storage = GetStorageApi();
    g_engine.world = nullptr;
    SetRuntimeCapabilities(SW_CAP_UI_EVENTS | SW_CAP_MAIN_THREAD_TASKS |
        SW_CAP_KEYBOARD_ACTIONS | SW_CAP_MOD_STORAGE |
        (g_luaAvailable ? SW_CAP_LUA_EXTRAS | SW_CAP_LUA_EVENTS : 0));
    LogInfo("Engine API v%u initialized (UI available, Lua %s, video %s)", g_engine.abiVersion,
            g_luaAvailable ? "validated" : "unavailable", g_videoAvailable ? "validated" : "unavailable");
}
const SwEngineApi* GetEngineApi() { return &g_engine; }
bool IsEngineApiOwner(const SwModHost* owner) { return ValidOwner(owner); }
void RegisterEngineApiOwner(const SwModHost* owner) {
    if (!owner) return;
    { std::lock_guard<std::mutex> lock(g_ownerMutex);g_owners.insert(owner); }
    RegisterRuntimeOwner(owner);
    if (RegisterModStorageOwner(owner) != SW_STORAGE_OK)
        LogWarn("P0: mod storage unavailable for this owner");
}
void ReleaseEngineApiOwner(const SwModHost* owner) {
    ReleaseModUiOwner(owner);
    ReleaseRuntimeOwner(owner);
    ReleaseModStorageOwner(owner);
    for (auto& token : g_clickTokens) {
        if (token->owner == owner) { token->owner = nullptr; token->callback = nullptr; token->userData = nullptr; }
    }
    ReleaseNativeLuaOwner(owner);
    DisableVideo(owner);
    DisableQol(owner);
    { std::lock_guard<std::mutex> lock(g_ownerMutex);g_owners.erase(owner); }
}
}
