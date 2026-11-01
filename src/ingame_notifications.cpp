#include "ingame_notifications.h"
#include "engine_api.h"
#include "notification_overlay.h"
#include "log.h"
#include "engine_bindings.h"
#include "hook_transaction.h"
#include "mod_native_ui.h"
#include "mod_runtime.h"
#include <windows.h>

namespace swml {
namespace {
// Slot 3 forwards RCX/RDX/R8 (not floats); slot 0 is MSVC scalar deleting destructor.
// Evidence: tools/ghidra/notifications_abi.txt and notifications_lifecycle.txt.
// RDX in update is application state, unlike RDX in init/teardown (native UI context).
using InitFn = void (*)(void*, void*);
using UpdateFn = void (*)(void*, void*, void*);
using DestroyFn = void* (*)(void*, unsigned);
struct Target {
    const char* name;
    uint32_t table, update, destroy, init;
    UpdateFn originalUpdate = nullptr;
    DestroyFn originalDestroy = nullptr;
    InitFn originalInit = nullptr;
    InitFn originalTeardown = nullptr;
    NotificationInterfaces interfaces;
};
Target g_hud = {"HUD", 0xb423c0, 0x5c78b0, 0x247ed0, 0x5c7020};
Target g_editor = {"microcontroller editor", 0xb71d60, 0x642000, 0x643380, 0x640e80};
SwUiSurface SurfaceOf(Target& target) { return &target == &g_hud ? SW_SURFACE_HUD : SW_SURFACE_EDITOR; }
bool Focused() { DWORD pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId(); }

void Initialize(Target& target, void* self, void* context) {
    target.interfaces.Forget(self);
    target.originalInit(self, context);
    target.interfaces.Initialize(self, context);
    InitializeModUi(SurfaceOf(target), self, context);
    LogDebug("Native notifications: %s initialized (interface %p, UI context %p)", target.name, self, context);
}
void Teardown(Target& target, void* self, void* context) {
    ForgetModUi(SurfaceOf(target), self);
    target.interfaces.Forget(self);
    target.originalTeardown(self, context);
}
void Update(Target& target, void* self, void* gameState, void* frame) {
    target.originalUpdate(self, gameState, frame);
    target.interfaces.Update(*GetEngineApi()->ui, self);
    // UI pulses are not simulation ticks. Do not expose gameState as UI context.
    PumpModRuntime(SurfaceOf(target), Focused(), ModUiKeyboardCaptured(SurfaceOf(target), self));
    UpdateModUi(SurfaceOf(target), self);
}
void* Destroy(Target& target, void* self, unsigned flags) {
    ForgetModUi(SurfaceOf(target), self);
    target.interfaces.Forget(self); // native destructor owns and destroys the children
    return target.originalDestroy(self, flags);
}
void HudInit(void* self, void* ctx) { Initialize(g_hud, self, ctx); }
void EditorInit(void* self, void* ctx) { Initialize(g_editor, self, ctx); }
void HudTeardown(void* self, void* ctx) { Teardown(g_hud, self, ctx); }
void EditorTeardown(void* self, void* ctx) { Teardown(g_editor, self, ctx); }
void HudUpdate(void* self, void* state, void* frame) { Update(g_hud, self, state, frame); }
void EditorUpdate(void* self, void* state, void* frame) { Update(g_editor, self, state, frame); }
void* HudDestroy(void* self, unsigned flags) { return Destroy(g_hud, self, flags); }
void* EditorDestroy(void* self, unsigned flags) { return Destroy(g_editor, self, flags); }

bool Prepare(Target& t, uint8_t* base, void* init, void* teardown, void* update, void* destroy, SlotPatch* patches) {
    auto** vt = reinterpret_cast<void**>(base + EngineRva(base, t.table));
    if (vt[0] != base + EngineRva(base, t.destroy) || vt[1] != base + EngineRva(base, t.init) ||
        vt[2] != base + EngineRva(base, 0x241bc0) || vt[3] != base + EngineRva(base, t.update)) {
        LogError("Notifications: %s vtable mismatch; no hook installed", t.name);
        return false;
    }
    patches[0] = {&vt[0],vt[0],destroy,reinterpret_cast<void**>(&t.originalDestroy)};
    patches[1] = {&vt[1],vt[1],init,reinterpret_cast<void**>(&t.originalInit)};
    patches[2] = {&vt[2],vt[2],teardown,reinterpret_cast<void**>(&t.originalTeardown)};
    patches[3] = {&vt[3],vt[3],update,reinterpret_cast<void**>(&t.originalUpdate)};
    return true;
}
}
bool InstallIngameNotifications(const sw::Api& game) {
    // ResolveApi has already validated the supported PE build and native widget API.
    if (!sw::ValidateIngameNotificationProfile(game.base)) {
        LogError("Native notifications: in-game profile validation failed; hooks disabled");
        return false;
    }
    SlotPatch patches[8];
    if (!Prepare(g_hud, game.base, reinterpret_cast<void*>(&HudInit), reinterpret_cast<void*>(&HudTeardown),
        reinterpret_cast<void*>(&HudUpdate), reinterpret_cast<void*>(&HudDestroy), patches) ||
        !Prepare(g_editor, game.base, reinterpret_cast<void*>(&EditorInit), reinterpret_cast<void*>(&EditorTeardown),
        reinterpret_cast<void*>(&EditorUpdate), reinterpret_cast<void*>(&EditorDestroy), patches+4) ||
        !PatchSlots(patches, _countof(patches))) return false;
    LogInfo("Native notifications: HUD and editor lifecycle hooks installed (UI context from init)");
    SetRuntimeCapabilities(RuntimePrimitives()->capabilities() | SW_CAP_NATIVE_HUD);
    return true;
}
}
