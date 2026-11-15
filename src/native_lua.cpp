// Loader-owned Microcontroller Lua service (version-specific implementation).
//
// Existing QoL functionality migrated from mc_lua_qol (runtime migration not yet tested):
//   - 1A. Lecture groupée : input.getNumbers(...), input.getNumberRange(a,b), input.getBools(...),
//         input.getBoolRange(a,b), input.getAllNumbers(), input.getAllBools()
//   - 1B. Écriture groupée : output.setNumbers(startChan, ...), output.setBools(startChan, ...),
//         output.clear()
//   - 1C. Pulse & Toggle intégrés : input.getPulse(chan), input.getToggle(chan, [init])
//   - 1D. Passthrough composite : output.copyInputs()
//   - 2A. Texte aligné : screen.drawTextCentered(cx, cy, txt), screen.drawTextRight(rx, y, txt)
//   - 2B. Jauges & Polygones : screen.drawArc(cx, cy, r, a0, a1, [segs]),
//         screen.drawGauge(cx, cy, r, ratio, [a0, a1]), screen.drawPoly(...), screen.drawPolyF(...)
//   - 2C. Bouton tactile : screen.drawButton(x, y, w, h, txt, tx, ty, pressed, [fillIdle]) -> bool
//   - 2D. Couleurs Hex & HSV : screen.setColorHex(hex, [a]), screen.setColorHSV(h, s, v, [a])
//   - 3A. Maths étendues : clamp, lerp, invLerp, remap, round, sign, wrap, deadzone, smooth
//   - 3B. Régulateur PID natif C++ : pid(id, sp, pv, kp, ki, kd, [min, max]), resetPid(id)
//   - 3C. Debug Terminal & Notifications UI depuis le Lua : debug.log(...), debug.notify(...)
//   - 3D. Déblocage du quota CPU Lua (50 000 instr / 10 000 ms)
//   - 3E. Déblocage de la limite de caractères des scripts Lua (4096/8192 -> 131 072 caractères)
//   - Documentation intégrée automatiquement dans l'onglet "Help" de l'éditeur Lua en jeu.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../include/sw_mod_sdk.h"
#include "../include/sw_sdk_lua.h"
#include "native_lua.h"
#include "mod_runtime.h"
#include "engine_profile.h"
#include "engine_bindings.h"
#include "hook_transaction.h"

namespace {

constexpr uint32_t kUnlockedScriptCharLimit = 131072;  // 128 Ko au lieu de 4096 / 8192
constexpr uint32_t kDefaultScriptCharLimit = 8192;
constexpr int kUnlockedHookInstrCount = 50000;         // au lieu de 100 instructions
constexpr uint32_t kUnlockedTimeoutMs = 10000;         // 10 000 ms au lieu de 1 000 ms
constexpr double kPi = 3.14159265358979323846;

const SwModHost* g_host = nullptr;
uint8_t* g_base = nullptr;
bool g_enabled = false;
bool g_installed = false;
bool g_qolEditorInstalled = false; // optional QoL/editor hooks, installed separately
thread_local uintptr_t g_upvalueOverride = 0;

struct LibraryFunction {
    const SwModHost* owner;
    std::string library;
    std::string name;
    SwLuaCallback callback;
    void* userData;
    bool addon; // installed in addon/mission script VMs instead of microcontroller VMs
    // Simulation tick subscription: no library, never installed in a VM.
    SwSimulationTickCallback tick = nullptr;
    SwHandle handle = 0;
};
// Tokens are loader-owned and retained: Lua closures can outlive a plugin or its host.
std::vector<std::unique_ptr<LibraryFunction>> g_libraryFunctions;
std::recursive_mutex g_libraryMutex;
void ApplyRegisteredLibraries(uintptr_t self, bool addon);

// `addon` VMs belong to c_playlist_lua_script: their `self` has none of the
// microcontroller input/output/screen fields, so QoL wrappers must refuse them.
struct LiveLuaVm { uintptr_t self; uint64_t generation; bool addon; };
std::map<uintptr_t, LiveLuaVm> g_liveLuaVms;
uint64_t g_nextLuaGeneration = 0;
struct LuaCallbackFrame {
    LibraryFunction* entry;
    uintptr_t state;
    uintptr_t self;
    uint64_t generation;
};
thread_local LuaCallbackFrame* g_luaCallbackFrame = nullptr;

// ---- Structures internes du moteur Stormworks & Lua 5.3 -------------------------------------

struct EStr {
    char* data;
    uint32_t len;
    uint32_t cap;
};

struct LuaReg {
    const char* name;
    void* func;
};

struct SwCompositeBuffer {
    uint32_t boolMask;   // +0x00 : 32 canaux booléens (bit 0 = canal 1 .. bit 31 = canal 32)
    float numbers[32];   // +0x04 : 32 canaux numériques (index 0 = canal 1 .. index 31 = canal 32)
};

struct SwIoContext {
    SwCompositeBuffer* inBuf;   // +0x00
    SwCompositeBuffer* outBuf;  // +0x08
    uint8_t inTick;             // +0x10 : != 0 pendant onTick()
};

// ---- Pointeurs vers les fonctions internes de stormworks64.exe (RVA résolues) ---------------

using fn_op_new_t = void* (*)(size_t);
using fn_str_reserve_t = void (*)(EStr*, uint32_t);
using fn_str_free_t = void (*)(void*);
using fn_reg_lib_t = void (*)(uintptr_t mcScript, EStr* name, LuaReg* regs, void* upval);
using fn_index2addr_t = uintptr_t (*)(uintptr_t L, int idx);
using fn_tonumber_t = int (*)(uintptr_t tv, double* out);
using fn_checkstack_t = bool (*)(uintptr_t L, int n);
using fn_newtable_t = uintptr_t (*)(uintptr_t L);
using fn_resizearray_t = void (*)(uintptr_t L, uintptr_t tbl, uint32_t nasize);
using fn_rawgeti_t = void (*)(uintptr_t L, int idx, long long n);
using fn_pushcclosure_t = void (*)(uintptr_t L, void* fn, int nup);
using fn_setfield_t = void (*)(uintptr_t L, uintptr_t tblTv, const char* key);
using fn_luaopen_math_t = int (*)(uintptr_t L, uintptr_t a, uintptr_t b, uintptr_t c);
using fn_get_time_ms_t = uint32_t (*)();
using fn_lua_hook_t = void (*)(uintptr_t L, uintptr_t ar, uintptr_t p3, uintptr_t p4);
using fn_pushlstring_t = const char* (*)(uintptr_t L, const char*, size_t);
using fn_getfield_t = int (*)(uintptr_t L, uintptr_t tblTv, const char* key);
using fn_rawrunprotected_t = int (*)(uintptr_t L, void (*)(uintptr_t, void*), void*);
using fn_pcallk_t = int (*)(uintptr_t L, int nargs, int nresults, int errfunc, intptr_t ctx, void* k);

// Primitives de rendu d'écran (screen.*)
using fn_draw_line_t = void (*)(void* batch, float* p1, float* p2, uint32_t* rgba);
using fn_draw_rect_t = void (*)(void* batch, float x1, float x2, float y1, float y2, uint32_t* rgba);
using fn_draw_rectf_t = void (*)(void* batch, float x1, float x2, float y1, float y2, uint32_t* rgba);
using fn_draw_trif_t = void (*)(void* batch, float* p1, float* p2, float* p3, uint32_t* rgba);
using fn_draw_text_t = void (*)(void* batch, uintptr_t font, float* pos, EStr* text, uint32_t* rgba);

// Primitives de l'onglet Help de l'éditeur Lua
using fn_help_heading_t = void (*)(uintptr_t tab, uintptr_t ctx, float w, EStr* title);
using fn_help_desc_t = void (*)(uintptr_t tab, uintptr_t ctx, float w, EStr* desc);
using fn_scroll_space_t = void (*)(uintptr_t scroll, uintptr_t ctx, float space);
using fn_editbox_ctor_t = void* (*)(void* mem, uint32_t x, uint32_t y, float w, float h,
                                    uint32_t anchor, uint8_t p7, uint8_t p8);
using fn_scroll_add_t = uintptr_t (*)(uintptr_t scroll, uintptr_t ctx, void* item);
using fn_editbox_set_t = void (*)(uintptr_t editbox, uintptr_t ctx, EStr* text);
using fn_scroll_fin_t = void (*)(uintptr_t scroll, uintptr_t ctx);

fn_op_new_t       g_opNew = nullptr;
fn_str_reserve_t  g_strReserve = nullptr;
fn_str_free_t     g_strFree = nullptr;
fn_reg_lib_t      g_regLib = nullptr;
fn_index2addr_t   g_index2addr = nullptr;
fn_tonumber_t     g_tonumber = nullptr;
fn_checkstack_t   g_checkstack = nullptr;
fn_newtable_t     g_newtable = nullptr;
fn_resizearray_t  g_resizearray = nullptr;
fn_rawgeti_t      g_rawgeti = nullptr;
fn_pushcclosure_t g_pushcclosure = nullptr;
fn_setfield_t     g_setfield = nullptr;
fn_luaopen_math_t g_luaopenMath = nullptr;
fn_get_time_ms_t  g_getTimeMs = nullptr;
fn_lua_hook_t     g_origTimeoutHook = nullptr;
fn_pushlstring_t g_pushlstring = nullptr;
fn_getfield_t g_getfield = nullptr;
fn_rawrunprotected_t g_rawrunprotected = nullptr;
fn_pcallk_t g_pcallk = nullptr; // optional: newer reviewed build only

fn_draw_line_t    g_drawLine = nullptr;
fn_draw_rect_t    g_drawRect = nullptr;
fn_draw_rectf_t   g_drawRectF = nullptr;
fn_draw_trif_t    g_drawTriF = nullptr;
fn_draw_text_t    g_drawText = nullptr;

fn_help_heading_t g_helpHeading = nullptr;
fn_help_desc_t    g_helpDesc = nullptr;
fn_scroll_space_t g_scrollSpace = nullptr;
fn_editbox_ctor_t g_editboxCtor = nullptr;
fn_scroll_add_t   g_scrollAdd = nullptr;
fn_editbox_set_t  g_editboxSet = nullptr;
fn_scroll_fin_t   g_scrollFin = nullptr;

// Vtables et pointeurs originaux
using McLuaInitFn = void (*)(uintptr_t self);
using McLuaDestroyFn = void (*)(uintptr_t self);
using ScriptTabInitFn = void (*)(uintptr_t self, uintptr_t ctx);
using ScriptTabUpdateFn = void (*)(uintptr_t self, uintptr_t a, uintptr_t ctx, uintptr_t b);
using HelpTabInitFn = void (*)(uintptr_t self, uintptr_t ctx);

void** g_mcLuaVt = nullptr;
void** g_scriptTabVt = nullptr;
void** g_helpTabVt = nullptr;

McLuaInitFn       g_origMcLuaInit = nullptr;
McLuaDestroyFn    g_origMcLuaDestroy = nullptr;
ScriptTabInitFn   g_origScriptTabInit = nullptr;
ScriptTabUpdateFn g_origScriptTabUpdate = nullptr;
HelpTabInitFn     g_origHelpTabInit = nullptr;

// Addon/mission script VMs (newer reviewed build only; see engine_profile.h).
void** g_addonLuaVt = nullptr;
McLuaInitFn    g_origAddonLuaInit = nullptr;
McLuaDestroyFn g_origAddonLuaDestroy = nullptr;
bool g_addonInstalled = false;

uintptr_t g_lastScriptTab = 0;

// ---- État persistant par instance de microcontrôleur (Pulse, Toggle, PID, Smooth) -----------

struct PidState {
    double integral = 0.0;
    double prevError = 0.0;
    bool initialized = false;
};

struct McInstanceState {
    uint32_t pulsePrevMask = 0;
    uint32_t togglePrevMask = 0;
    uint32_t toggleStateMask = 0;
    uint32_t toggleInitMask = 0;
    std::map<std::string, PidState> pids;
    std::map<std::string, double> smooths;
};

std::map<uintptr_t, McInstanceState> g_mcStates;

void MakeEStr(EStr& s, const char* text) {
    size_t n = strlen(text);
    s.data = nullptr;
    s.len = (uint32_t)n;
    s.cap = 0;
    g_strReserve(&s, (uint32_t)n + 1);
    memcpy(s.data, text, n + 1);
}

void FreeEStr(EStr& s) {
    if (s.data) g_strFree(s.data);
    s.data = nullptr;
    s.len = 0;
}

// ---- Helpers de pile Lua 5.3 (TValue = 16 octets : 8 octets valeur + 4 octets tag) ----------

constexpr int kUpvalueIdx1 = -0xf4629;     // LUA_REGISTRYINDEX - 1
constexpr int kRegistryIdx = -0xf4628;     // LUA_REGISTRYINDEX

inline int LuaGetTop(uintptr_t L) {
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    uintptr_t ciFunc = **(uintptr_t**)(L + 0x20);
    return (int)((top - ciFunc - 0x10) >> 4);
}

inline uintptr_t LuaGetUpvaluePtr(uintptr_t L) {
    if (g_upvalueOverride) return g_upvalueOverride;
    uintptr_t uv = g_index2addr(L, kUpvalueIdx1);
    uint32_t tag = *(uint32_t*)(uv + 8) & 0xF;
    if (tag == 2) return *(uintptr_t*)uv;
    if (tag == 7) return *(uintptr_t*)uv + 0x28;
    return 0;
}

inline double LuaArgNumber(uintptr_t L, int idx, double defVal = 0.0) {
    if (idx > LuaGetTop(L)) return defVal;
    uintptr_t tv = g_index2addr(L, idx);
    int tag = *(int*)(tv + 8);
    if (tag == 3) return *(double*)tv;
    if (tag == 0x13) return (double)(*(long long*)tv);
    double res = defVal;
    if (g_tonumber(tv, &res) == 0) return defVal;
    return res;
}

inline bool LuaArgBool(uintptr_t L, int idx, bool defVal = false) {
    if (idx > LuaGetTop(L)) return defVal;
    uintptr_t tv = g_index2addr(L, idx);
    int tag = *(int*)(tv + 8);
    if (tag == 0) return false;
    if (tag == 1) return *(int*)tv != 0;
    return true;
}

inline std::string LuaArgString(uintptr_t L, int idx) {
    if (idx > LuaGetTop(L)) return {};
    uintptr_t tv = g_index2addr(L, idx);
    int tag = *(int*)(tv + 8);
    if ((tag & 0xF) == 4) {
        uintptr_t str = *(uintptr_t*)tv;
        if (!str) return {};
        size_t n = (tag & 0x3f) == 4 ? *(uint8_t*)(str + 0xb) : *(size_t*)(str + 0x10);
        if (n > SW_LUA_MAX_STRING_BYTES) return {};
        return std::string(reinterpret_cast<const char*>(str + 0x18), n);
    }
    if (tag == 0x13) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", *(long long*)tv);
        return buf;
    }
    if (tag == 3) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4g", *(double*)tv);
        return buf;
    }
    if (tag == 1) {
        return (*(int*)tv != 0) ? "true" : "false";
    }
    if (tag == 0) {
        return "nil";
    }
    return {};
}

inline void LuaPushNumber(uintptr_t L, double val) {
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    *(double*)top = val;
    *(uint32_t*)(top + 8) = 3;  // LUA_TNUMFLT
    *(uintptr_t*)(L + 0x10) = top + 0x10;
}

inline void LuaPushBool(uintptr_t L, bool val) {
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    *(uint32_t*)top = val ? 1 : 0;
    *(uint32_t*)(top + 8) = 1;  // LUA_TBOOLEAN
    *(uintptr_t*)(L + 0x10) = top + 0x10;
}

inline float ReadInNumber(SwIoContext* io, int chan) {
    if (!io || !io->inBuf || chan < 1 || chan > 32) return 0.0f;
    return io->inBuf->numbers[chan - 1];
}

inline bool ReadInBool(SwIoContext* io, int chan) {
    if (!io || !io->inBuf || chan < 1 || chan > 32) return false;
    return (io->inBuf->boolMask & (1u << (uint32_t)(chan - 1))) != 0;
}

inline void WriteOutNumber(SwIoContext* io, int chan, float val) {
    if (!io || !io->outBuf || chan < 1 || chan > 32) return;
    io->outBuf->numbers[chan - 1] = val;
}

inline void WriteOutBool(SwIoContext* io, int chan, bool val) {
    if (!io || !io->outBuf || chan < 1 || chan > 32) return;
    uint32_t bit = 1u << (uint32_t)(chan - 1);
    if (val) io->outBuf->boolMask |= bit;
    else io->outBuf->boolMask &= ~bit;
}

// =============================================================================================
// 1. FONCTIONS QOL D'ENTRÉES / SORTIES (input.* & output.*)
// =============================================================================================

// input.getNumbers(c1, c2, ...) -> retourne n valeurs d'un coup (ou les 32 canaux si 0 arg)
int Lua_Input_GetNumbers(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    int nargs = LuaGetTop(L);
    if (nargs == 0) {
        g_checkstack(L, 36);
        for (int c = 1; c <= 32; ++c) LuaPushNumber(L, (double)ReadInNumber(io, c));
        return 32;
    }
    int count = (std::min)(nargs, 32);
    g_checkstack(L, count + 4);
    for (int i = 1; i <= count; ++i) {
        int chan = (int)LuaArgNumber(L, i, 0.0);
        LuaPushNumber(L, (double)ReadInNumber(io, chan));
    }
    return count;
}

// input.getNumberRange(startChan, endChan) -> retourne tous les canaux de startChan à endChan
int Lua_Input_GetNumberRange(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    int c1 = (std::max)(1, (std::min)(32, (int)LuaArgNumber(L, 1, 1.0)));
    int c2 = (std::max)(c1, (std::min)(32, (int)LuaArgNumber(L, 2, 32.0)));
    int count = c2 - c1 + 1;
    g_checkstack(L, count + 4);
    for (int c = c1; c <= c2; ++c) LuaPushNumber(L, (double)ReadInNumber(io, c));
    return count;
}

// input.getBools(c1, c2, ...) -> retourne plusieurs booléens d'un coup
int Lua_Input_GetBools(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    int nargs = LuaGetTop(L);
    if (nargs == 0) {
        g_checkstack(L, 36);
        for (int c = 1; c <= 32; ++c) LuaPushBool(L, ReadInBool(io, c));
        return 32;
    }
    int count = (std::min)(nargs, 32);
    g_checkstack(L, count + 4);
    for (int i = 1; i <= count; ++i) {
        int chan = (int)LuaArgNumber(L, i, 0.0);
        LuaPushBool(L, ReadInBool(io, chan));
    }
    return count;
}

// input.getBoolRange(startChan, endChan)
int Lua_Input_GetBoolRange(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    int c1 = (std::max)(1, (std::min)(32, (int)LuaArgNumber(L, 1, 1.0)));
    int c2 = (std::max)(c1, (std::min)(32, (int)LuaArgNumber(L, 2, 32.0)));
    int count = c2 - c1 + 1;
    g_checkstack(L, count + 4);
    for (int c = c1; c <= c2; ++c) LuaPushBool(L, ReadInBool(io, c));
    return count;
}

// input.getAllNumbers() -> retourne une table Lua { [1]=n1, ..., [32]=n32 }
int Lua_Input_GetAllNumbers(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    uintptr_t tbl = g_newtable(L);
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    *(uintptr_t*)top = tbl;
    *(uint32_t*)(top + 8) = 0x45;  // LUA_TTABLE | collectable
    *(uintptr_t*)(L + 0x10) = top + 0x10;
    g_resizearray(L, tbl, 32);
    uintptr_t arr = *(uintptr_t*)(tbl + 0x10);
    for (int i = 0; i < 32; ++i) {
        uintptr_t slot = arr + (size_t)i * 0x10;
        *(double*)slot = (double)ReadInNumber(io, i + 1);
        *(uint32_t*)(slot + 8) = 3;
    }
    return 1;
}

// input.getAllBools() -> retourne une table Lua { [1]=b1, ..., [32]=b32 }
int Lua_Input_GetAllBools(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    uintptr_t tbl = g_newtable(L);
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    *(uintptr_t*)top = tbl;
    *(uint32_t*)(top + 8) = 0x45;
    *(uintptr_t*)(L + 0x10) = top + 0x10;
    g_resizearray(L, tbl, 32);
    uintptr_t arr = *(uintptr_t*)(tbl + 0x10);
    for (int i = 0; i < 32; ++i) {
        uintptr_t slot = arr + (size_t)i * 0x10;
        *(uint32_t*)slot = ReadInBool(io, i + 1) ? 1 : 0;
        *(uint32_t*)(slot + 8) = 1;
    }
    return 1;
}

// input.getPulse(chan) -> true uniquement sur le front montant (false -> true) du canal
int Lua_Input_GetPulse(uintptr_t L) {
    uintptr_t ioPtr = LuaGetUpvaluePtr(L);
    auto* io = reinterpret_cast<SwIoContext*>(ioPtr);
    if (!io || !io->inTick) return 0;
    int chan = (int)LuaArgNumber(L, 1, 1.0);
    if (chan < 1 || chan > 32) {
        LuaPushBool(L, false);
        return 1;
    }
    uint32_t bit = 1u << (uint32_t)(chan - 1);
    bool cur = ReadInBool(io, chan);
    McInstanceState& st = g_mcStates[ioPtr - 0x330];
    bool was = (st.pulsePrevMask & bit) != 0;
    if (cur) st.pulsePrevMask |= bit;
    else st.pulsePrevMask &= ~bit;
    LuaPushBool(L, cur && !was);
    return 1;
}

// input.getToggle(chan, [initialState]) -> bascule l'état interne à chaque front montant
int Lua_Input_GetToggle(uintptr_t L) {
    uintptr_t ioPtr = LuaGetUpvaluePtr(L);
    auto* io = reinterpret_cast<SwIoContext*>(ioPtr);
    if (!io || !io->inTick) return 0;
    int chan = (int)LuaArgNumber(L, 1, 1.0);
    if (chan < 1 || chan > 32) {
        LuaPushBool(L, false);
        return 1;
    }
    uint32_t bit = 1u << (uint32_t)(chan - 1);
    McInstanceState& st = g_mcStates[ioPtr - 0x330];
    if ((st.toggleInitMask & bit) == 0) {
        st.toggleInitMask |= bit;
        if (LuaGetTop(L) >= 2 && LuaArgBool(L, 2, false)) {
            st.toggleStateMask |= bit;
        }
    }
    bool cur = ReadInBool(io, chan);
    bool was = (st.togglePrevMask & bit) != 0;
    if (cur) st.togglePrevMask |= bit;
    else st.togglePrevMask &= ~bit;
    if (cur && !was) {
        st.toggleStateMask ^= bit;
    }
    LuaPushBool(L, (st.toggleStateMask & bit) != 0);
    return 1;
}

// output.setNumbers(startChan, v1, v2, ...)
int Lua_Output_SetNumbers(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    int nargs = LuaGetTop(L);
    if (nargs < 2) return 0;
    int startChan = (int)LuaArgNumber(L, 1, 1.0);
    for (int i = 2; i <= nargs; ++i) {
        int chan = startChan + (i - 2);
        if (chan >= 1 && chan <= 32) {
            WriteOutNumber(io, chan, (float)LuaArgNumber(L, i, 0.0));
        }
    }
    return 0;
}

// output.setBools(startChan, b1, b2, ...)
int Lua_Output_SetBools(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick) return 0;
    int nargs = LuaGetTop(L);
    if (nargs < 2) return 0;
    int startChan = (int)LuaArgNumber(L, 1, 1.0);
    for (int i = 2; i <= nargs; ++i) {
        int chan = startChan + (i - 2);
        if (chan >= 1 && chan <= 32) {
            WriteOutBool(io, chan, LuaArgBool(L, i, false));
        }
    }
    return 0;
}

// output.copyInputs() -> copie les 32 nombres + 32 bools d'entrée vers la sortie
int Lua_Output_CopyInputs(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick || !io->inBuf || !io->outBuf) return 0;
    io->outBuf->boolMask = io->inBuf->boolMask;
    memcpy(io->outBuf->numbers, io->inBuf->numbers, sizeof(io->outBuf->numbers));
    return 0;
}

// output.clear() -> remet à zéro toutes les sorties
int Lua_Output_Clear(uintptr_t L) {
    auto* io = reinterpret_cast<SwIoContext*>(LuaGetUpvaluePtr(L));
    if (!io || !io->inTick || !io->outBuf) return 0;
    io->outBuf->boolMask = 0;
    memset(io->outBuf->numbers, 0, sizeof(io->outBuf->numbers));
    return 0;
}

// =============================================================================================
// 2. FONCTIONS QOL D'ÉCRAN (screen.*)
// =============================================================================================

inline bool GetScreenCtx(uintptr_t L, uintptr_t& outScr) {
    outScr = LuaGetUpvaluePtr(L);
    return outScr != 0 && *(uint8_t*)(outScr + 0x2a4) != 0;
}

void DrawScreenTextRaw(uintptr_t scr, float x, float y, const std::string& text) {
    EStr es;
    MakeEStr(es, text.c_str());
    float pos[2] = {std::floor(x) - 0.5f, std::floor(y) - 0.5f};
    g_drawText(*(void**)(scr + 0x2a8), *(uintptr_t*)(scr + 0x2b0), pos, &es,
               reinterpret_cast<uint32_t*>(scr + 0x280));
    FreeEStr(es);
}

// screen.drawTextCentered(cx, cy, text)
int Lua_Screen_DrawTextCentered(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    float cx = (float)LuaArgNumber(L, 1, 0.0);
    float cy = (float)LuaArgNumber(L, 2, 0.0);
    std::string txt = LuaArgString(L, 3);
    float textW = (float)((int)txt.size() * 5);
    DrawScreenTextRaw(scr, cx - textW * 0.5f, cy - 2.0f, txt);
    return 0;
}

// screen.drawTextRight(rx, y, text)
int Lua_Screen_DrawTextRight(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    float rx = (float)LuaArgNumber(L, 1, 0.0);
    float y = (float)LuaArgNumber(L, 2, 0.0);
    std::string txt = LuaArgString(L, 3);
    float textW = (float)((int)txt.size() * 5);
    DrawScreenTextRaw(scr, rx - textW, y, txt);
    return 0;
}

// screen.drawArc(cx, cy, radius, startDeg, endDeg, [segments])
int Lua_Screen_DrawArc(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    float cx = (float)LuaArgNumber(L, 1, 0.0);
    float cy = (float)LuaArgNumber(L, 2, 0.0);
    float r = (float)LuaArgNumber(L, 3, 10.0);
    double a0 = LuaArgNumber(L, 4, 0.0) * (kPi / 180.0);
    double a1 = LuaArgNumber(L, 5, 360.0) * (kPi / 180.0);
    int segs = (int)LuaArgNumber(L, 6, 16.0);
    if (segs < 3) segs = 3;
    if (segs > 64) segs = 64;

    void* batch = *(void**)(scr + 0x2a8);
    auto* rgba = reinterpret_cast<uint32_t*>(scr + 0x280);
    double step = (a1 - a0) / (double)segs;
    for (int i = 0; i < segs; ++i) {
        double t0 = a0 + step * i;
        double t1 = a0 + step * (i + 1);
        float p1[2] = {cx + (float)(std::cos(t0) * r), cy + (float)(std::sin(t0) * r)};
        float p2[2] = {cx + (float)(std::cos(t1) * r), cy + (float)(std::sin(t1) * r)};
        g_drawLine(batch, p1, p2, rgba);
    }
    return 0;
}

// screen.drawGauge(cx, cy, radius, ratio, [startDeg, endDeg])
int Lua_Screen_DrawGauge(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    float cx = (float)LuaArgNumber(L, 1, 0.0);
    float cy = (float)LuaArgNumber(L, 2, 0.0);
    float r = (float)LuaArgNumber(L, 3, 12.0);
    double ratio = (std::max)(0.0, (std::min)(1.0, LuaArgNumber(L, 4, 0.0)));
    double startDeg = LuaArgNumber(L, 5, 135.0);
    double endDeg = LuaArgNumber(L, 6, 405.0);

    double a0 = startDeg * (kPi / 180.0);
    double aEnd = (startDeg + (endDeg - startDeg) * ratio) * (kPi / 180.0);
    void* batch = *(void**)(scr + 0x2a8);
    auto* rgba = reinterpret_cast<uint32_t*>(scr + 0x280);

    int segs = 16;
    double step = (aEnd - a0) / (double)segs;
    for (int i = 0; i < segs; ++i) {
        double t0 = a0 + step * i;
        double t1 = a0 + step * (i + 1);
        float p1[2] = {cx + (float)(std::cos(t0) * r), cy + (float)(std::sin(t0) * r)};
        float p2[2] = {cx + (float)(std::cos(t1) * r), cy + (float)(std::sin(t1) * r)};
        g_drawLine(batch, p1, p2, rgba);
    }
    // Aiguille du cadran
    float center[2] = {cx, cy};
    float tip[2] = {cx + (float)(std::cos(aEnd) * (r - 1.0f)),
                    cy + (float)(std::sin(aEnd) * (r - 1.0f))};
    g_drawLine(batch, center, tip, rgba);
    return 0;
}

// screen.drawPoly(x1, y1, x2, y2, x3, y3, ...) -> contour de polygone fermé
int Lua_Screen_DrawPoly(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    int nargs = LuaGetTop(L);
    int npts = nargs / 2;
    if (npts < 2) return 0;
    void* batch = *(void**)(scr + 0x2a8);
    auto* rgba = reinterpret_cast<uint32_t*>(scr + 0x280);

    for (int i = 0; i < npts; ++i) {
        int j = (i + 1) % npts;
        float p1[2] = {(float)LuaArgNumber(L, i * 2 + 1), (float)LuaArgNumber(L, i * 2 + 2)};
        float p2[2] = {(float)LuaArgNumber(L, j * 2 + 1), (float)LuaArgNumber(L, j * 2 + 2)};
        g_drawLine(batch, p1, p2, rgba);
    }
    return 0;
}

// screen.drawPolyF(x1, y1, x2, y2, x3, y3, ...) -> polygone plein (éventail de triangles)
int Lua_Screen_DrawPolyF(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    int nargs = LuaGetTop(L);
    int npts = nargs / 2;
    if (npts < 3) return 0;
    void* batch = *(void**)(scr + 0x2a8);
    auto* rgba = reinterpret_cast<uint32_t*>(scr + 0x280);

    float p0[2] = {(float)LuaArgNumber(L, 1), (float)LuaArgNumber(L, 2) - 1.0f};
    for (int i = 1; i + 1 < npts; ++i) {
        float p1[2] = {(float)LuaArgNumber(L, i * 2 + 1),
                       (float)LuaArgNumber(L, i * 2 + 2) - 1.0f};
        float p2[2] = {(float)LuaArgNumber(L, (i + 1) * 2 + 1),
                       (float)LuaArgNumber(L, (i + 1) * 2 + 2) - 1.0f};
        g_drawTriF(batch, p0, p1, p2, rgba);
    }
    return 0;
}

// screen.drawButton(x, y, w, h, label, touchX, touchY, isPressed, [fillIdle]) -> bool
int Lua_Screen_DrawButton(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    float x = (float)LuaArgNumber(L, 1, 0.0);
    float y = (float)LuaArgNumber(L, 2, 0.0);
    float w = (float)LuaArgNumber(L, 3, 20.0);
    float h = (float)LuaArgNumber(L, 4, 9.0);
    std::string label = LuaArgString(L, 5);
    float tx = (float)LuaArgNumber(L, 6, -1.0);
    float ty = (float)LuaArgNumber(L, 7, -1.0);
    bool pressed = LuaArgBool(L, 8, false);
    bool fillIdle = LuaArgBool(L, 9, false);

    bool inside = pressed && (tx >= x && tx <= x + w && ty >= y && ty <= y + h);
    void* batch = *(void**)(scr + 0x2a8);
    auto* rgba = reinterpret_cast<uint32_t*>(scr + 0x280);

    if (inside || fillIdle) {
        float fy = y - 1.0f;
        g_drawRectF(batch, x, x + w, fy, fy + h, rgba);
    } else {
        g_drawRect(batch, x, x + w, y, y + h, rgba);
    }

    if (!label.empty()) {
        uint32_t savedColor = *rgba;
        if (inside) {
            // Inverse le texte en sombre quand le bouton est plein/cliqué pour la lisibilité
            *(uint8_t*)(scr + 0x280) = 15;
            *(uint8_t*)(scr + 0x281) = 15;
            *(uint8_t*)(scr + 0x282) = 15;
        }
        float textW = (float)((int)label.size() * 5);
        DrawScreenTextRaw(scr, x + (w - textW) * 0.5f, y + (h - 5.0f) * 0.5f, label);
        *rgba = savedColor;
    }

    LuaPushBool(L, inside);
    return 1;
}

// screen.setColorHex(hex, [alpha]) -> accepte 0xFF8800 ou "#FF8800"
int Lua_Screen_SetColorHex(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    uint32_t rgb = 0;
    uintptr_t tv = g_index2addr(L, 1);
    if ((*(uint32_t*)(tv + 8) & 0xF) == 4) {
        std::string s = LuaArgString(L, 1);
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        rgb = (uint32_t)strtoul(s.c_str(), nullptr, 16);
    } else {
        rgb = (uint32_t)(long long)LuaArgNumber(L, 1, 0.0);
    }
    int alpha = (int)LuaArgNumber(L, 2, 255.0);
    *(uint8_t*)(scr + 0x280) = (uint8_t)((rgb >> 16) & 0xFF);
    *(uint8_t*)(scr + 0x281) = (uint8_t)((rgb >> 8) & 0xFF);
    *(uint8_t*)(scr + 0x282) = (uint8_t)(rgb & 0xFF);
    *(uint8_t*)(scr + 0x283) = (uint8_t)(std::max)(0, (std::min)(255, alpha));
    return 0;
}

// screen.setColorHSV(h, s, v, [alpha])
int Lua_Screen_SetColorHSV(uintptr_t L) {
    uintptr_t scr = 0;
    if (!GetScreenCtx(L, scr)) return 0;
    double h = LuaArgNumber(L, 1, 0.0);
    double s = LuaArgNumber(L, 2, 1.0);
    double v = LuaArgNumber(L, 3, 1.0);
    int alpha = (int)LuaArgNumber(L, 4, 255.0);

    // Supporte h en 0..360 ou 0..1, et s/v en 0..1 ou 0..100
    if (h > 1.0 || h < 0.0) {
        h = std::fmod(h, 360.0);
        if (h < 0.0) h += 360.0;
        h /= 360.0;
    }
    if (s > 1.0) s /= 100.0;
    if (v > 1.0) v /= 100.0;
    s = (std::max)(0.0, (std::min)(1.0, s));
    v = (std::max)(0.0, (std::min)(1.0, v));

    double r = v, g = v, b = v;
    if (s > 0.0) {
        double hh = h * 6.0;
        int i = (int)std::floor(hh);
        double f = hh - i;
        double p = v * (1.0 - s);
        double q = v * (1.0 - s * f);
        double t = v * (1.0 - s * (1.0 - f));
        switch (i % 6) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }
    *(uint8_t*)(scr + 0x280) = (uint8_t)(r * 255.0 + 0.5);
    *(uint8_t*)(scr + 0x281) = (uint8_t)(g * 255.0 + 0.5);
    *(uint8_t*)(scr + 0x282) = (uint8_t)(b * 255.0 + 0.5);
    *(uint8_t*)(scr + 0x283) = (uint8_t)(std::max)(0, (std::min)(255, alpha));
    return 0;
}

// =============================================================================================
// 3. MATHS ÉTENDUES, PID NATIF & DEBUG (math.*, qol.*, debug.*)
// =============================================================================================

int Lua_Math_Clamp(uintptr_t L) {
    double v = LuaArgNumber(L, 1, 0.0);
    double lo = LuaArgNumber(L, 2, 0.0);
    double hi = LuaArgNumber(L, 3, 1.0);
    if (lo > hi) std::swap(lo, hi);
    LuaPushNumber(L, (std::max)(lo, (std::min)(hi, v)));
    return 1;
}

int Lua_Math_Lerp(uintptr_t L) {
    double a = LuaArgNumber(L, 1, 0.0);
    double b = LuaArgNumber(L, 2, 1.0);
    double t = LuaArgNumber(L, 3, 0.5);
    LuaPushNumber(L, a + (b - a) * t);
    return 1;
}

int Lua_Math_InvLerp(uintptr_t L) {
    double a = LuaArgNumber(L, 1, 0.0);
    double b = LuaArgNumber(L, 2, 1.0);
    double v = LuaArgNumber(L, 3, 0.0);
    double denom = b - a;
    LuaPushNumber(L, std::fabs(denom) < 1e-12 ? 0.0 : (v - a) / denom);
    return 1;
}

int Lua_Math_Remap(uintptr_t L) {
    double v = LuaArgNumber(L, 1, 0.0);
    double inMin = LuaArgNumber(L, 2, 0.0);
    double inMax = LuaArgNumber(L, 3, 1.0);
    double outMin = LuaArgNumber(L, 4, 0.0);
    double outMax = LuaArgNumber(L, 5, 1.0);
    bool doClamp = LuaArgBool(L, 6, false);
    double denom = inMax - inMin;
    double t = std::fabs(denom) < 1e-12 ? 0.0 : (v - inMin) / denom;
    if (doClamp) t = (std::max)(0.0, (std::min)(1.0, t));
    LuaPushNumber(L, outMin + (outMax - outMin) * t);
    return 1;
}

int Lua_Math_Round(uintptr_t L) {
    double v = LuaArgNumber(L, 1, 0.0);
    int decimals = (int)LuaArgNumber(L, 2, 0.0);
    if (decimals <= 0) {
        LuaPushNumber(L, std::round(v));
    } else {
        double p = std::pow(10.0, (double)(std::min)(decimals, 12));
        LuaPushNumber(L, std::round(v * p) / p);
    }
    return 1;
}

int Lua_Math_Sign(uintptr_t L) {
    double v = LuaArgNumber(L, 1, 0.0);
    LuaPushNumber(L, (v > 0.0) ? 1.0 : ((v < 0.0) ? -1.0 : 0.0));
    return 1;
}

int Lua_Math_Wrap(uintptr_t L) {
    double v = LuaArgNumber(L, 1, 0.0);
    double lo = LuaArgNumber(L, 2, -180.0);
    double hi = LuaArgNumber(L, 3, 180.0);
    double range = hi - lo;
    if (range <= 1e-12) {
        LuaPushNumber(L, lo);
        return 1;
    }
    double r = std::fmod(v - lo, range);
    if (r < 0.0) r += range;
    LuaPushNumber(L, lo + r);
    return 1;
}

int Lua_Math_Deadzone(uintptr_t L) {
    double v = LuaArgNumber(L, 1, 0.0);
    double dz = std::fabs(LuaArgNumber(L, 2, 0.05));
    if (std::fabs(v) <= dz) {
        LuaPushNumber(L, 0.0);
    } else if (dz < 1.0) {
        double s = (v > 0.0) ? 1.0 : -1.0;
        LuaPushNumber(L, s * (std::fabs(v) - dz) / (1.0 - dz));
    } else {
        LuaPushNumber(L, v);
    }
    return 1;
}

// math.smooth(id, targetVal, [alpha]) -> moyenne glissante exponentielle par ID
int Lua_Math_Smooth(uintptr_t L) {
    uintptr_t mcSelf = *(uintptr_t*)(L - 8);
    std::string id = LuaArgString(L, 1);
    double target = LuaArgNumber(L, 2, 0.0);
    double alpha = (std::max)(0.001, (std::min)(1.0, LuaArgNumber(L, 3, 0.1)));
    auto& map = g_mcStates[mcSelf].smooths;
    auto it = map.find(id);
    if (it == map.end()) {
        map[id] = target;
        LuaPushNumber(L, target);
    } else {
        it->second += (target - it->second) * alpha;
        LuaPushNumber(L, it->second);
    }
    return 1;
}

// pid(id, setpoint, processVar, kp, ki, kd, [minOut, maxOut])
int Lua_Qol_Pid(uintptr_t L) {
    uintptr_t mcSelf = *(uintptr_t*)(L - 8);
    std::string id = LuaArgString(L, 1);
    double sp = LuaArgNumber(L, 2, 0.0);
    double pv = LuaArgNumber(L, 3, 0.0);
    double kp = LuaArgNumber(L, 4, 1.0);
    double ki = LuaArgNumber(L, 5, 0.0);
    double kd = LuaArgNumber(L, 6, 0.0);
    bool hasClamp = LuaGetTop(L) >= 8;
    double minOut = LuaArgNumber(L, 7, -1e9);
    double maxOut = LuaArgNumber(L, 8, 1e9);
    if (minOut > maxOut) std::swap(minOut, maxOut);

    PidState& st = g_mcStates[mcSelf].pids[id];
    double err = sp - pv;
    double deriv = st.initialized ? (err - st.prevError) : 0.0;
    st.prevError = err;
    st.initialized = true;

    st.integral += err * ki;
    if (hasClamp) {
        st.integral = (std::max)(minOut, (std::min)(maxOut, st.integral));
    }

    double out = kp * err + st.integral + kd * deriv;
    if (hasClamp) {
        out = (std::max)(minOut, (std::min)(maxOut, out));
    }
    LuaPushNumber(L, out);
    return 1;
}

// resetPid(id)
int Lua_Qol_ResetPid(uintptr_t L) {
    uintptr_t mcSelf = *(uintptr_t*)(L - 8);
    if (LuaGetTop(L) == 0) {
        g_mcStates[mcSelf].pids.clear();
    } else {
        g_mcStates[mcSelf].pids.erase(LuaArgString(L, 1));
    }
    return 0;
}

// debug.log(...) -> écrit dans le Terminal de logs du ModLoader + StormworksModLoader.log
int Lua_Debug_Log(uintptr_t L) {
    int nargs = LuaGetTop(L);
    std::string msg;
    for (int i = 1; i <= nargs; ++i) {
        if (i > 1) msg += "  ";
        msg += LuaArgString(L, i);
    }
    if (g_host && g_host->log) {
        std::string line = "[MC-Lua] " + msg;
        g_host->log("mc_lua_qol", line.c_str());
    }
    std::string dbg = msg + "\n";
    OutputDebugStringA(dbg.c_str());
    return 0;
}

// debug.notify(levelOrTitle, titleOrMsg, [msg]) -> affiche une notification native en haut à gauche
int Lua_Debug_Notify(uintptr_t L) {
    int nargs = LuaGetTop(L);
    if (nargs < 1 || !g_host || !g_host->log) return 0;

    if (nargs >= 3) {
        std::string lvl = LuaArgString(L, 1);
        std::string title = LuaArgString(L, 2);
        std::string detail = LuaArgString(L, 3);
        std::string prefix = "INFO: [Lua] ";
        if (_stricmp(lvl.c_str(), "error") == 0 || _stricmp(lvl.c_str(), "err") == 0) {
            prefix = "ERROR: [Lua] ";
        } else if (_stricmp(lvl.c_str(), "warn") == 0 || _stricmp(lvl.c_str(), "warning") == 0) {
            prefix = "WARN: [Lua] ";
        }
        std::string full = prefix + title + " | " + detail;
        g_host->log("mc_lua_qol", full.c_str());
    } else {
        std::string title = LuaArgString(L, 1);
        std::string detail = nargs >= 2 ? LuaArgString(L, 2) : "";
        std::string full = "INFO: [Lua] " + title + (detail.empty() ? "" : (" | " + detail));
        g_host->log("mc_lua_qol", full.c_str());
    }
    return 0;
}

// =============================================================================================
// 4. HOOKS DE LA VM LUA DES MICROCONTRÔLEURS ET DE L'ÉDITEUR DE SCRIPTS
// =============================================================================================

// Hook du timeout Lua : autorise 10 000 ms au lieu de 1 000 ms avant d'appeler l'erreur native
void HkLuaTimeout(uintptr_t L, uintptr_t ar, uintptr_t p3, uintptr_t p4) {
    if (!g_enabled) {
        g_origTimeoutHook(L, ar, p3, p4);
        return;
    }
    uintptr_t mcSelf = *(uintptr_t*)(L - 8);
    uint32_t now = g_getTimeMs();
    uint32_t start = *(uint32_t*)(mcSelf + 0x368);
    if ((uint32_t)(now - start) < kUnlockedTimeoutMs) {
        return;
    }
    g_origTimeoutHook(L, ar, p3, p4);
}

void RegisterTableWithUpval(uintptr_t mcSelf, const char* tableName, LuaReg* regs, void* upval) {
    EStr nameStr;
    MakeEStr(nameStr, tableName);
    g_regLib(mcSelf, &nameStr, regs, upval);
    FreeEStr(nameStr);
}

void HkMcLuaInit(uintptr_t self) {
    // A reinitialisation invalidates the old state before the game can free/reuse it.
    {
        std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
        for (auto it = g_liveLuaVms.begin(); it != g_liveLuaVms.end();) {
            if (it->second.self == self) {
                swml::QueueModLuaEvent(SW_EVENT_LUA_DESTROYED,it->second.generation);
                it = g_liveLuaVms.erase(it);
            } else ++it;
        }
    }
    // 1. Laisse le jeu initialiser la VM Lua 5.3 et les tables de base
    g_origMcLuaInit(self);

    uint64_t vmGeneration=0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
        uintptr_t state = *(uintptr_t*)(self + 8);
        if (state) {vmGeneration=++g_nextLuaGeneration;g_liveLuaVms[state] = {self, vmGeneration};}
    }

    ApplyRegisteredLibraries(self, false);

    if (!g_enabled) {
        if(vmGeneration)swml::QueueModLuaEvent(SW_EVENT_LUA_CREATED,vmGeneration);
        return;
    }

    // Réinitialise l'état QoL associé à ce microcontrôleur
    g_mcStates.erase(self);
    uintptr_t L = *(uintptr_t*)(self + 8);
    if (!L) return;

    // 2. Table `input` enrichie (fonctions d'origine + QoL 1A / 1C)
    LuaReg inputRegs[] = {
        {"getBool",         g_base + swml::EngineRva(g_base, 0x2e9b20)},
        {"getNumber",       g_base + swml::EngineRva(g_base, 0x2e9c30)},
        {"getNumbers",      reinterpret_cast<void*>(&Lua_Input_GetNumbers)},
        {"getNumberRange",  reinterpret_cast<void*>(&Lua_Input_GetNumberRange)},
        {"getBools",        reinterpret_cast<void*>(&Lua_Input_GetBools)},
        {"getBoolRange",    reinterpret_cast<void*>(&Lua_Input_GetBoolRange)},
        {"getAllNumbers",   reinterpret_cast<void*>(&Lua_Input_GetAllNumbers)},
        {"getAllBools",     reinterpret_cast<void*>(&Lua_Input_GetAllBools)},
        {"getPulse",        reinterpret_cast<void*>(&Lua_Input_GetPulse)},
        {"getToggle",       reinterpret_cast<void*>(&Lua_Input_GetToggle)},
        {nullptr,           nullptr}
    };
    RegisterTableWithUpval(self, "input", inputRegs, reinterpret_cast<void*>(self + 0x330));

    // 3. Table `output` enrichie (fonctions d'origine + QoL 1B / 1D)
    LuaReg outputRegs[] = {
        {"setBool",         g_base + swml::EngineRva(g_base, 0x2e98c0)},
        {"setNumber",       g_base + swml::EngineRva(g_base, 0x2e99f0)},
        {"setNumbers",      reinterpret_cast<void*>(&Lua_Output_SetNumbers)},
        {"setBools",        reinterpret_cast<void*>(&Lua_Output_SetBools)},
        {"copyInputs",      reinterpret_cast<void*>(&Lua_Output_CopyInputs)},
        {"clear",           reinterpret_cast<void*>(&Lua_Output_Clear)},
        {nullptr,           nullptr}
    };
    RegisterTableWithUpval(self, "output", outputRegs, reinterpret_cast<void*>(self + 0x330));

    // 4. Table `screen` enrichie (19 fonctions d'origine + QoL 2A / 2B / 2C / 2D)
    LuaReg screenRegs[] = {
        {"drawLine",             g_base + swml::EngineRva(g_base, 0x2e86a0)},
        {"setColor",             g_base + swml::EngineRva(g_base, 0x2e6f00)},
        {"setMapColorOcean",     g_base + swml::EngineRva(g_base, 0x2e70c0)},
        {"setMapColorShallows",  g_base + swml::EngineRva(g_base, 0x2e7280)},
        {"setMapColorLand",      g_base + swml::EngineRva(g_base, 0x2e7440)},
        {"setMapColorGrass",     g_base + swml::EngineRva(g_base, 0x2e7600)},
        {"setMapColorSand",      g_base + swml::EngineRva(g_base, 0x2e77c0)},
        {"setMapColorSnow",      g_base + swml::EngineRva(g_base, 0x2e7980)},
        {"setMapColorRock",      g_base + swml::EngineRva(g_base, 0x2e7b40)},
        {"setMapColorGravel",    g_base + swml::EngineRva(g_base, 0x2e7d00)},
        {"drawCircle",           g_base + swml::EngineRva(g_base, 0x2e88b0)},
        {"drawCircleF",          g_base + swml::EngineRva(g_base, 0x2e8a80)},
        {"drawRect",             g_base + swml::EngineRva(g_base, 0x2e8c50)},
        {"drawRectF",            g_base + swml::EngineRva(g_base, 0x2e9120)},
        {"drawTriangle",         g_base + swml::EngineRva(g_base, 0x2e9320)},
        {"drawTriangleF",        g_base + swml::EngineRva(g_base, 0x2e95e0)},
        {"drawClear",            g_base + swml::EngineRva(g_base, 0x2e7fe0)},
        {"getWidth",             g_base + swml::EngineRva(g_base, 0x2e7ec0)},
        {"getHeight",            g_base + swml::EngineRva(g_base, 0x2e7f50)},
        {"drawText",             g_base + swml::EngineRva(g_base, 0x2e80c0)},
        {"drawTextBox",          g_base + swml::EngineRva(g_base, 0x2e8310)},
        {"drawMap",              g_base + swml::EngineRva(g_base, 0x2e8e50)},
        // Nouvelles fonctions QoL screen.*
        {"drawTextCentered",     reinterpret_cast<void*>(&Lua_Screen_DrawTextCentered)},
        {"drawTextRight",        reinterpret_cast<void*>(&Lua_Screen_DrawTextRight)},
        {"drawArc",              reinterpret_cast<void*>(&Lua_Screen_DrawArc)},
        {"drawGauge",            reinterpret_cast<void*>(&Lua_Screen_DrawGauge)},
        {"drawPoly",             reinterpret_cast<void*>(&Lua_Screen_DrawPoly)},
        {"drawPolyF",            reinterpret_cast<void*>(&Lua_Screen_DrawPolyF)},
        {"drawButton",           reinterpret_cast<void*>(&Lua_Screen_DrawButton)},
        {"setColorHex",          reinterpret_cast<void*>(&Lua_Screen_SetColorHex)},
        {"setColorHSV",          reinterpret_cast<void*>(&Lua_Screen_SetColorHSV)},
        {nullptr,                nullptr}
    };
    RegisterTableWithUpval(self, "screen", screenRegs, reinterpret_cast<void*>(self + 0x60));

    // 5. Table `debug` enrichie (debug.log vers le Terminal ModLoader + debug.notify)
    LuaReg debugRegs[] = {
        {"log",    reinterpret_cast<void*>(&Lua_Debug_Log)},
        {"notify", reinterpret_cast<void*>(&Lua_Debug_Notify)},
        {nullptr,  nullptr}
    };
    RegisterTableWithUpval(self, "debug", debugRegs, reinterpret_cast<void*>(self));

    // 6. Table `qol` dédiée + enrichissement de `math` et raccourcis globaux `_G`
    LuaReg mathAndQolRegs[] = {
        {"clamp",    reinterpret_cast<void*>(&Lua_Math_Clamp)},
        {"lerp",     reinterpret_cast<void*>(&Lua_Math_Lerp)},
        {"invLerp",  reinterpret_cast<void*>(&Lua_Math_InvLerp)},
        {"remap",    reinterpret_cast<void*>(&Lua_Math_Remap)},
        {"round",    reinterpret_cast<void*>(&Lua_Math_Round)},
        {"sign",     reinterpret_cast<void*>(&Lua_Math_Sign)},
        {"wrap",     reinterpret_cast<void*>(&Lua_Math_Wrap)},
        {"deadzone", reinterpret_cast<void*>(&Lua_Math_Deadzone)},
        {"smooth",   reinterpret_cast<void*>(&Lua_Math_Smooth)},
        {"pid",      reinterpret_cast<void*>(&Lua_Qol_Pid)},
        {"resetPid", reinterpret_cast<void*>(&Lua_Qol_ResetPid)},
        {nullptr,    nullptr}
    };
    RegisterTableWithUpval(self, "qol", mathAndQolRegs, reinterpret_cast<void*>(self));

    // Ajoute également ces fonctions dans `math.*` et en raccourcis globaux dans `_G`
    g_rawgeti(L, kRegistryIdx, 2);        // pousse _G sur la pile
    g_luaopenMath(L, 0, 0, 0);            // pousse une table `math` standard complète
    for (int i = 0; mathAndQolRegs[i].name != nullptr; ++i) {
        g_pushcclosure(L, mathAndQolRegs[i].func, 0);
        uintptr_t mathTv = g_index2addr(L, -2);
        g_setfield(L, mathTv, mathAndQolRegs[i].name);
    }
    uintptr_t gTv = g_index2addr(L, -2);
    g_setfield(L, gTv, "math");           // _G.math = math (dépile math)

    // Raccourcis globaux directs (clamp, lerp, remap, round, sign, pid, resetPid)
    for (int i = 0; mathAndQolRegs[i].name != nullptr; ++i) {
        g_pushcclosure(L, mathAndQolRegs[i].func, 0);
        uintptr_t gTv2 = g_index2addr(L, -2);
        g_setfield(L, gTv2, mathAndQolRegs[i].name);
    }
    *(uintptr_t*)(L + 0x10) -= 0x10;      // dépile _G

    // 7. Déblocage du quota CPU / Timeout de la VM Lua (3D)
    *(void**)(L + 0xa8) = reinterpret_cast<void*>(&HkLuaTimeout);
    *(int*)(L + 0xbc) = kUnlockedHookInstrCount;
    *(int*)(L + 0xc0) = kUnlockedHookInstrCount;
    if(vmGeneration)swml::QueueModLuaEvent(SW_EVENT_LUA_CREATED,vmGeneration);
}

void HkMcLuaDestroy(uintptr_t self) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
        for (auto it = g_liveLuaVms.begin(); it != g_liveLuaVms.end();) {
            if (it->second.self == self) {
                swml::QueueModLuaEvent(SW_EVENT_LUA_DESTROYED,it->second.generation);
                it = g_liveLuaVms.erase(it);
            } else ++it;
        }
    }
    g_mcStates.erase(self);
    g_origMcLuaDestroy(self);
}

// ---- Addon/mission script VMs (c_playlist_lua_script) ---------------------------------------
// Same protocol as the microcontroller class: slot 1 closes the state stored at self+8,
// slot 3 registers the game's tables (server, matrix, ...) on a freshly created state.
// The script chunk itself is loaded afterwards, so libraries installed here are visible
// to script.lua from its first line.
void RetireAddonLuaVms(uintptr_t self) {
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    for (auto it = g_liveLuaVms.begin(); it != g_liveLuaVms.end();) {
        if (it->second.addon && it->second.self == self) {
            swml::QueueModLuaEvent(SW_EVENT_ADDON_LUA_DESTROYED, it->second.generation);
            it = g_liveLuaVms.erase(it);
        } else ++it;
    }
}

void HkAddonLuaInit(uintptr_t self) {
    // A reinitialisation invalidates the old state before the game can free/reuse it.
    RetireAddonLuaVms(self);
    g_origAddonLuaInit(self);

    uint64_t vmGeneration = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
        uintptr_t state = *(uintptr_t*)(self + 8);
        if (state) { vmGeneration = ++g_nextLuaGeneration; g_liveLuaVms[state] = {self, vmGeneration, true}; }
    }
    if (!vmGeneration) return;
    ApplyRegisteredLibraries(self, true);
    swml::QueueModLuaEvent(SW_EVENT_ADDON_LUA_CREATED, vmGeneration);
}

void HkAddonLuaDestroy(uintptr_t self) {
    RetireAddonLuaVms(self);
    g_origAddonLuaDestroy(self);
}

// Débloque la limite de caractères dans l'éditeur Lua en jeu (4096/8192 -> 131 072)
void ApplyScriptEditorCharLimit(uintptr_t scriptTab, uint32_t limit) {
    if (!scriptTab) return;
    uintptr_t editBoxAdv = *(uintptr_t*)(scriptTab + 0x88);
    if (!editBoxAdv) return;
    uintptr_t editBoxText = *(uintptr_t*)(editBoxAdv + 0x68);
    if (!editBoxText) return;
    *(uint32_t*)(editBoxText + 0x1f8) = limit;
}

void HkScriptTabInit(uintptr_t self, uintptr_t ctx) {
    g_origScriptTabInit(self, ctx);
    g_lastScriptTab = self;
    ApplyScriptEditorCharLimit(self, g_enabled ? kUnlockedScriptCharLimit : kDefaultScriptCharLimit);
    if (g_host && g_host->log) {
        g_host->log("mc_lua_qol",
                    "Editeur Lua ouvert : limite de caracteres debloquee a 131072 (128 Ko)");
    }
}

void HkScriptTabUpdate(uintptr_t self, uintptr_t a, uintptr_t ctx, uintptr_t b) {
    g_lastScriptTab = self;
    if (g_enabled) ApplyScriptEditorCharLimit(self, kUnlockedScriptCharLimit);
    g_origScriptTabUpdate(self, a, ctx, b);
}

// Ajoute la documentation des fonctions QoL directement dans l'onglet Help de l'éditeur Lua !
void AddHelpCodeBlock(uintptr_t scroll, uintptr_t ctx, float width, const char* code) {
    EStr s;
    MakeEStr(s, code);
    void* mem = g_opNew(0x200);
    void* box = g_editboxCtor(mem, 0, 0, width, 0.0f, 0, 1, 1);
    uintptr_t added = g_scrollAdd(scroll, ctx, box);
    g_editboxSet(added, ctx, &s);
    *(uint32_t*)(added + 8) = 0x3ccccccd;
    FreeEStr(s);
}

void AddHelpSection(uintptr_t tab, uintptr_t ctx, float w, const char* title, const char* desc,
                    const char* code) {
    uintptr_t scroll = *(uintptr_t*)(tab + 0x88);
    g_scrollSpace(scroll, ctx, 0.04f);
    EStr t, d;
    MakeEStr(t, title);
    g_helpHeading(tab, ctx, w, &t);
    FreeEStr(t);

    MakeEStr(d, desc);
    g_helpDesc(tab, ctx, w, &d);
    FreeEStr(d);

    AddHelpCodeBlock(scroll, ctx, w - 0.03f, code);
}

void HkHelpTabInit(uintptr_t self, uintptr_t ctx) {
    g_origHelpTabInit(self, ctx);
    if (!g_enabled) return;
    uintptr_t scroll = *(uintptr_t*)(self + 0x88);
    if (!scroll) return;
    float w = *(float*)(scroll + 0x10) - 0.05f;

    AddHelpSection(
        self, ctx, w,
        "C++ MOD : LUA QOL - INPUT & OUTPUT",
        "Batch read/write composite channels, rising-edge pulse, push-to-toggle & passthrough:",
        "a, b, c = input.getNumbers(1, 2, 3)\n"
        "x, y, z = input.getNumberRange(1, 3)\n"
        "nums = input.getAllNumbers()       -- table [1..32]\n"
        "bools = input.getAllBools()        -- table [1..32]\n"
        "if input.getPulse(1) then ... end  -- rising edge detector\n"
        "isOn = input.getToggle(2, false)   -- built-in push toggle\n\n"
        "output.setNumbers(1, x, y, z, rpm) -- writes channels 1..4\n"
        "output.setBools(1, true, false)\n"
        "output.copyInputs()                -- full 32-ch passthrough\n"
        "output.clear()");

    AddHelpSection(
        self, ctx, w,
        "C++ MOD : LUA QOL - SCREEN & TOUCH UI",
        "Centered/right text, arcs, gauges, polygons, one-line touchscreen button, Hex & HSV colors:",
        "screen.setColorHex(0xFF8800, 255)  -- or \"#FF8800\"\n"
        "screen.setColorHSV(120, 1.0, 1.0)  -- Hue 0..360, S 0..1, V 0..1\n"
        "screen.drawTextCentered(w/2, h/2, \"CENTERED\")\n"
        "screen.drawTextRight(w - 2, 2, \"100%\")\n"
        "screen.drawArc(32, 32, 18, 0, 270, 20)\n"
        "screen.drawGauge(32, 32, 20, rpm / maxRpm, 135, 405)\n"
        "screen.drawPolyF(10,10, 20,15, 15,25, 5,20)\n"
        "if screen.drawButton(4, 4, 28, 10, \"START\", tx, ty, touch) then\n"
        "\t-- button clicked!\n"
        "end");

    AddHelpSection(
        self, ctx, w,
        "C++ MOD : LUA QOL - MATH, PID & DEBUG",
        "Extended math functions (in math.*, qol.*, and global), 1-line C++ PID & ModLoader alerts:",
        "v = math.clamp(x, 0, 100)\n"
        "v = math.lerp(a, b, 0.25)\n"
        "v = math.remap(rpm, 0, 60, 0, 1, true)\n"
        "v = math.round(val, 2)\n"
        "v = math.wrap(heading, -180, 180)\n"
        "v = math.deadzone(stickX, 0.08)\n"
        "v = math.smooth(\"alt\", rawAlt, 0.1)\n\n"
        "-- Built-in stateful C++ PID with anti-windup:\n"
        "throttle = pid(\"eng\", targetRpm, curRpm, 0.08, 0.002, 0.01, 0, 1)\n\n"
        "-- Print to ModLoader Debug Terminal & Top-Left UI Notification:\n"
        "debug.log(\"RPM =\", curRpm, \"Throttle =\", throttle)\n"
        "debug.notify(\"warn\", \"Engine Alert\", \"Temperature high!\")");

    g_scrollFin(scroll, ctx);
}

bool PatchVtableSlot(void** vt, size_t slot, void* hook, void** origOut) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (origOut) *origOut = vt[slot];
    vt[slot] = hook;
    DWORD dummy = 0;
    VirtualProtect(&vt[slot], sizeof(void*), old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));
    return true;
}

}  // namespace

bool swml::EnableNativeLua(const SwModHost* host) {
    if (!host || !host->gameBase) return false;
    g_host = host;
    if (g_installed) return true;
    g_base = reinterpret_cast<uint8_t*>(host->gameBase);
    if (!ValidateLuaProfile(g_base)) return false;

    // Reference keys are translated only through the reviewed build profile.
    // Core service only: primitives, table extras and microcontroller VM lifecycle
    // slots. Optional QoL/editor targets are bound in EnableNativeQolEditor so
    // runtime Lua startup never depends on them.
    g_opNew        = reinterpret_cast<fn_op_new_t>(g_base + EngineRva(g_base, 0x957478));
    g_strReserve   = reinterpret_cast<fn_str_reserve_t>(g_base + EngineRva(g_base, 0x17cb00));
    g_strFree      = reinterpret_cast<fn_str_free_t>(g_base + EngineRva(g_base, 0x9b15a0));
    g_regLib       = reinterpret_cast<fn_reg_lib_t>(g_base + EngineRva(g_base, 0x1e3ee0));
    g_index2addr   = reinterpret_cast<fn_index2addr_t>(g_base + EngineRva(g_base, 0x097920));
    g_tonumber     = reinterpret_cast<fn_tonumber_t>(g_base + EngineRva(g_base, 0x0be4a0));
    g_checkstack   = reinterpret_cast<fn_checkstack_t>(g_base + EngineRva(g_base, 0x097a10));
    g_newtable     = reinterpret_cast<fn_newtable_t>(g_base + EngineRva(g_base, 0x0ba830));
    g_resizearray  = reinterpret_cast<fn_resizearray_t>(g_base + EngineRva(g_base, 0x0ba2e0));
    g_rawgeti      = reinterpret_cast<fn_rawgeti_t>(g_base + EngineRva(g_base, 0x0982c0));
    g_pushcclosure = reinterpret_cast<fn_pushcclosure_t>(g_base + EngineRva(g_base, 0x098050));
    g_setfield     = reinterpret_cast<fn_setfield_t>(g_base + EngineRva(g_base, 0x0983d0));

    // Optional P0 extension: no address fallback when the central profile lacks a key.
    // Parent merges tools/p0_lua_binding_evidence.json into its reviewed binding table.
    if (ValidateEngineBinding(g_base, 0x097e90) &&
        ValidateEngineBinding(g_base, 0x098130) && ValidateEngineBinding(g_base, 0x0a26f0)) {
        g_pushlstring = reinterpret_cast<fn_pushlstring_t>(g_base + EngineRva(g_base, 0x097e90));
        g_getfield = reinterpret_cast<fn_getfield_t>(g_base + EngineRva(g_base, 0x098130));
        g_rawrunprotected = reinterpret_cast<fn_rawrunprotected_t>(g_base + EngineRva(g_base, 0x0a26f0));
    }

    g_mcLuaVt      = reinterpret_cast<void**>(g_base + EngineRva(g_base, 0xb6dac0));

    // Vérification de sécurité des slots de vtable avant de poser les hooks
    if (g_mcLuaVt[1] != g_base + EngineRva(g_base, 0x2e5ad0) ||
        g_mcLuaVt[3] != g_base + EngineRva(g_base, 0x2e6050)) {
        if (host->log) {
            host->log("mc_lua_qol", "ERROR: Vtable slots mismatch — hooks aborted");
        }
        return false;
    }

    const swml::SlotPatch patches[] = {
        {&g_mcLuaVt[1], g_base+EngineRva(g_base,0x2e5ad0), reinterpret_cast<void*>(&HkMcLuaDestroy), reinterpret_cast<void**>(&g_origMcLuaDestroy)},
        {&g_mcLuaVt[3], g_base+EngineRva(g_base,0x2e6050), reinterpret_cast<void*>(&HkMcLuaInit), reinterpret_cast<void**>(&g_origMcLuaInit)}
    };
    if (!swml::PatchSlots(patches, _countof(patches))) {
        DisableNativeLua();
        if (host->log) host->log("mc_lua_qol", "ERROR: VirtualProtect failed on Lua vtables");
        return false;
    }

    if (host->log) {
        host->log("mc_lua_qol",
                  "Service Lua central installe : bibliotheques personnalisees et evenements VM (QoL/editeur optionnels separes)");
    }
    g_installed = true;

    // Optional protected call (extras->call): no fallback when the body differs.
    g_pcallk = reinterpret_cast<fn_pcallk_t>(swml::ResolveLuaPcallk(g_base));

    // Optional addon/mission script VMs: certified for the newer reviewed build only.
    // A rejected profile or a failed transaction leaves the core service untouched.
    swml::AddonLuaProfile addon{};
    if (swml::ResolveAddonLuaProfile(g_base, &addon)) {
        g_addonLuaVt = addon.vtable;
        const swml::SlotPatch addonPatches[] = {
            {&g_addonLuaVt[1], addon.close, reinterpret_cast<void*>(&HkAddonLuaDestroy), reinterpret_cast<void**>(&g_origAddonLuaDestroy)},
            {&g_addonLuaVt[3], addon.init, reinterpret_cast<void*>(&HkAddonLuaInit), reinterpret_cast<void**>(&g_origAddonLuaInit)}
        };
        g_addonInstalled = swml::PatchSlots(addonPatches, _countof(addonPatches));
        if (host->log) host->log("lua", g_addonInstalled
            ? "Addon Lua: script VM lifecycle hooks installed (libraries apply to newly created addon VMs)"
            : "ERROR: Addon Lua: vtable transaction failed; addon libraries unavailable");
    } else if (host->log) {
        host->log("lua", "Addon Lua: unavailable for this build (newer reviewed build only); microcontroller Lua unaffected");
    }
    return true;
}

bool swml::NativeAddonLuaInstalled() { return g_addonInstalled; }

// ---- Simulation tick ------------------------------------------------------------------------
// The server update (profile kSimUpdate) runs every addon's onTick once per simulation
// update. Its three direct call sites are redirected to a near thunk that jumps here; the
// hook calls the original with the same twelve arguments, then delivers the tick, so mods
// observe the world after every addon has ticked. Installed on the first subscription and
// kept for the process lifetime (the thunk only ever points into the loader).
namespace {
using SimUpdateFn = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
SimUpdateFn g_origSimUpdate = nullptr;
bool g_tickInstalled = false;
uint64_t g_tickSequence = 0;
uintptr_t g_tickState = 0;      // addon state handed to the previous tick, kept while valid
SwHandle g_nextTickHandle = 0x5400000000000001ull;
DWORD g_tickThread = 0;
ULONGLONG g_tickRateStart = 0;  // one-shot rate diagnostic in the loader log
constexpr size_t kTickPerOwnerLimit = 64;

bool AddonScriptReady(uintptr_t self, uintptr_t state) {
    __try { return *(uintptr_t*)(self + 8) == state && *(uint32_t*)(self + 0x78) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool InvokeTickCpp(SwSimulationTickCallback fn, const SwSimulationTick* tick, void* data) {
    try { fn(tick, data); return true; } catch (...) { return false; }
}
bool SafeTickCallback(SwSimulationTickCallback fn, const SwSimulationTick* tick, void* data) {
    __try { return InvokeTickCpp(fn, tick, data); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RunSimulationTick() {
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    const uint64_t sequence = ++g_tickSequence;
    std::vector<LibraryFunction*> subscribers;
    for (const auto& e : g_libraryFunctions) if (e->owner && e->tick) subscribers.push_back(e.get());
    if (subscribers.empty()) return;
    if (!g_tickThread) {
        g_tickThread = GetCurrentThreadId();
        g_tickRateStart = GetTickCount64();
        if (g_host && g_host->log) {
            char line[128];
            snprintf(line, sizeof(line), "Simulation tick: first tick delivered (thread %lu)", g_tickThread);
            g_host->log("lua", line);
        }
    } else if (g_tickRateStart && sequence % 600 == 0) {
        const ULONGLONG elapsed = GetTickCount64() - g_tickRateStart;
        if (g_host && g_host->log && elapsed) {
            char line[128];
            snprintf(line, sizeof(line), "Simulation tick: 600 updates in %llu ms", elapsed);
            g_host->log("lua", line);
        }
        g_tickRateStart = 0; // measured once
    }
    // Keep the same addon VM while it stays valid; otherwise take any ready one.
    uintptr_t state = 0;
    LiveLuaVm vm{};
    auto kept = g_liveLuaVms.find(g_tickState);
    if (kept != g_liveLuaVms.end() && kept->second.addon && AddonScriptReady(kept->second.self, kept->first)) {
        state = kept->first; vm = kept->second;
    } else {
        for (const auto& item : g_liveLuaVms)
            if (item.second.addon && AddonScriptReady(item.second.self, item.first)) { state = item.first; vm = item.second; break; }
    }
    g_tickState = state;
    for (LibraryFunction* entry : subscribers) {
        if (!entry->owner || !entry->tick) continue; // unsubscribed by an earlier callback
        const SwSimulationTick info{sizeof(SwSimulationTick), sequence, reinterpret_cast<void*>(state)};
        LuaCallbackFrame frame{entry, state, vm.self, vm.generation};
        LuaCallbackFrame* saved = g_luaCallbackFrame;
        // The state is idle between updates; whatever the callback leaves is discarded.
        const uintptr_t savedOffset = state ? *(uintptr_t*)(state + 0x10) - *(uintptr_t*)(state + 0x38) : 0;
        if (state) g_luaCallbackFrame = &frame;
        const bool ok = SafeTickCallback(entry->tick, &info, entry->userData);
        g_luaCallbackFrame = saved;
        if (state) *(uintptr_t*)(state + 0x10) = *(uintptr_t*)(state + 0x38) + savedOffset;
        if (!ok) {
            entry->tick = nullptr;
            if (g_host && g_host->log) g_host->log("lua", "ERROR: plugin simulation tick callback crashed; disabled");
        }
    }
}

void HkSimulationUpdate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                        uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12) {
    g_origSimUpdate(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    RunSimulationTick();
}

uint8_t* AllocateNearThunk(const void* target) {
    const uintptr_t granularity = 0x10000ull;
    const uintptr_t base = reinterpret_cast<uintptr_t>(target) & ~(granularity - 1);
    for (int step = 1; step <= 64; ++step) {
        for (int sign : {-1, 1}) {
            const uintptr_t candidate = base + static_cast<uintptr_t>(sign) * step * 0x400000ull;
            if (candidate < granularity) continue;
            if (void* block = VirtualAlloc(reinterpret_cast<void*>(candidate), 0x1000,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(block);
        }
    }
    return nullptr;
}
bool WriteCallTarget(uint8_t* site, int32_t rel) {
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site + 1, &rel, sizeof(rel));
    VirtualProtect(site + 1, 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// All checks happen before the first write; a failed write restores the sites already
// redirected, so the image is either fully redirected or untouched.
bool InstallSimulationTick() {
    swml::SimulationTickProfile profile{};
    if (!swml::ResolveSimulationTickProfile(g_base, &profile)) return false;
    uint8_t* thunk = AllocateNearThunk(profile.sites[0]);
    if (!thunk) return false;
    thunk[0] = 0xff; thunk[1] = 0x25;                     // jmp qword ptr [rip+0]
    *reinterpret_cast<int32_t*>(thunk + 2) = 0;
    *reinterpret_cast<uint64_t*>(thunk + 6) = reinterpret_cast<uint64_t>(&HkSimulationUpdate);
    DWORD old = 0;
    VirtualProtect(thunk, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 14);
    int32_t original[3] = {}, redirected[3] = {};
    for (size_t i = 0; i < 3; ++i) {
        const int64_t delta = reinterpret_cast<intptr_t>(thunk) - reinterpret_cast<intptr_t>(profile.sites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) { VirtualFree(thunk, 0, MEM_RELEASE); return false; }
        redirected[i] = static_cast<int32_t>(delta);
        memcpy(&original[i], profile.sites[i] + 1, sizeof(int32_t));
    }
    g_origSimUpdate = reinterpret_cast<SimUpdateFn>(profile.update); // published before any redirect
    for (size_t i = 0; i < 3; ++i) {
        if (!WriteCallTarget(profile.sites[i], redirected[i])) {
            while (i--) WriteCallTarget(profile.sites[i], original[i]);
            return false;
        }
    }
    return true;
}
}

SwResult swml::SubscribeSimulationTick(const SwModHost* owner, SwSimulationTickCallback callback,
                                       void* userData, SwHandle* out) {
    if (!owner) return SW_INVALID_OWNER;
    if (!callback || !out) return SW_INVALID_ARGUMENT;
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    if (!g_installed) return SW_UNAVAILABLE;
    if (!g_tickInstalled) {
        g_tickInstalled = InstallSimulationTick();
        if (g_host && g_host->log) g_host->log("lua", g_tickInstalled
            ? "Simulation tick: server update call sites redirected (ticks start when a world runs)"
            : "ERROR: Simulation tick: profile rejected or redirection failed; ticks unavailable");
        if (!g_tickInstalled) return SW_UNAVAILABLE;
    }
    size_t count = 0;
    for (const auto& e : g_libraryFunctions) if (e->owner == owner && e->tick) ++count;
    if (count >= kTickPerOwnerLimit) return SW_LIMIT_REACHED;
    try {
        auto entry = std::make_unique<LibraryFunction>();
        entry->owner = owner;
        entry->tick = callback;
        entry->userData = userData;
        entry->handle = g_nextTickHandle++;
        *out = entry->handle;
        g_libraryFunctions.push_back(std::move(entry));
    } catch (...) { return SW_LIMIT_REACHED; }
    return SW_OK;
}

SwResult swml::UnsubscribeSimulationTick(const SwModHost* owner, SwHandle handle) {
    if (!owner) return SW_INVALID_OWNER;
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    for (auto& e : g_libraryFunctions) {
        if (e->handle != handle || !e->tick) continue;
        if (e->owner != owner) return SW_NOT_FOUND;
        e->owner = nullptr; e->tick = nullptr; e->userData = nullptr;
        return SW_OK;
    }
    return SW_NOT_FOUND;
}

// Optional QoL/editor hooks are a separate activation: validated, bound and
// installed only when a mod asks for them, never as a side effect of core Lua
// startup. PatchSlots is transactional, so a failed pass leaves no editor hook.
bool swml::EnableNativeQolEditor(const SwModHost* host) {
    if (!host || !host->gameBase) return false;
    if (g_qolEditorInstalled) return true;
    if (!g_installed) return false;
    g_host = host;
    if (!ValidateQolEditorProfile(g_base)) return false;

    g_luaopenMath  = reinterpret_cast<fn_luaopen_math_t>(g_base + EngineRva(g_base, 0x0abd60));
    g_getTimeMs    = reinterpret_cast<fn_get_time_ms_t>(g_base + EngineRva(g_base, 0x174f60));
    g_origTimeoutHook = reinterpret_cast<fn_lua_hook_t>(g_base + EngineRva(g_base, 0x2e6b00));

    g_drawLine     = reinterpret_cast<fn_draw_line_t>(g_base + EngineRva(g_base, 0x2e3bc0));
    g_drawRect     = reinterpret_cast<fn_draw_rect_t>(g_base + EngineRva(g_base, 0x2e3eb0));
    g_drawRectF    = reinterpret_cast<fn_draw_rectf_t>(g_base + EngineRva(g_base, 0x2e4460));
    g_drawTriF     = reinterpret_cast<fn_draw_trif_t>(g_base + EngineRva(g_base, 0x2e4140));
    g_drawText     = reinterpret_cast<fn_draw_text_t>(g_base + EngineRva(g_base, 0x2e4620));

    g_helpHeading  = reinterpret_cast<fn_help_heading_t>(g_base + EngineRva(g_base, 0x63f300));
    g_helpDesc     = reinterpret_cast<fn_help_desc_t>(g_base + EngineRva(g_base, 0x63f420));
    g_scrollSpace  = reinterpret_cast<fn_scroll_space_t>(g_base + EngineRva(g_base, 0x24b140));
    g_editboxCtor  = reinterpret_cast<fn_editbox_ctor_t>(g_base + EngineRva(g_base, 0x25e730));
    g_scrollAdd    = reinterpret_cast<fn_scroll_add_t>(g_base + EngineRva(g_base, 0x9253e0));
    g_editboxSet   = reinterpret_cast<fn_editbox_set_t>(g_base + EngineRva(g_base, 0x25ed10));
    g_scrollFin    = reinterpret_cast<fn_scroll_fin_t>(g_base + EngineRva(g_base, 0x24b650));

    g_scriptTabVt  = reinterpret_cast<void**>(g_base + EngineRva(g_base, 0xb67728));
    g_helpTabVt    = reinterpret_cast<void**>(g_base + EngineRva(g_base, 0xae9a20));

    if (g_scriptTabVt[1] != g_base + EngineRva(g_base, 0x63f550) ||
        g_scriptTabVt[3] != g_base + EngineRva(g_base, 0x63fbc0) ||
        g_helpTabVt[1] != g_base + EngineRva(g_base, 0x63d980)) {
        if (host->log) {
            host->log("mc_lua_qol", "ERROR: Vtable slots mismatch (editor) — hooks aborted");
        }
        return false;
    }

    const swml::SlotPatch patches[] = {
        {&g_scriptTabVt[1], g_base+EngineRva(g_base,0x63f550), reinterpret_cast<void*>(&HkScriptTabInit), reinterpret_cast<void**>(&g_origScriptTabInit)},
        {&g_scriptTabVt[3], g_base+EngineRva(g_base,0x63fbc0), reinterpret_cast<void*>(&HkScriptTabUpdate), reinterpret_cast<void**>(&g_origScriptTabUpdate)},
        {&g_helpTabVt[1], g_base+EngineRva(g_base,0x63d980), reinterpret_cast<void*>(&HkHelpTabInit), reinterpret_cast<void**>(&g_origHelpTabInit)}
    };
    if (!swml::PatchSlots(patches, _countof(patches))) {
        if (host->log) host->log("mc_lua_qol", "ERROR: VirtualProtect failed on editor vtables");
        return false;
    }

    g_qolEditorInstalled = true;
    if (host->log) {
        host->log("mc_lua_qol",
                  "Hooks Lua QoL installes : 25+ fonctions Lua + limite 131072 chars + CPU x10");
    }
    return true;
}

void swml::SetNativeLuaEnabled(bool enabled) { g_enabled = enabled; }

namespace {
bool HasLuaContext(void* state, const SwModHost* owner = nullptr) {
    const auto* frame = g_luaCallbackFrame;
    if (!g_installed || !state || !frame || frame->state != reinterpret_cast<uintptr_t>(state) ||
        !frame->entry->owner || (!frame->entry->callback && !frame->entry->tick) ||
        (owner && frame->entry->owner != owner)) return false;
    auto it = g_liveLuaVms.find(frame->state);
    return it != g_liveLuaVms.end() && it->second.self == frame->self &&
           it->second.generation == frame->generation;
}

int InvokePluginCpp(SwLuaCallback fn, void* L, void* data) {
    try { return fn(L, data); } catch (...) { return -1; }
}
bool SafeLuaCallback(SwLuaCallback fn, void* L, void* data, int* result) {
    __try {
        *result = InvokePluginCpp(fn, L, data);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *result = 0;
        return false;
    }
}

int DispatchLibraryFunction(uintptr_t L) {
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    auto* entry = reinterpret_cast<LibraryFunction*>(LuaGetUpvaluePtr(L));
    bool known = false;
    for (const auto& token : g_libraryFunctions) if (token.get() == entry) { known = true; break; }
    auto vm = g_liveLuaVms.find(L);
    if (!known || !entry->owner || !entry->callback || vm == g_liveLuaVms.end()) return 0;
    LuaCallbackFrame frame{entry, L, vm->second.self, vm->second.generation};
    LuaCallbackFrame* saved = g_luaCallbackFrame;
    const int initialTop = LuaGetTop(L);
    g_luaCallbackFrame = &frame;
    int result = 0;
    const bool succeeded = SafeLuaCallback(entry->callback, reinterpret_cast<void*>(L), entry->userData, &result);
    g_luaCallbackFrame = saved;
    if (!succeeded || result < 0 || result > LuaGetTop(L) || !entry->owner) {
        uintptr_t func = **(uintptr_t**)(L + 0x20);
        *(uintptr_t*)(L + 0x10) = func + 16 + static_cast<uintptr_t>(initialTop) * 16;
        result = 0;
        entry->callback = nullptr;
        if (g_host && g_host->log) g_host->log("lua", "ERROR: Plugin Lua callback crashed; disabled");
    }
    return result;
}

bool IsLibraryName(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_')) return false;
        if (p - s >= 63) return false;
    }
    return true;
}

// Addon libraries become globals of script.lua: refuse every game table, standard
// library, base function and callback name so a mod cannot shadow the game's API.
bool IsReservedAddonName(const char* name) {
    static const char* const reserved[] = {
        "server", "matrix", "property", "debug", "async", "g_savedata", "_G", "_ENV", "_VERSION",
        "math", "string", "table", "coroutine", "utf8", "os", "io", "package",
        "pairs", "ipairs", "next", "tostring", "tonumber", "type", "print", "select", "error",
        "assert", "pcall", "xpcall", "load", "loadfile", "dofile", "require", "collectgarbage",
        "getmetatable", "setmetatable", "rawget", "rawset", "rawequal", "rawlen", "httpReply"};
    for (const char* r : reserved) if (strcmp(name, r) == 0) return true;
    // onTick, onCreate, onPlayerJoin, ...: the game looks these globals up as callbacks.
    return name[0] == 'o' && name[1] == 'n' && name[2] >= 'A' && name[2] <= 'Z';
}

bool RegisterLibraryScoped(const SwModHost* owner, const char* name,
                           const SwLuaFunction* functions, size_t count, bool addon) {
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    if (!g_installed || !g_rawrunprotected || !owner || !IsLibraryName(name) || !functions || !count || count > 256) return false;
    if (addon) {
        if (!g_addonInstalled || IsReservedAddonName(name)) return false;
    } else {
        const char* reserved[] = {"input", "output", "screen", "math", "debug", "qol", "_G", "string", "table"};
        for (const char* r : reserved) if (strcmp(name, r) == 0) return false;
    }
    for (const auto& e : g_libraryFunctions) {
        if (e->owner && e->addon == addon && e->library == name) return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!IsLibraryName(functions[i].name) || !functions[i].callback) return false;
        for (size_t j = 0; j < i; ++j) if (strcmp(functions[i].name, functions[j].name) == 0) return false;
    }
    try {
        std::vector<std::unique_ptr<LibraryFunction>> staged;
        staged.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto e = std::make_unique<LibraryFunction>();
            e->owner = owner;
            e->library = name;
            e->name = functions[i].name;
            e->callback = functions[i].callback;
            e->userData = functions[i].userData;
            e->addon = addon;
            staged.push_back(std::move(e));
        }
        g_libraryFunctions.reserve(g_libraryFunctions.size() + count);
        for (auto& e : staged) g_libraryFunctions.push_back(std::move(e));
    } catch (...) { return false; }
    return true;
}
bool RegisterLibrary(const SwModHost* owner, const char* name,
                     const SwLuaFunction* functions, size_t count) {
    return RegisterLibraryScoped(owner, name, functions, count, false);
}
bool RegisterAddonLibrary(const SwModHost* owner, const char* name,
                          const SwLuaFunction* functions, size_t count) {
    return RegisterLibraryScoped(owner, name, functions, count, true);
}

struct LibraryInstallData { const char* name; const std::vector<LibraryFunction*>* entries; };
void InstallLibraryLeaf(uintptr_t L, void* opaque) {
    auto* data = static_cast<LibraryInstallData*>(opaque);
    g_rawgeti(L, kRegistryIdx, 2);
    uintptr_t table = g_newtable(L);
    uintptr_t top = *(uintptr_t*)(L + 0x10);
    *(uintptr_t*)top = table;
    *(uint32_t*)(top + 8) = 0x45;
    *(uintptr_t*)(L + 0x10) = top + 16;
    for (size_t i = 0; i < data->entries->size(); ++i) {
        LibraryFunction* entry = (*data->entries)[i];
        top = *(uintptr_t*)(L + 0x10);
        *(uintptr_t*)top = reinterpret_cast<uintptr_t>(entry);
        *(uint32_t*)(top + 8) = 2;
        *(uintptr_t*)(L + 0x10) = top + 16;
        g_pushcclosure(L, reinterpret_cast<void*>(&DispatchLibraryFunction), 1);
        g_setfield(L, g_index2addr(L, -2), entry->name.c_str());
    }
    g_setfield(L, g_index2addr(L, -2), data->name);
    *(uintptr_t*)(L + 0x10) -= 16;
}
void ApplyRegisteredLibraries(uintptr_t self, bool addon) {
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    uintptr_t L = *(uintptr_t*)(self + 8);
    if (!L || !g_rawrunprotected || !g_checkstack(L, 8)) return;
    std::map<std::string, std::vector<LibraryFunction*>> libraries;
    for (const auto& e : g_libraryFunctions)
        if (e->owner && e->callback && e->addon == addon) libraries[e->library].push_back(e.get());
    for (const auto& lib : libraries) {
        const uintptr_t savedOffset = *(uintptr_t*)(L + 0x10) - *(uintptr_t*)(L + 0x38);
        LibraryInstallData data{lib.first.c_str(), &lib.second};
        if (g_rawrunprotected(L, &InstallLibraryLeaf, &data) != 0) {
            *(uintptr_t*)(L + 0x10) = *(uintptr_t*)(L + 0x38) + savedOffset;
            if (g_host && g_host->log) g_host->log("lua", "ERROR: Library installation allocation failed");
            break;
        }
    }
}

int ApiGetTop(void* L) { return HasLuaContext(L) ? LuaGetTop(reinterpret_cast<uintptr_t>(L)) : 0; }
double ApiArgNumber(void* L, int i, double d) { return HasLuaContext(L) && i > 0 ? LuaArgNumber(reinterpret_cast<uintptr_t>(L), i, d) : d; }
bool ApiArgBool(void* L, int i, bool d) { return HasLuaContext(L) && i > 0 ? LuaArgBool(reinterpret_cast<uintptr_t>(L), i, d) : d; }
size_t ApiArgString(void* L, int i, char* out, size_t cap) {
    std::string value = HasLuaContext(L) && i > 0 ? LuaArgString(reinterpret_cast<uintptr_t>(L), i) : "";
    if (out && cap) {
        size_t n = (std::min)(value.size(), cap - 1);
        memcpy(out, value.data(), n);
        out[n] = 0;
    }
    return value.size();
}
bool ApiCheckStack(void* L, int n) { return HasLuaContext(L) && n >= 0 && n <= SW_LUA_MAX_STACK_SLOTS && g_checkstack(reinterpret_cast<uintptr_t>(L), n); }
void ApiPushNumber(void* L, double v) { if (ApiCheckStack(L, 1)) LuaPushNumber(reinterpret_cast<uintptr_t>(L), v); }
void ApiPushBool(void* L, bool v) { if (ApiCheckStack(L, 1)) LuaPushBool(reinterpret_cast<uintptr_t>(L), v); }

SwLuaStatus ExtrasContext(const SwModHost* owner, void* L) {
    if (!owner || !L) return SW_LUA_INVALID_ARGUMENT;
    return HasLuaContext(L, owner) ? SW_LUA_OK : SW_LUA_INVALID_CONTEXT;
}
SwLuaStatus StackIndex(void* state, int index, int* absolute) {
    const int top = LuaGetTop(reinterpret_cast<uintptr_t>(state));
    if (!index || index <= kRegistryIdx) return SW_LUA_OUT_OF_RANGE;
    if (index > top || index < -top) return SW_LUA_OUT_OF_RANGE;
    *absolute = index > 0 ? index : top + index + 1;
    return SW_LUA_OK;
}

// Only this POD-only leaf is inside the engine's setjmp scope. No plugin frames,
// C++ locks, strings, or destructors can be skipped by an allocation-error longjmp.
enum class LuaOperation { PushString, CreateTable, GetField, SetField, GetGlobal };
struct LuaOperationData {
    LuaOperation operation;
    int index;
    const char* bytes;
    size_t length;
    uint32_t arraySlots;
};
void LuaOperationLeaf(uintptr_t L, void* opaque) {
    auto* data = static_cast<LuaOperationData*>(opaque);
    switch (data->operation) {
    case LuaOperation::PushString:
        g_pushlstring(L, data->bytes, data->length);
        break;
    case LuaOperation::CreateTable: {
        uintptr_t table = g_newtable(L);
        uintptr_t top = *(uintptr_t*)(L + 0x10);
        *(uintptr_t*)top = table;
        *(uint32_t*)(top + 8) = 0x45;
        *(uintptr_t*)(L + 0x10) = top + 16; // root table before resizing
        if (data->arraySlots) g_resizearray(L, table, data->arraySlots);
        break;
    }
    case LuaOperation::GetField:
        g_getfield(L, g_index2addr(L, data->index), data->bytes);
        break;
    case LuaOperation::SetField:
        g_setfield(L, g_index2addr(L, data->index), data->bytes);
        break;
    case LuaOperation::GetGlobal: {
        g_rawgeti(L, kRegistryIdx, 2); // push _G
        uintptr_t globals = *(uintptr_t*)(L + 0x10) - 16;
        // A metatable on _G could run arbitrary script code: report it, push nil.
        if (*(uint32_t*)(globals + 8) != 0x45 || *(uintptr_t*)(*(uintptr_t*)globals + 0x28)) {
            *(uint64_t*)globals = 0; *(uint32_t*)(globals + 8) = 0;
            data->index = -1;
            break;
        }
        g_getfield(L, globals, data->bytes);
        // Replace _G with the value just pushed above it. The stack cannot move here.
        uintptr_t top = *(uintptr_t*)(L + 0x10);
        memcpy(reinterpret_cast<void*>(top - 32), reinterpret_cast<void*>(top - 16), 16);
        *(uintptr_t*)(L + 0x10) = top - 16;
        break;
    }
    }
}
SwLuaStatus RunLuaOperation(void* state, LuaOperationData* data, int slots) {
    if (!g_rawrunprotected || !g_pushlstring || !g_getfield) return SW_LUA_UNSUPPORTED;
    uintptr_t L = reinterpret_cast<uintptr_t>(state);
    if (!g_checkstack(L, slots)) return SW_LUA_STACK_LIMIT;
    // Lua stack can move during allocation; retain an offset, never an old top pointer.
    const uintptr_t savedOffset = *(uintptr_t*)(L + 0x10) - *(uintptr_t*)(L + 0x38);
    const int status = g_rawrunprotected(L, &LuaOperationLeaf, data);
    if (status != 0) {
        *(uintptr_t*)(L + 0x10) = *(uintptr_t*)(L + 0x38) + savedOffset;
        return SW_LUA_ENGINE_ERROR;
    }
    return SW_LUA_OK;
}
SwLuaStatus ExtrasPushString(const SwModHost* owner, void* L, const char* bytes, size_t length) {
    auto status = ExtrasContext(owner, L);
    if (status != SW_LUA_OK) return status;
    if ((!bytes && length) || length > SW_LUA_MAX_STRING_BYTES) return SW_LUA_INVALID_ARGUMENT;
    try {
        const std::string copy(bytes ? bytes : "", length);
        LuaOperationData data{LuaOperation::PushString, 0, copy.data(), length, 0};
        return RunLuaOperation(L, &data, 1);
    } catch (...) { return SW_LUA_ENGINE_ERROR; }
}
SwLuaStatus ExtrasPushNil(const SwModHost* owner, void* state) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (!ApiCheckStack(state, 1)) return SW_LUA_STACK_LIMIT;
    uintptr_t L = reinterpret_cast<uintptr_t>(state), top = *(uintptr_t*)(L + 0x10);
    *(uint64_t*)top = 0;
    *(uint32_t*)(top + 8) = 0;
    *(uintptr_t*)(L + 0x10) = top + 16;
    return SW_LUA_OK;
}
SwLuaStatus ExtrasPushInteger(const SwModHost* owner, void* state, int64_t value) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (!ApiCheckStack(state, 1)) return SW_LUA_STACK_LIMIT;
    uintptr_t L = reinterpret_cast<uintptr_t>(state), top = *(uintptr_t*)(L + 0x10);
    *(int64_t*)top = value;
    *(uint32_t*)(top + 8) = 0x13;
    *(uintptr_t*)(L + 0x10) = top + 16;
    return SW_LUA_OK;
}
SwLuaStatus ExtrasCreateTable(const SwModHost* owner, void* state, uint32_t arraySlots) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (arraySlots > SW_LUA_MAX_ARRAY_SLOTS) return SW_LUA_INVALID_ARGUMENT;
    LuaOperationData data{LuaOperation::CreateTable, 0, nullptr, 0, arraySlots};
    return RunLuaOperation(state, &data, 1);
}
SwLuaStatus ExtrasField(const SwModHost* owner, void* state, int index, const char* key, bool set) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (!key) return SW_LUA_INVALID_ARGUMENT;
    char copy[256];
    size_t n = 0;
    for (; n < sizeof(copy); ++n) { copy[n] = key[n]; if (!copy[n]) break; }
    if (!n || n == sizeof(copy)) return SW_LUA_INVALID_ARGUMENT;
    int absolute = 0;
    status = StackIndex(state, index, &absolute);
    if (status != SW_LUA_OK) return status;
    uintptr_t tv = g_index2addr(reinterpret_cast<uintptr_t>(state), absolute);
    if (*(uint32_t*)(tv + 8) != 0x45) return SW_LUA_TYPE_MISMATCH;
    // Avoid calling user metamethods (which can reenter a plugin or yield/throw).
    if (*(uintptr_t*)(*(uintptr_t*)tv + 0x28)) return SW_LUA_UNSUPPORTED;
    LuaOperationData data{set ? LuaOperation::SetField : LuaOperation::GetField,
                          absolute, copy, n, 0};
    return RunLuaOperation(state, &data, 2);
}
SwLuaStatus ExtrasGetField(const SwModHost* o, void* L, int i, const char* k) { return ExtrasField(o, L, i, k, false); }
SwLuaStatus ExtrasSetField(const SwModHost* o, void* L, int i, const char* k) { return ExtrasField(o, L, i, k, true); }
SwLuaStatus ExtrasGetType(const SwModHost* owner, void* state, int index, SwLuaType* type) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (!type) return SW_LUA_INVALID_ARGUMENT;
    *type = SW_LUA_TYPE_NONE;
    int absolute = 0;
    status = StackIndex(state, index, &absolute);
    if (status != SW_LUA_OK) return status;
    uint32_t tag = *(uint32_t*)(g_index2addr(reinterpret_cast<uintptr_t>(state), absolute) + 8);
    *type = tag == 0x13 ? SW_LUA_TYPE_INTEGER : static_cast<SwLuaType>(tag & 0xf);
    return SW_LUA_OK;
}
SwLuaStatus ExtrasPop(const SwModHost* owner, void* state, int count) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (count < 0 || count > ApiGetTop(state)) return SW_LUA_OUT_OF_RANGE;
    *(uintptr_t*)(reinterpret_cast<uintptr_t>(state) + 0x10) -= static_cast<uintptr_t>(count) * 16;
    return SW_LUA_OK;
}
SwLuaStatus ExtrasRaiseError(const SwModHost* owner, void* state, const char* bytes, size_t length) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if ((!bytes && length) || length > SW_LUA_MAX_STRING_BYTES) return SW_LUA_INVALID_ARGUMENT;
    return SW_LUA_UNSUPPORTED;
}
SwLuaStatus ExtrasCopyString(const SwModHost* owner, void* state, int index,
                             char* out, size_t capacity, size_t* length) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (!length || (!out && capacity)) return SW_LUA_INVALID_ARGUMENT;
    *length = 0;
    if (capacity) out[0] = 0;
    int absolute = 0;
    status = StackIndex(state, index, &absolute);
    if (status != SW_LUA_OK) return status;
    uintptr_t tv = g_index2addr(reinterpret_cast<uintptr_t>(state), absolute);
    uint32_t tag = *(uint32_t*)(tv + 8);
    if ((tag & 0xf) != 4) return SW_LUA_TYPE_MISMATCH;
    uintptr_t str = *(uintptr_t*)tv;
    const size_t n = (tag & 0x3f) == 4 ? *(uint8_t*)(str + 0xb) : *(size_t*)(str + 0x10);
    if (n > SW_LUA_MAX_STRING_BYTES) return SW_LUA_OUT_OF_RANGE;
    *length = n;
    if (capacity) {
        const size_t copied = (std::min)(n, capacity - 1);
        memcpy(out, reinterpret_cast<const void*>(str + 0x18), copied);
        out[copied] = 0;
    }
    // NULL/0 is an explicit length query, not a truncating copy.
    return capacity && capacity <= n ? SW_LUA_BUFFER_TOO_SMALL : SW_LUA_OK;
}
SwLuaStatus ExtrasGetGlobal(const SwModHost* owner, void* state, const char* name) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (!name) return SW_LUA_INVALID_ARGUMENT;
    char copy[256];
    size_t n = 0;
    for (; n < sizeof(copy); ++n) { copy[n] = name[n]; if (!copy[n]) break; }
    if (!n || n == sizeof(copy)) return SW_LUA_INVALID_ARGUMENT;
    if (!g_rawgeti) return SW_LUA_UNSUPPORTED;
    LuaOperationData data{LuaOperation::GetGlobal, 0, copy, n, 0};
    status = RunLuaOperation(state, &data, 2);
    if (status == SW_LUA_OK && data.index == -1) {
        *(uintptr_t*)(reinterpret_cast<uintptr_t>(state) + 0x10) -= 16; // drop the placeholder nil
        return SW_LUA_UNSUPPORTED;
    }
    return status;
}
// lua_pcallk owns its setjmp scope: an error raised by the callee unwinds only
// engine frames above this call and comes back as a status. No plugin frame, lock
// or C++ destructor sits between the raise and that scope.
SwLuaStatus ExtrasCall(const SwModHost* owner, void* state, int argumentCount, int resultCount,
                       char* errorOut, size_t errorCapacity) {
    auto status = ExtrasContext(owner, state);
    if (status != SW_LUA_OK) return status;
    if (errorOut && errorCapacity) errorOut[0] = 0;
    if (argumentCount < 0 || argumentCount > 255 || resultCount < 0 || resultCount > 255 ||
        (!errorOut && errorCapacity)) return SW_LUA_INVALID_ARGUMENT;
    if (!g_pcallk) return SW_LUA_UNSUPPORTED;
    uintptr_t L = reinterpret_cast<uintptr_t>(state);
    const int top = LuaGetTop(L);
    if (top < argumentCount + 1) return SW_LUA_OUT_OF_RANGE;
    const int functionIndex = top - argumentCount;
    if ((*(uint32_t*)(g_index2addr(L, functionIndex) + 8) & 0xf) != 6) return SW_LUA_TYPE_MISMATCH;
    if (!g_checkstack(L, resultCount + 1)) return SW_LUA_STACK_LIMIT;
    // Keep the dispatcher's frame: the callee may reenter another plugin callback.
    LuaCallbackFrame* frame = g_luaCallbackFrame;
    const int failed = g_pcallk(L, argumentCount, resultCount, 0, 0, nullptr);
    g_luaCallbackFrame = frame;
    if (!failed) return SW_LUA_OK;
    // The function and its arguments are gone; exactly one error object is on top.
    if (LuaGetTop(L) >= functionIndex) {
        if (errorOut && errorCapacity) {
            uintptr_t tv = g_index2addr(L, functionIndex);
            uint32_t tag = *(uint32_t*)(tv + 8);
            if ((tag & 0xf) == 4) {
                uintptr_t str = *(uintptr_t*)tv;
                size_t n = (tag & 0x3f) == 4 ? *(uint8_t*)(str + 0xb) : *(size_t*)(str + 0x10);
                n = (std::min)(n, errorCapacity - 1);
                memcpy(errorOut, reinterpret_cast<const void*>(str + 0x18), n);
                errorOut[n] = 0;
            }
        }
        *(uintptr_t*)(L + 0x10) -= static_cast<uintptr_t>(LuaGetTop(L) - functionIndex + 1) * 16;
    }
    return SW_LUA_CALL_FAILED;
}
const SwLuaExtrasApi g_luaExtras = {sizeof(SwLuaExtrasApi), &ExtrasPushString, &ExtrasPushNil,
    &ExtrasPushInteger, &ExtrasCreateTable, &ExtrasGetField, &ExtrasSetField, &ExtrasGetType,
    &ExtrasPop, &ExtrasRaiseError, &ExtrasCopyString, &ExtrasGetGlobal, &ExtrasCall};

struct QolCallData { int (*fn)(uintptr_t); int result; };
void QolCallLeaf(uintptr_t L, void* data) {
    auto* call = static_cast<QolCallData*>(data);
    call->result = call->fn(L);
}
int InvokeQol(void* state, int (*fn)(uintptr_t), size_t contextOffset) {
    if (!HasLuaContext(state) || !ApiCheckStack(state, 40)) return 0;
    uintptr_t L = reinterpret_cast<uintptr_t>(state);
    uintptr_t self = g_luaCallbackFrame->self;
    if (!self) return 0;
    // QoL reads microcontroller fields of `self`; an addon script object has none.
    auto vm = g_liveLuaVms.find(L);
    if (vm == g_liveLuaVms.end() || vm->second.addon) return 0;
    uintptr_t saved = g_upvalueOverride;
    int result = 0;
    __try {
        g_upvalueOverride = self + contextOffset;
        // Only the two table builders allocate in Lua; both are POD-only leaves.
        // Never place arbitrary plugin code, or C++ QoL string/map frames, inside setjmp.
        if (fn == &Lua_Input_GetAllNumbers || fn == &Lua_Input_GetAllBools) {
            if (g_rawrunprotected) {
                const uintptr_t savedOffset = *(uintptr_t*)(L + 0x10) - *(uintptr_t*)(L + 0x38);
                QolCallData data{fn, 0};
                if (g_rawrunprotected(L, &QolCallLeaf, &data) == 0) result = data.result;
                else *(uintptr_t*)(L + 0x10) = *(uintptr_t*)(L + 0x38) + savedOffset;
            }
        } else result = fn(L);
    } __finally {
        g_upvalueOverride = saved;
    }
    return result;
}
#define QOL_WRAPPER(name, fn, offset) int name(void* L) { return InvokeQol(L, fn, offset); }
QOL_WRAPPER(ApiGetNumbers, Lua_Input_GetNumbers, 0x330)
QOL_WRAPPER(ApiGetNumberRange, Lua_Input_GetNumberRange, 0x330)
QOL_WRAPPER(ApiGetBools, Lua_Input_GetBools, 0x330)
QOL_WRAPPER(ApiGetBoolRange, Lua_Input_GetBoolRange, 0x330)
QOL_WRAPPER(ApiGetAllNumbers, Lua_Input_GetAllNumbers, 0x330)
QOL_WRAPPER(ApiGetAllBools, Lua_Input_GetAllBools, 0x330)
QOL_WRAPPER(ApiGetPulse, Lua_Input_GetPulse, 0x330)
QOL_WRAPPER(ApiGetToggle, Lua_Input_GetToggle, 0x330)
QOL_WRAPPER(ApiSetNumbers, Lua_Output_SetNumbers, 0x330)
QOL_WRAPPER(ApiSetBools, Lua_Output_SetBools, 0x330)
QOL_WRAPPER(ApiCopyInputs, Lua_Output_CopyInputs, 0x330)
QOL_WRAPPER(ApiClearOutputs, Lua_Output_Clear, 0x330)
QOL_WRAPPER(ApiDrawTextCentered, Lua_Screen_DrawTextCentered, 0x60)
QOL_WRAPPER(ApiDrawTextRight, Lua_Screen_DrawTextRight, 0x60)
QOL_WRAPPER(ApiDrawArc, Lua_Screen_DrawArc, 0x60)
QOL_WRAPPER(ApiDrawGauge, Lua_Screen_DrawGauge, 0x60)
QOL_WRAPPER(ApiDrawPoly, Lua_Screen_DrawPoly, 0x60)
QOL_WRAPPER(ApiDrawPolyF, Lua_Screen_DrawPolyF, 0x60)
QOL_WRAPPER(ApiDrawButton, Lua_Screen_DrawButton, 0x60)
QOL_WRAPPER(ApiSetColorHex, Lua_Screen_SetColorHex, 0x60)
QOL_WRAPPER(ApiSetColorHSV, Lua_Screen_SetColorHSV, 0x60)
QOL_WRAPPER(ApiClamp, Lua_Math_Clamp, 0)
QOL_WRAPPER(ApiLerp, Lua_Math_Lerp, 0)
QOL_WRAPPER(ApiInvLerp, Lua_Math_InvLerp, 0)
QOL_WRAPPER(ApiRemap, Lua_Math_Remap, 0)
QOL_WRAPPER(ApiRound, Lua_Math_Round, 0)
QOL_WRAPPER(ApiSign, Lua_Math_Sign, 0)
QOL_WRAPPER(ApiWrap, Lua_Math_Wrap, 0)
QOL_WRAPPER(ApiDeadzone, Lua_Math_Deadzone, 0)
QOL_WRAPPER(ApiSmooth, Lua_Math_Smooth, 0)
QOL_WRAPPER(ApiPid, Lua_Qol_Pid, 0)
QOL_WRAPPER(ApiResetPid, Lua_Qol_ResetPid, 0)
QOL_WRAPPER(ApiLog, Lua_Debug_Log, 0)
QOL_WRAPPER(ApiNotify, Lua_Debug_Notify, 0)
#undef QOL_WRAPPER

const SwLuaQolApi g_qolApi = {sizeof(SwLuaQolApi),
    &ApiGetNumbers, &ApiGetNumberRange, &ApiGetBools, &ApiGetBoolRange, &ApiGetAllNumbers,
    &ApiGetAllBools, &ApiGetPulse, &ApiGetToggle, &ApiSetNumbers, &ApiSetBools, &ApiCopyInputs,
    &ApiClearOutputs, &ApiDrawTextCentered, &ApiDrawTextRight, &ApiDrawArc, &ApiDrawGauge,
    &ApiDrawPoly, &ApiDrawPolyF, &ApiDrawButton, &ApiSetColorHex, &ApiSetColorHSV,
    &ApiClamp, &ApiLerp, &ApiInvLerp, &ApiRemap, &ApiRound, &ApiSign, &ApiWrap, &ApiDeadzone,
    &ApiSmooth, &ApiPid, &ApiResetPid, &ApiLog, &ApiNotify};
}

void swml::ReleaseNativeLuaOwner(const SwModHost* owner) {
    // Wait for an in-flight plugin Lua callback before allowing FreeLibrary.
    std::lock_guard<std::recursive_mutex> lock(g_libraryMutex);
    for (auto& e : g_libraryFunctions) {
        if (e->owner == owner) { e->owner = nullptr; e->callback = nullptr; e->userData = nullptr; e->tick = nullptr; }
    }
}

const SwLuaApi* swml::NativeLuaPrimitives() {
    static const SwLuaApi api = {sizeof(SwLuaApi), nullptr, nullptr, &RegisterLibrary,
        &ApiGetTop, &ApiArgNumber, &ApiArgBool, &ApiArgString, &ApiCheckStack, &ApiPushNumber, &ApiPushBool, &g_qolApi,
        nullptr, &RegisterAddonLibrary};
    return &api;
}

// Declaration is appended to native_lua.h by the integration owner.
namespace swml { const SwLuaExtrasApi* NativeLuaExtras() { return &g_luaExtras; } }

void swml::DisableNativeLua() {
    g_enabled = false;
    // Existing Lua closures and timeout callbacks point into the loader, not the plugin.
    // Keep successfully installed hooks alive for the process lifetime. New VMs use vanilla
    // libraries while disabled; existing VMs keep their extensions until recreated.
    if (g_installed) {
        g_host = nullptr;
        return;
    }
    if (g_mcLuaVt && g_origMcLuaDestroy) {
        PatchVtableSlot(g_mcLuaVt, 1, reinterpret_cast<void*>(g_origMcLuaDestroy), nullptr);
    }
    if (g_mcLuaVt && g_origMcLuaInit) {
        PatchVtableSlot(g_mcLuaVt, 3, reinterpret_cast<void*>(g_origMcLuaInit), nullptr);
    }
    if (g_scriptTabVt && g_origScriptTabInit) {
        PatchVtableSlot(g_scriptTabVt, 1, reinterpret_cast<void*>(g_origScriptTabInit), nullptr);
    }
    if (g_scriptTabVt && g_origScriptTabUpdate) {
        PatchVtableSlot(g_scriptTabVt, 3, reinterpret_cast<void*>(g_origScriptTabUpdate), nullptr);
    }
    if (g_helpTabVt && g_origHelpTabInit) {
        PatchVtableSlot(g_helpTabVt, 1, reinterpret_cast<void*>(g_origHelpTabInit), nullptr);
    }
    g_mcStates.clear();
    if (g_host && g_host->log) {
        g_host->log("mc_lua_qol", "Hooks Lua QoL retires proprement et limite d'origine restauree");
    }
    g_host = nullptr;
}
