// Interface avec stormworks64.exe : types du moteur UI, offsets de champs, pointeurs de
// fonctions internes. Toutes les adresses sont des RVA (adresse virtuelle - 0x140000000) ;
// l'adresse réelle = base du module + RVA (ASLR). Voir game.cpp pour la résolution/validation.
#pragma once
#include <cstddef>
#include <cstdint>

namespace swml { namespace discovery { struct Result; } }

namespace sw {

// Chaîne du moteur : 16 octets. data est alloué par _aligned_malloc du CRT *statique* du jeu :
// ne jamais la créer/libérer avec un autre allocateur (utiliser Api::str_reserve / Api::str_free).
struct EStr {
    char*    data;
    uint32_t len;  // sans le NUL final
    uint32_t cap;  // capacité (puissance de 2, min 32)
};

// ---- Offsets des champs (objets UI ; voir le rapport) ---------------------------------------
namespace off {
// mm_ui_element / mm_ui_element_composite / c_ui_element_composite_ext (0x58 octets)
constexpr size_t kX = 0x08, kY = 0x0c, kW = 0x10, kH = 0x14, kAnchor = 0x18;
constexpr size_t kVisible = 0x1c;   // u8 : dessin + mise à jour + entrées
constexpr size_t kParent = 0x20;    // mm_ui_element*
constexpr size_t kScale = 0x28;     // float (1.0)
// c_ui_element_point_button
constexpr size_t kBtnCallback = 0xd8;  // void (*)(void* ctx, void* userdata)
constexpr size_t kBtnUserData = 0xe0;
// c_ui_element_rectangle_button : callback au relâchement
constexpr size_t kRectBtnCallback = 0xb8;
constexpr size_t kRectBtnUserData = 0xc0;
// c_ui_element_checkbox : état coché (u8)
constexpr size_t kCheckboxState = 0x70;
// ctx (état + 0xbd6f0) : éléments sous le curseur / capturés, à effacer avant de détruire une branche
constexpr size_t kCtxHovered = 0x190, kCtxFocused = 0x188;
// c_ui_main_menu_default_view
constexpr size_t kDvLaunchMode = 0x68;  // u8 : mode compte à rebours (seul Quit créé)
constexpr size_t kDvNewGame = 0x70, kDvLoadGame = 0x78, kDvContinue = 0x80;
constexpr size_t kDvSettings = 0x88, kDvQuit = 0x90, kDvMultiplayer = 0x98;
// c_ui_main_menu_interface
constexpr size_t kUiActiveView = 0x58;  // int
}  // namespace off

// Indices de vtable (slot * 8 = offset)
namespace slot {
constexpr size_t kInit = 1;      // init(this, ctx, ...)
constexpr size_t kUpdate = 3;    // update(this, ?, ctx, ?)
constexpr size_t kSetAlpha = 30; // set_alpha(this, ctx, u8)
}  // namespace slot

constexpr int kActionBack = 0x1e;  // action « retour » du dispatcher FUN_14083b990

// ---- Prototypes des fonctions internes du jeu (x64 MSVC) -------------------------------------
using fn_new_t = void* (*)(size_t);                                  // FUN_140957478  operator new
using fn_str_reserve_t = void (*)(EStr*, uint32_t);                  // FUN_14017cb00
using fn_str_free_t = void (*)(void*);                               // FUN_1409b15a0
using fn_button_t = void* (*)(void* ctx, float x, float y, int anchor, EStr* icon, EStr* label,
                              uint32_t labelPos, int action, float labelWidth, float size);
                                                                     // FUN_140262680  point_button
using fn_text_t = void* (*)(void* mem, float x, float y, float w, float h, int anchor, EStr* text,
                            int align, uint32_t argb, float fontSize, float lineSpacing,
                            bool autoHeight);                        // FUN_140245990  c_ui_element_text
using fn_rect_t = void* (*)(void* mem, float x, float y, float w, float h, int anchor,
                            uint32_t argb);                          // FUN_140244540  rectangle
using fn_set_text_t = void (*)(void* text, void* ctx, EStr* s);      // FUN_140245de0
using fn_add_child_t = void* (*)(void* parent, void* ctx, void* child);  // FUN_1408f5680
using fn_set_view_t = void (*)(void* ui, void* ctx, void* data, int index, void* extra);
                                                                     // FUN_140702d10
using fn_localized_t = EStr* (*)(EStr* out, int id);                 // FUN_14020bfd0
// Widgets de liste du jeu (utilisés par c_ui_mods_selector)
using fn_scroll_t = void* (*)(void* mem, float x, float y, float w, float h, int anchor,
                              float itemSpacing);                    // FUN_14024ac00  scroll_container (0xb0)
using fn_scroll_add_t = void* (*)(void* scroll, void* ctx, void* item);  // FUN_1409253e0  empile l'item
using fn_checkbox_t = void* (*)(void* mem, float x, float y, float size, int anchor);
                                                                     // FUN_14025b870  checkbox (0x78), état en +0x70
using fn_rbutton_t = void* (*)(void* mem, float x, float y, float w, float h, int anchor,
                               int action, uint32_t colNormal, uint32_t colHover,
                               uint32_t colPressed, bool rounded);   // FUN_14024a050  rectangle_button (0xc8)
using fn_heading_t = void* (*)(void* mem, float x, float y, float w, int anchor, EStr* text,
                               float size, float spacing);           // FUN_140259c30  heading (0x80)
using fn_remove_child_t = void (*)(void* parent, void* ctx, void* child);  // FUN_1401dd7e0

struct Api {
    uint8_t* base = nullptr;
    fn_new_t op_new = nullptr;
    fn_str_reserve_t str_reserve = nullptr;
    fn_str_free_t str_free = nullptr;
    fn_button_t make_button = nullptr;
    fn_text_t make_text = nullptr;
    fn_rect_t make_rect = nullptr;
    fn_set_text_t set_text = nullptr;
    fn_add_child_t add_child = nullptr;
    fn_set_view_t set_view = nullptr;
    fn_localized_t localized = nullptr;
    fn_scroll_t make_scroll = nullptr;
    fn_scroll_add_t scroll_add = nullptr;
    fn_checkbox_t make_checkbox = nullptr;
    fn_rbutton_t make_rect_button = nullptr;
    fn_heading_t make_heading = nullptr;
    fn_remove_child_t remove_child = nullptr;

    void** default_view_vt = nullptr;  // c_ui_main_menu_default_view::vftable
    void** interface_vt = nullptr;     // c_ui_main_menu_interface::vftable
    void** composite_vt = nullptr;     // c_ui_element_composite_ext, central version profile
};

// Résout et valide toutes les adresses pour le module principal du processus.
// Retourne false (et écrit la raison dans le log) si l'exécutable n'est pas la version attendue.
bool ResolveApi(Api& out, const swml::discovery::Result* discovered = nullptr);
// Read-only bindings validation on a mapped image; does not execute game code.
bool ResolveApiImage(Api& out, uint8_t* base);
bool ValidateIngameNotificationProfile(uint8_t* base);

// Helpers chaînes moteur
void StrMake(const Api& api, EStr& s, const char* utf8);   // alloue via l'allocateur du jeu
void StrRelease(const Api& api, EStr& s);                  // libère (si le jeu ne s'en charge pas)

}  // namespace sw
