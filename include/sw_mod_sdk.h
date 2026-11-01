// SDK minimaliste pour les mods C++ natifs de Stormworks (StormworksModLoader).
//
// Une DLL de mod peut :
//   1. Fonctionner uniquement avec un DllMain standard (aucun export requis).
//   2. Exporter optionnellement (en C, `extern "C" __declspec(dllexport)`) :
//      - bool SwModInit(const SwModHost* host);   // appelé après le chargement (hors Loader Lock)
//      - void SwModTick(void* uiContext);         // appelé à chaque image par le thread principal
//      - void SwModUnload(void);                  // appelé juste avant FreeLibrary (désactivation)
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SW_MOD_SDK_VERSION 2
#define SW_ENGINE_API_VERSION 1

typedef struct SwModHost SwModHost;
typedef struct SwEngineApi SwEngineApi;
typedef struct SwLuaExtrasApi SwLuaExtrasApi;
typedef void* SwUiHandle;
typedef void (*SwUiClickFn)(void* context, void* userData);

// Coordinates use Stormworks UI units (screen height = 2), +Y points up.
// Colors are ABGR. Anchors/alignment use the native 3x3 grid, 0..8.
typedef struct SwUiLayout {
    float x, y, width, height;
    int anchor;
} SwUiLayout;

typedef struct SwUiApi {
    uint32_t structSize;
    SwUiHandle (*composite)(void* context, SwUiHandle parent, const SwUiLayout* layout);
    // fontSize is a native font scalar, NOT a layout height; existing native
    // menu/notification text uses approximately .00044f..00072f with spacing .8f.
    SwUiHandle (*text)(void* context, SwUiHandle parent, const SwUiLayout* layout,
                       const char* utf8, int alignment, uint32_t abgr,
                       float fontSize, float lineSpacing);
    SwUiHandle (*rectangle)(void* context, SwUiHandle parent, const SwUiLayout* layout, uint32_t abgr);
    SwUiHandle (*button)(const SwModHost* owner, void* context, SwUiHandle parent,
                         float x, float y, int anchor, const char* icon, const char* label,
                         float size, SwUiClickFn callback, void* userData);
    SwUiHandle (*rectangleButton)(const SwModHost* owner, void* context, SwUiHandle parent,
                                  const SwUiLayout* layout, uint32_t normal, uint32_t hover,
                                  uint32_t pressed, bool rounded, SwUiClickFn callback, void* userData);
    SwUiHandle (*checkbox)(void* context, SwUiHandle parent, float x, float y, float size, int anchor);
    SwUiHandle (*heading)(void* context, SwUiHandle parent, float x, float y, float width,
                          int anchor, const char* text, float size, float spacing);
    SwUiHandle (*scroll)(void* context, SwUiHandle parent, const SwUiLayout* layout, float spacing);
    void (*scrollAdd)(void* context, SwUiHandle scroll, SwUiHandle item);
    void (*addChild)(void* context, SwUiHandle parent, SwUiHandle child);
    void (*removeChild)(void* context, SwUiHandle parent, SwUiHandle child);
    void (*setText)(void* context, SwUiHandle element, const char* utf8);
    void (*setVisible)(SwUiHandle element, bool visible);
    void (*setAlpha)(void* context, SwUiHandle element, uint8_t alpha);
    void (*setActive)(void* context, SwUiHandle element, bool active);
    bool (*getChecked)(SwUiHandle checkbox);
    void (*setChecked)(SwUiHandle checkbox, bool checked);
    bool (*getLayout)(SwUiHandle element, SwUiLayout* layout);
    void (*setLayout)(SwUiHandle element, const SwUiLayout* layout);
    size_t (*localizedText)(int id, char* outUtf8, size_t capacity);
} SwUiApi;

typedef int (*SwLuaCallback)(void* luaState, void* userData);
typedef struct SwLuaFunction {
    const char* name;
    SwLuaCallback callback;
    void* userData;
} SwLuaFunction;

// Callable from a microcontroller Lua callback, with the same arguments/results as the
// corresponding Lua QoL function. The loader supplies the internal input/screen context.
typedef int (*SwLuaNativeFn)(void* luaState);
typedef struct SwLuaQolApi {
    uint32_t structSize;
    SwLuaNativeFn getNumbers, getNumberRange, getBools, getBoolRange;
    SwLuaNativeFn getAllNumbers, getAllBools, getPulse, getToggle;
    SwLuaNativeFn setNumbers, setBools, copyInputs, clearOutputs;
    SwLuaNativeFn drawTextCentered, drawTextRight, drawArc, drawGauge, drawPoly, drawPolyF;
    SwLuaNativeFn drawButton, setColorHex, setColorHSV;
    SwLuaNativeFn clamp, lerp, invLerp, remap, round, sign, wrap, deadzone, smooth, pid, resetPid;
    SwLuaNativeFn log, notify;
} SwLuaQolApi;

typedef struct SwLuaApi {
    uint32_t structSize;
    // Reference-counted service. Hooks and native closures live in the loader, never in a mod.
    bool (*enableQol)(const SwModHost* owner);
    void (*disableQol)(const SwModHost* owner);
    // Applied to newly created microcontroller VMs. Names/functions are copied by the loader.
    // Unloading the owner makes existing closures inert before its DLL is freed.
    bool (*registerLibrary)(const SwModHost* owner, const char* name,
                            const SwLuaFunction* functions, size_t count);
    int (*getTop)(void* luaState);
    double (*argNumber)(void* luaState, int index, double defaultValue);
    bool (*argBool)(void* luaState, int index, bool defaultValue);
    size_t (*argString)(void* luaState, int index, char* outUtf8, size_t capacity);
    bool (*checkStack)(void* luaState, int additionalSlots);
    void (*pushNumber)(void* luaState, double value);
    void (*pushBool)(void* luaState, bool value);
    const SwLuaQolApi* qol;
    const SwLuaExtrasApi* extras; // appended: check structSize before reading
} SwLuaApi;

// Native monitor video service. Only callable from an onDraw Lua callback for a vehicle
// monitor. Effects reset for each monitor draw; screen.* and tactile coordinates are unchanged.
typedef struct SwVideoPixel {
    uint8_t r, g, b, a;
    uint32_t captureSequence; // latest completed source capture, not the current GPU image
} SwVideoPixel;
typedef struct SwVideoApi {
    uint32_t structSize;
    bool (*enable)(const SwModHost* owner);
    void (*disable)(const SwModHost* owner);
    bool (*setOffset)(const SwModHost* owner, void* luaState, double x, double y);
    bool (*setScale)(const SwModHost* owner, void* luaState, double x, double y);
    bool (*setTint)(const SwModHost* owner, void* luaState, double r, double g, double b, double a);
    bool (*setFilter)(const SwModHost* owner, void* luaState, double grayscale, double invert);
    bool (*reset)(const SwModHost* owner, void* luaState);
    bool (*requestCapture)(const SwModHost* owner, void* luaState, bool enabled);
    bool (*getPixel)(const SwModHost* owner, void* luaState, int x, int y, SwVideoPixel* out);
} SwVideoApi;

#include "sw_sdk_p0.h"
#include "sw_sdk_storage.h"
#include "sw_sdk_lua.h"

struct SwEngineApi {
    uint32_t abiVersion;
    uint32_t structSize;
    const SwUiApi* ui;
    const SwLuaApi* lua; // NULL if the central Lua profile failed validation
    const SwVideoApi* video; // appended; check structSize before accessing, NULL on unsupported build
    const SwRuntimeApi* runtime;
    const SwInputApi* input;
    const SwNativeUiApi* nativeUi;
    const SwStorageApi* storage;
    const SwWorldApi* world; // NULL until native world bindings are certified
};

struct SwModHost {
    uint32_t sdkVersion;          // SW_MOD_SDK_VERSION (2)
    void* gameBase;               // Adresse de base réelle (ASLR) de stormworks64.exe
    const char* modId;            // Identifiant unique du mod (UTF-8)
    const char* modName;          // Nom d'affichage du mod (UTF-8)
    const char* modVersion;       // Version du mod (UTF-8)
    const char* modAuthor;        // Auteur du mod (UTF-8)
    const char* modDir;           // Chemin complet du dossier du mod (UTF-8)
    void* uiContext;              // Contexte UI interne du moteur (peut être nul à l'init précoce)
    void* mainMenuInterface;      // Instance de c_ui_main_menu_interface (peut être nulle)
    void (*log)(const char* modId, const char* message);  // Écrit dans StormworksModLoader.log
    // Appended: SDK v1 fields retain their offsets. Prefer this API over gameBase arithmetic.
    const SwEngineApi* api;
};

typedef bool (*SwModInitFn)(const SwModHost* host);
typedef void (*SwModTickFn)(void* uiContext);
typedef void (*SwModUnloadFn)(void);

#if defined(_WIN32) && defined(__cplusplus)
#define SW_MOD_EXPORT extern "C" __declspec(dllexport)
#elif defined(_WIN32)
#define SW_MOD_EXPORT __declspec(dllexport)
#else
#define SW_MOD_EXPORT extern "C"
#endif

#ifdef __cplusplus
}
#endif
