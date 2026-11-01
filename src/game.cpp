#include "game.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "engine_discovery.h"
#include "engine_bindings.h"

namespace sw {
namespace {

// Identité de l'exécutable pour lequel les RVA ci-dessous ont été relevés (Ghidra 12.1.4).
// SHA-256 : 55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3
constexpr uint32_t kTimeDateStamp = 0x6a392154;
constexpr uint32_t kSizeOfImage = 0x00d89000;

struct FuncSpec {
    const char* name;
    uint32_t rva;
    const char* pattern;  // octets hex, « ?? » = joker (déplacements relatifs, cibles d'appel)
};

// Les patterns sont les premiers octets de chaque fonction avec les `call rel32` / `jmp` /
// opérandes RIP-relatifs masqués. Ils valident que le RVA tombe bien sur la bonne fonction.
constexpr FuncSpec kOpNew = {"operator new (FUN_140957478)", 0x957478,
    "40 53 48 83 EC 20 48 8B D9 ?? ?? 48 8B CB ?? ?? ?? ?? ?? 85 C0 ?? ?? 48 8B CB ?? ?? ?? ?? ?? "
    "48 85 C0 ?? ?? 48 83 C4 20"};
constexpr FuncSpec kStrReserve = {"string reserve (FUN_14017cb00)", 0x17cb00,
    "48 89 5C 24 08 57 48 83 EC 20 BB 20 00 00 00 48 8B F9 3B D3 ?? ?? 03 DB 3B DA ?? ?? 3B 59 0C "
    "?? ?? 48 8B 09 8B C3 48 85 C9"};
constexpr FuncSpec kStrFree = {"string free (FUN_1409b15a0)", 0x9b15a0,
    "48 83 EC 28 48 85 C9 ?? ?? 48 83 E1 F8 48 8B 49 F8 ?? ?? ?? ?? ?? 48 83 C4 28"};
constexpr FuncSpec kButton = {"point_button factory (FUN_140262680)", 0x262680,
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 81 EC A0 00 00 00 0F 29 70 D8 "
    "0F 29 78 C8 44 0F 29 40 B8 41 8B F9 0F 28 F2 0F 28 F9"};
constexpr FuncSpec kText = {"c_ui_element_text ctor (FUN_140245990)", 0x245990,
    "4C 8B DC 49 89 5B 18 49 89 4B 08 55 56 57 48 83 EC 30 48 8B F9 C7 44 24 58 00 00 00 00 ?? ?? "
    "?? ?? ?? ?? ?? 48 89 01 F3 0F 11 49 08 F3 0F 11 51 0C F3 0F 11 59 10 F3 0F 10 4C 24 70 F3 0F "
    "11 49 14 8B 44 24 78 89 41 18 66 C7 41 1C 01 00 C6 41 1E 00 33 ED 48 89 69 20 C7 41 28 00 00 "
    "80 3F ?? ?? ?? ?? ?? ?? ?? 48 89 01 48 8D 41 30 49 89 43 D8 48 89 28 48 89 68 08 89 68 10 66 "
    "89 69 48 8B 44 24 58"};
constexpr FuncSpec kRect = {"c_ui_element_rectangle ctor (FUN_140244540)", 0x244540,
    "48 89 4C 24 08 48 83 EC 18 C7 44 24 28 00 00 00 00 ?? ?? ?? ?? ?? ?? ?? 48 89 01 F3 0F 11 49 "
    "08 F3 0F 11 51 0C F3 0F 11 59 10 F3 0F 10 4C 24 40 F3 0F 11 49 14 8B 44 24 48 89 41 18 66 C7 "
    "41 1C 01 00 C6 41 1E 00 45 33 C0 4C 89 41 20 C7 41 28 00 00 80 3F ?? ?? ?? ?? ?? ?? ?? 48 89 "
    "01 48 8D 41 30 48 89 04 24 4C 89 00 4C 89 40 08 44 89 40 10 66 44 89 41 48 8B 44 24 28"};
constexpr FuncSpec kSetText = {"c_ui_element_text::set_text (FUN_140245de0)", 0x245de0,
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 41 60 49 8B D8 48 8B EA 48 8B "
    "F1 41 3B 40 08 ?? ?? 48 8B 13"};
constexpr FuncSpec kAddChild = {"add_child (FUN_1408f5680)", 0x8f5680,
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F1 49 8B F8 8B 49 40 48 8B "
    "EA 44 8B 4E 38 41 3B C9 ?? ?? 43 8D 14 09 B8 20 00 00 00 3B D0 48 8D 4E 30 0F 42 D0 ?? ?? ?? "
    "?? ?? 8B 4E 40 44 8B 4E 38 8B 46 3C FF C1 FF C8 89 4E 40"};
constexpr FuncSpec kSetView = {"main_menu_interface::set_view (FUN_140702d10)", 0x702d10,
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 49 8B D8 48 8B "
    "F2 48 8B F9 44 89 49 58 48 8B 89 F0 00 00 00"};
constexpr FuncSpec kLocalized = {"localized string (FUN_14020bfd0)", 0x20bfd0,
    "48 89 5C 24 10 48 89 74 24 18 48 89 4C 24 08 57 48 83 EC 30 48 8B F9 33 F6 89 74 24 20 48 63 "
    "C2 48 8D 14 40 48 8D 1C D5 00 00 00 00"};
constexpr FuncSpec kScroll = {"c_ui_element_scroll_container ctor (FUN_14024ac00)", 0x24ac00,
    "48 89 4C 24 08 48 83 EC 18 C7 44 24 28 00 00 00 00 48 8D 05 E0 AF 93 00 48 89 01 F3 0F 11 49 "
    "08 F3 0F 11 51 0C F3 0F 11 59 10 F3 0F 10 4C 24 40 F3 0F 11 49 14 8B 44 24 48 89 41 18 66 C7 "
    "41 1C 01 00 C6 41 1E 00 45 33 C0 4C 89 41 20 C7 41 28 00 00 80 3F 48 8D 05 85 09 95 00 48 89 "
    "01 48 8D 41 30 48 89 04 24 4C 89 00 4C 89 40 08 44 89 40 10 66 44 89 41 48 8B 44 24 28 89 41 "
    "4A 41 0F B6 C0 BA 01 00 00 00 0F 57 C0 0F 2F C3 0F 47 C2 88 41 48 0F B6 41 49"};
constexpr FuncSpec kScrollAdd = {"scroll_container::add_item (FUN_1409253e0)", 0x9253e0,
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B EA 48 8B D9 ?? ?? "
    "?? ?? ?? 8B 4B 68 44 8B 43 60 41 3B C8 ?? ?? 43 8D 14 00 B8 20 00 00 00 3B D0 48 8D 4B 58"};
constexpr FuncSpec kCheckbox = {"c_ui_element_checkbox ctor (FUN_14025b870)", 0x25b870,
    "48 89 4C 24 08 48 83 EC 18 C7 44 24 28 00 00 00 00 48 8D 05 70 A3 92 00 48 89 01 F3 0F 11 49 "
    "08 F3 0F 11 51 0C F3 0F 11 59 10 F3 0F 11 59 14 8B 44 24 40 89 41 18 66 C7 41 1C 01 00 C6 41 "
    "1E 00 33 D2 48 89 51 20 C7 41 28 00 00 80 3F 48 8D 05 1C FD 93 00 48 89 01 48 8D 41 30 48 89 "
    "04 24 48 89 10 48 89 50 08 89 50 10 66 89 51 48 8B 44 24 28 89 41 4A 0F 57 C0 0F 2F C3"};
constexpr FuncSpec kRectButton = {"c_ui_element_rectangle_button ctor (FUN_14024a050)", 0x24a050,
    "48 89 4C 24 08 48 83 EC 18 C7 44 24 28 00 00 00 00 48 8D 05 90 BB 93 00 48 89 01 F3 0F 11 49 "
    "08 F3 0F 11 51 0C F3 0F 11 59 10 F3 0F 10 4C 24 40 F3 0F 11 49 14 8B 44 24 48 89 41 18 66 C7 "
    "41 1C 01 00 C6 41 1E 00 45 33 C0 4C 89 41 20 C7 41 28 00 00 80 3F 48 8D 05 35 15 95 00 48 89 "
    "01 48 8D 41 30 48 89 04 24 4C 89 00 4C 89 40 08 44 89 40 10 66 44 89 41 48 8B 44 24 28 89 41 "
    "4A 41 0F B6 C0 BA 01 00 00 00 0F 57 C0 0F 2F C3 0F 47 C2 88 41 48 0F B6 41 49"};
constexpr FuncSpec kHeading = {"c_ui_element_heading ctor (FUN_140259c30)", 0x259c30,
    "48 89 5C 24 18 48 89 74 24 20 48 89 4C 24 08 57 48 83 EC 30 48 8B F1 C7 44 24 48 00 00 00 00 "
    "?? ?? ?? ?? ?? ?? ?? 48 89 01 F3 0F 11 49 08 F3 0F 11 51 0C F3 0F 11 59 10 33 D2 89 51 14 8B "
    "44 24 60 89 41 18 66 C7 41 1C 01 00 88 51 1E 48 89 51 20 C7 41 28 00 00 80 3F ?? ?? ?? ?? ?? "
    "?? ?? 48 89 01 48 8D 41 30 48 89 44 24 20 48 89 10 48 89 50 08 89 50 10 66 89 51 48 8B 44 24 "
    "48 89 41 4A 0F B6 C2 B9 01 00 00 00 0F 57 C0 0F 2F C3"};
constexpr FuncSpec kRemoveChild = {"remove_child (FUN_1401dd7e0)", 0x1dd7e0,
    "48 89 5C 24 20 41 56 48 83 EC 20 44 8B 51 40 45 33 F6 4C 8B DA 41 8B DE 45 85 D2 ?? ?? ?? ?? "
    "?? ?? 44 8B 49 3C 48 89 6C 24 30 48 8B 69 30 48 89 74 24 38 8B 71 38 48 89 7C 24 40 48 8D 79 "
    "30"};
// Cibles des hooks (vérifiées aussi via le contenu des vtables)
constexpr FuncSpec kViewInit = {"default_view::init (FUN_1406fbff0)", 0x6fbff0,
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 38 01 00 00 0F 29 70 A8 0F "
    "29 78 98 44 0F 29 40 88 44 0F 29 88 78 FF FF FF 44 0F 29 90 68 FF FF FF"};
constexpr FuncSpec kViewAlpha = {"default_view::set_alpha (FUN_1406fd420)", 0x6fd420,
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 41 0F B6 F8 48 8B 49 60 48 8B F2 48 8B "
    "01 FF 90 F0 00 00 00 80 7B 68 00"};
constexpr FuncSpec kUiUpdate = {"main_menu_interface::update (FUN_140702b80)", 0x702b80,
    "48 83 EC 48 8B 41 58 4C 8B C9 ?? ?? ?? ?? ?? ?? ?? ?? 0F 29 74 24 30 83 F8 01 ?? ?? 0F 57 E4 "
    "0F 28 DA 0F 28 EA 0F 28 F2"};

constexpr uint32_t kRvaDefaultViewVtable = 0xb88818;  // c_ui_main_menu_default_view::vftable
constexpr uint32_t kRvaInterfaceVtable = 0xb904d8;    // c_ui_main_menu_interface::vftable

constexpr FuncSpec kHudUpdate = {"hud::update", 0x5c78b0,
    "40 53 48 83 EC 20 48 8B D9 ?? ?? ?? ?? ?? 8B 83 90 00 00 00 85 C0"};
constexpr FuncSpec kHudInit = {"hud::init", 0x5c7020,
    "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC C0 00 00 00"};
constexpr FuncSpec kEditorInit = {"microcontroller_editor::init", 0x640e80,
    "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC B0 00 00 00"};
constexpr FuncSpec kUiTeardown = {"composite::teardown", 0x241bc0,
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 ?? ?? ?? ?? ?? 33 C0 48 39 BB 90 01 00 00"};
constexpr FuncSpec kEditorUpdate = {"microcontroller_editor::update", 0x642000,
    "40 53 48 83 EC 20 48 8B D9 ?? ?? ?? ?? ?? 48 8B 83 18 01 00 00 C7 40 0C 9A 99 19 3E"};
constexpr FuncSpec kHudDestroy = {"hud::deleting_destructor", 0x247ed0,
    "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 8B DA 48 8B F9"};
constexpr FuncSpec kEditorDestroy = {"microcontroller_editor::deleting_destructor", 0x643380,
    "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 8B DA 48 8B F9 48 81 C1 28 01 00 00"};

bool MatchAt(const uint8_t* p, const char* pattern) {
    const char* s = pattern;
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?' && s[1] == '?') {
            ++p;
            s += 2;
            continue;
        }
        char hex[3] = {s[0], s[1], 0};
        if (*p != (uint8_t)strtoul(hex, nullptr, 16)) return false;
        ++p;
        s += 2;
    }
    return true;
}

bool Locate(uint8_t* base, const FuncSpec& f, void** out, const swml::discovery::Result* discovered = nullptr) {
    (void)discovered;
    uint32_t rva = swml::EngineRva(base, f.rva);
    if (!rva) {
        swml::LogError("UI discovery: unresolved dependency %s", f.name);
        return false;
    }
    uint8_t* p = base + rva;
    __try {
        // The compiled profile checks the complete per-build body, including
        // its actual vtable LEA displacements. Old short patterns contain old
        // RIP operands and must not be applied to a correctly moved constructor.
        if (!swml::ValidateEngineBinding(base, f.rva)) {
            swml::Log("ERREUR: signature invalide pour %s (RVA 0x%X)", f.name, f.rva);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        swml::Log("ERREUR: lecture impossible pour %s", f.name);
        return false;
    }
    *out = p;
    return true;
}

bool CheckSlot(uint8_t* base, void** vt, size_t slot, uint32_t expectedRva, const char* what) {
    uint32_t resolved = swml::EngineRva(base, expectedRva);
    if (!resolved || vt[slot] != base + resolved) {
        swml::Log("ERREUR: %s slot %zu = %p, attendu %p", what, slot, vt[slot], base + expectedRva);
        return false;
    }
    return true;
}

}  // namespace

bool ResolveApi(Api& api, const swml::discovery::Result* discovered) {
    (void)discovered;
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    return ResolveApiImage(api, base);
}
bool ResolveApiImage(Api& api, uint8_t* base) {
    if (!base) return false;
    api.base = base;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    swml::Log("module principal @ %p, timestamp 0x%08X, SizeOfImage 0x%X (ImageBase prefere 0x%llX)",
              base, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
              (unsigned long long)nt->OptionalHeader.ImageBase);
    if (!swml::IsSupportedEngineBuild(base)) {
        swml::Log("Executable non reconnu (attendu timestamp 0x%08X / SizeOfImage 0x%X) : "
                  "aucun hook installe.", kTimeDateStamp, kSizeOfImage);
        return false;
    }

    void* v = nullptr;
    bool ok = true;
#define RESOLVE(spec, member, type)                      \
    if (Locate(base, spec, &v)) api.member = (type)v;    \
    else ok = false;
    RESOLVE(kOpNew, op_new, fn_new_t)
    RESOLVE(kStrReserve, str_reserve, fn_str_reserve_t)
    RESOLVE(kStrFree, str_free, fn_str_free_t)
    RESOLVE(kButton, make_button, fn_button_t)
    RESOLVE(kText, make_text, fn_text_t)
    RESOLVE(kRect, make_rect, fn_rect_t)
    RESOLVE(kSetText, set_text, fn_set_text_t)
    RESOLVE(kAddChild, add_child, fn_add_child_t)
    RESOLVE(kSetView, set_view, fn_set_view_t)
    RESOLVE(kLocalized, localized, fn_localized_t)
    RESOLVE(kScroll, make_scroll, fn_scroll_t)
    RESOLVE(kScrollAdd, scroll_add, fn_scroll_add_t)
    RESOLVE(kCheckbox, make_checkbox, fn_checkbox_t)
    RESOLVE(kRectButton, make_rect_button, fn_rbutton_t)
    RESOLVE(kHeading, make_heading, fn_heading_t)
    RESOLVE(kRemoveChild, remove_child, fn_remove_child_t)
#undef RESOLVE
    void *dummy;
    ok &= Locate(base, kViewInit, &dummy);
    ok &= Locate(base, kViewAlpha, &dummy);
    ok &= Locate(base, kUiUpdate, &dummy);
    if (!ok) return false;

    auto address = [base](uint32_t key) { return swml::EngineRva(base, key); };
    if (!address(kRvaDefaultViewVtable) || !address(kRvaInterfaceVtable) || !address(0xb850a8)) return false;
    api.default_view_vt = reinterpret_cast<void**>(base + address(kRvaDefaultViewVtable));
    api.interface_vt = reinterpret_cast<void**>(base + address(kRvaInterfaceVtable));
    api.composite_vt = reinterpret_cast<void**>(base + address(0xb850a8));
    ok &= CheckSlot(base, api.default_view_vt, slot::kInit, kViewInit.rva, "default_view vtable");
    ok &= CheckSlot(base, api.default_view_vt, slot::kSetAlpha, kViewAlpha.rva,
                    "default_view vtable");
    ok &= CheckSlot(base, api.interface_vt, slot::kUpdate, kUiUpdate.rva, "interface vtable");
    return ok;
}

bool ValidateIngameNotificationProfile(uint8_t* base) {
    if (!base) return false;
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            !swml::IsSupportedEngineBuild(base)) return false;
        void* dummy = nullptr;
        if (!Locate(base, kHudUpdate, &dummy) || !Locate(base, kEditorUpdate, &dummy) ||
            !Locate(base, kHudInit, &dummy) || !Locate(base, kEditorInit, &dummy) ||
            !Locate(base, kUiTeardown, &dummy) ||
            !Locate(base, kHudDestroy, &dummy) || !Locate(base, kEditorDestroy, &dummy)) return false;
        if (!swml::ValidateEngineBinding(base, 0xb423c0) || !swml::ValidateEngineBinding(base, 0xb71d60) ||
            !swml::ValidateEngineBinding(base, 0xb850a8)) return false;
        auto** hud = reinterpret_cast<void**>(base + swml::EngineRva(base, 0xb423c0));
        auto** editor = reinterpret_cast<void**>(base + swml::EngineRva(base, 0xb71d60));
        auto** composite = reinterpret_cast<void**>(base + swml::EngineRva(base, 0xb850a8));
        return CheckSlot(base, hud, 0, kHudDestroy.rva, "HUD") &&
               CheckSlot(base, hud, 1, kHudInit.rva, "HUD") &&
               CheckSlot(base, hud, 2, kUiTeardown.rva, "HUD") &&
               CheckSlot(base, hud, 3, kHudUpdate.rva, "HUD") &&
               CheckSlot(base, editor, 0, kEditorDestroy.rva, "MC editor") &&
               CheckSlot(base, editor, 1, kEditorInit.rva, "MC editor") &&
               CheckSlot(base, editor, 2, kUiTeardown.rva, "MC editor") &&
               CheckSlot(base, editor, 3, kEditorUpdate.rva, "MC editor") &&
               CheckSlot(base, composite, 0, 0x241fb0, "composite-ext") &&
               CheckSlot(base, composite, 4, 0x1dd500, "composite-ext render");
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void StrMake(const Api& api, EStr& s, const char* utf8) {
    size_t n = strlen(utf8);
    s.data = nullptr;
    s.len = (uint32_t)n;
    s.cap = 0;
    api.str_reserve(&s, (uint32_t)n + 1);
    memcpy(s.data, utf8, n + 1);
}

void StrRelease(const Api& api, EStr& s) {
    if (s.data) api.str_free(s.data);
    s.data = nullptr;
    s.len = 0;
}

}  // namespace sw
