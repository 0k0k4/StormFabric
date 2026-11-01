// Bouton « MODS » + vue native C++ Mods (Liste des mods, Paramètres & Debug, Visionneuse de logs)
// + Overlay de notifications natif en haut à gauche (ERROR / WARN / INFO).

#include "mods_ui.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "game.h"
#include "engine_api.h"
#include "engine_discovery.h"
#include "engine_bindings.h"
#include "hook_transaction.h"
#include "ingame_notifications.h"
#include "mod_runtime.h"
#include "mod_native_ui.h"
#include "log.h"
#include "mods_loader.h"
#include "mods_scan.h"

namespace swml {
namespace {

constexpr int kModsView = 0x13;  // index de vue libre (le jeu utilise 0..0x12)
constexpr float kPanelW = 2.4f, kPanelH = 1.8f;
constexpr float kRowH = 0.14f;
constexpr size_t kMaxNotifSlots = 4;

// Couleurs ABGR du moteur UI
constexpr uint32_t kColText = 0xffe6e6e6;
constexpr uint32_t kColAccent = 0xff5678c0;      // orange Stormworks (LOADED / INFO)
constexpr uint32_t kColWarn = 0xff2cb8f5;        // jaune-ambre (WARN)
constexpr uint32_t kColError = 0xff4444f2;       // rouge vif (ERROR)
constexpr uint32_t kColDim = 0xff9a9a9a;
constexpr uint32_t kColOff = 0xff7a7a7a;
constexpr uint32_t kColBackground = 0xeb141616;  // fond des c_ui_element_view_container
constexpr uint32_t kColNotifBg = 0xf0181a1d;     // fond sombre des notifications en haut à gauche
constexpr uint32_t kColRow = 0xff27292a, kColRowHover = 0xff4d443a, kColRowPress = 0xff5678c0;

enum class TabMode { ModsList = 0, SettingsDebug = 1, LiveLogs = 2 };

enum Cmd : intptr_t {
    kCmdNone = 0,
    kCmdBack,
    kCmdTabMods,
    kCmdTabSettings,
    kCmdTabLogs,
    kCmdEnableAll,
    kCmdDisableAll,
    kCmdRefresh,
    kCmdOpenFolder,
    kCmdToggleTerminal,
    kCmdTestNotifs,
    kCmdForceReload,
    kCmdDumpDiag,
    kCmdOpenLogFile,
    kCmdClearLogs
};

sw::Api g_api;

using ViewInitFn = void (*)(void*, void*, void*, void*);
using ViewAlphaFn = void (*)(void*, void*, uint8_t);
using UiUpdateFn = void (*)(void*, void*, void*, void*);
ViewInitFn g_origViewInit = nullptr;
ViewAlphaFn g_origViewAlpha = nullptr;
UiUpdateFn g_origUiUpdate = nullptr;

struct Row {
    int index = 0;                // index du mod (en mode ModsList) ou de l'option (en mode Settings)
    bool isSetting = false;
    void* check = nullptr;        // c_ui_element_checkbox
    void* nameOn = nullptr;       // texte actif (blanc)
    void* nameOff = nullptr;      // texte inactif (grisé)
    void* stateLoaded = nullptr;  // « LOADED » / « ON » (orange)
    void* stateDisabled = nullptr;// « DISABLED » / « OFF » (gris)
    void* stateError = nullptr;   // erreur / « NEED: ... » (rouge-corail)
};

struct NotifSlotUi {
    void* root = nullptr;
    void* barInfo = nullptr;
    void* barWarn = nullptr;
    void* barError = nullptr;
    void* hdrInfo = nullptr;
    void* hdrWarn = nullptr;
    void* hdrError = nullptr;
    void* detail = nullptr;
};

struct State {
    void* ctx = nullptr;          // c_ui context (état + 0xbd6f0)
    void* view = nullptr;         // c_ui_main_menu_default_view courante
    void* button = nullptr;       // bouton MODS du menu principal
    void* compositeVt = nullptr;
    // créés dans l'update de l'interface
    void* ui = nullptr;
    void* panel = nullptr;
    void* heading = nullptr;
    void* subtitle = nullptr;
    void* scroll = nullptr;
    void* buttons[9] = {};
    float headingH = 0.1f;

    // Overlay de notifications en haut à gauche
    void* notifContainer = nullptr;
    NotifSlotUi notifSlots[kMaxNotifSlots] = {};
    uint64_t lastNotifRev = 0;

    TabMode tab = TabMode::ModsList;
    bool wasActive = false;
    bool openRequested = false;
    int pendingModToggle = -1;
    int pendingSettingToggle = -1;
    Cmd pendingCmd = kCmdNone;

    std::vector<ModEntry> mods;
    std::vector<std::unique_ptr<Row>> rows;
};
State g_st;

inline uint8_t* B(void* p) { return reinterpret_cast<uint8_t*>(p); }
template <class T>
inline T& Field(void* obj, size_t off) { return *reinterpret_cast<T*>(B(obj) + off); }

inline float FloatFromBits(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void CallSetAlpha(void* element, void* ctx, uint8_t alpha) {
    void** vt = *reinterpret_cast<void***>(element);
    reinterpret_cast<ViewAlphaFn>(vt[sw::slot::kSetAlpha])(element, ctx, alpha);
}

void* NewComposite(float x, float y, float w, float h, int anchor) {
    uint8_t* m = B(g_api.op_new(0x58));
    memset(m, 0, 0x58);
    *reinterpret_cast<void**>(m) = g_st.compositeVt;
    Field<float>(m, sw::off::kX) = x;
    Field<float>(m, sw::off::kY) = y;
    Field<float>(m, sw::off::kW) = w;
    Field<float>(m, sw::off::kH) = h;
    Field<int>(m, sw::off::kAnchor) = anchor;
    m[sw::off::kVisible] = 1;
    Field<float>(m, sw::off::kScale) = 1.0f;
    m[0x48] = w < 0.0f;
    m[0x49] = h < 0.0f;
    return m;
}

void* NewRect(void* parent, float x, float y, float w, float h, int anchor, uint32_t abgr) {
    void* r = g_api.make_rect(g_api.op_new(0x60), x, y, w, h, anchor, abgr);
    g_api.add_child(parent, g_st.ctx, r);
    return r;
}

void* NewText(void* parent, float x, float y, float w, float h, int anchor, const std::string& utf8,
              int align, uint32_t argb, float fontSize, float lineSpacing) {
    sw::EStr s;
    sw::StrMake(g_api, s, utf8.c_str());
    void* t = g_api.make_text(g_api.op_new(200), x, y, w, h, anchor, &s, align, argb, fontSize,
                              lineSpacing, true);
    g_api.add_child(parent, g_st.ctx, t);
    return t;
}

void SetText(void* text, const std::string& utf8) {
    sw::EStr s;
    sw::StrMake(g_api, s, utf8.c_str());
    g_api.set_text(text, g_st.ctx, &s);
    sw::StrRelease(g_api, s);
}

void* NewButton(void* parent, float x, float y, int anchor, const char* icon, const char* label,
                int labelId, int action, void (*cb)(void*, void*), void* userData) {
    sw::EStr ic, lb = {};
    sw::StrMake(g_api, ic, icon);
    if (labelId >= 0) {
        g_api.localized(&lb, labelId);
    } else {
        sw::StrMake(g_api, lb, label);
    }
    constexpr float kLabelWidth = 0.42f, kSize = 0.14f;
    void* b = g_api.make_button(g_st.ctx, x, y, anchor, &ic, &lb, /*labelPos=*/1, action,
                                kLabelWidth, kSize);
    g_api.add_child(parent, g_st.ctx, b);
    if (cb) {
        Field<void*>(b, sw::off::kBtnCallback) = reinterpret_cast<void*>(cb);
        Field<void*>(b, sw::off::kBtnUserData) = userData;
    }
    return b;
}

void SetVisible(void* element, bool v) {
    if (element) Field<uint8_t>(element, sw::off::kVisible) = v ? 1 : 0;
}

std::string Truncate(std::string s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars - 3) + "...";
}

// --- Callbacks de clic -----------------------------------------------------------------------
void OnModsClicked(void*, void*) { g_st.openRequested = true; }
void OnCommand(void*, void* cmd) { g_st.pendingCmd = static_cast<Cmd>(reinterpret_cast<intptr_t>(cmd)); }
void OnModRowClicked(void*, void* row) { g_st.pendingModToggle = static_cast<Row*>(row)->index; }
void OnSettingRowClicked(void*, void* row) { g_st.pendingSettingToggle = static_cast<Row*>(row)->index; }

// --- Overlay de notifications en haut à gauche (ERROR / WARN / INFO) -------------------------

void BuildNotifOverlay(void* ui) {
    // Conteneur ancré en haut à gauche (anchor 0) de l'interface principale
    constexpr float kCardW = 0.94f, kCardH = 0.096f, kStepY = 0.108f;
    void* container = NewComposite(0.025f, -0.025f, kCardW, kStepY * kMaxNotifSlots, 0);
    g_api.add_child(ui, g_st.ctx, container);
    g_st.notifContainer = container;
    g_st.lastNotifRev = 0;

    for (size_t i = 0; i < kMaxNotifSlots; ++i) {
        NotifSlotUi& slot = g_st.notifSlots[i];
        void* card = NewComposite(0.0f, -(float)i * kStepY, kCardW, kCardH, 0);
        g_api.add_child(container, g_st.ctx, card);
        slot.root = card;

        // Fond sombre + barre latérale colorée selon la sévérité
        NewRect(card, 0.0f, 0.0f, kCardW, kCardH, 0, kColNotifBg);
        slot.barInfo  = NewRect(card, 0.0f, 0.0f, 0.015f, kCardH, 0, kColAccent);
        slot.barWarn  = NewRect(card, 0.0f, 0.0f, 0.015f, kCardH, 0, kColWarn);
        slot.barError = NewRect(card, 0.0f, 0.0f, 0.015f, kCardH, 0, kColError);

        // Titre coloré ([INFO] / [WARN] / [ERROR])
        slot.hdrInfo  = NewText(card, 0.030f, -0.012f, kCardW - 0.045f, 0.042f, 0, "", 0,
                                kColAccent, 0.00050f, 0.8f);
        slot.hdrWarn  = NewText(card, 0.030f, -0.012f, kCardW - 0.045f, 0.042f, 0, "", 0,
                                kColWarn, 0.00050f, 0.8f);
        slot.hdrError = NewText(card, 0.030f, -0.012f, kCardW - 0.045f, 0.042f, 0, "", 0,
                                kColError, 0.00050f, 0.8f);

        // Message de détail
        slot.detail = NewText(card, 0.030f, -0.052f, kCardW - 0.045f, 0.038f, 0, "", 0,
                              kColText, 0.00044f, 0.8f);

        SetVisible(card, false);
    }
}

void UpdateNotifOverlay() {
    if (!g_st.notifContainer) return;
    std::vector<NotificationItem> active;
    uint64_t rev = GetActiveNotifications(active, kMaxNotifSlots);
    if (rev == g_st.lastNotifRev) return;
    g_st.lastNotifRev = rev;

    for (size_t i = 0; i < kMaxNotifSlots; ++i) {
        NotifSlotUi& slot = g_st.notifSlots[i];
        if (i >= active.size()) {
            SetVisible(slot.root, false);
            continue;
        }
        const NotificationItem& item = active[i];
        const bool isErr = (item.level == LogLevel::Error);
        const bool isWrn = (item.level == LogLevel::Warn);
        const bool isInf = !isErr && !isWrn;

        std::string prefix = isErr ? "[ERROR]  " : (isWrn ? "[WARN]  " : "[INFO]  ");
        std::string hdrText = SafeText(Truncate(prefix + item.title, 72));
        std::string detText = SafeText(Truncate(item.detail, 88));

        SetVisible(slot.barInfo, isInf);
        SetVisible(slot.barWarn, isWrn);
        SetVisible(slot.barError, isErr);

        if (isInf) SetText(slot.hdrInfo, hdrText);
        if (isWrn) SetText(slot.hdrWarn, hdrText);
        if (isErr) SetText(slot.hdrError, hdrText);

        SetVisible(slot.hdrInfo, isInf);
        SetVisible(slot.hdrWarn, isWrn);
        SetVisible(slot.hdrError, isErr);

        SetText(slot.detail, detText);
        SetVisible(slot.root, true);
    }
}

// --- Gestion des options de Settings & Debug -------------------------------------------------

bool GetSettingValue(int idx) {
    const ModLoaderConfig& c = GetConfig();
    switch (idx) {
        case 0: return c.openTerminalOnStartup;
        case 1: return c.showNotifications;
        case 2: return c.notifError;
        case 3: return c.notifWarn;
        case 4: return c.notifInfo;
        case 5: return c.verboseDebug;
        case 6: return c.hotReloadMods;
        case 7: return c.autoOpenModsMenu;
        default: return false;
    }
}

void SetSettingValue(int idx, bool val) {
    ModLoaderConfig& c = GetConfig();
    const char* name = "";
    switch (idx) {
        case 0:
            c.openTerminalOnStartup = val;
            name = "Open Log Terminal on Startup";
            SetLogTerminalOpen(val);
            break;
        case 1:
            c.showNotifications = val;
            name = "Top-Left Notifications Overlay";
            if (!val) ClearNotifications();
            break;
        case 2: c.notifError = val; name = "ERROR Notifications"; break;
        case 3: c.notifWarn = val; name = "WARN Notifications"; break;
        case 4: c.notifInfo = val; name = "INFO Notifications"; break;
        case 5: c.verboseDebug = val; name = "Verbose Debug Logging"; break;
        case 6: c.hotReloadMods = val; name = "Dynamic Hot-Reload Mods"; break;
        case 7: c.autoOpenModsMenu = val; name = "Auto-Open Mods Menu on Startup"; break;
        default: return;
    }
    SaveConfig();
    g_st.lastNotifRev = 0;  // force le rafraîchissement de l'overlay
    Notify(LogLevel::Info, std::string(name) + (val ? " : ON" : " : OFF"),
           "Setting saved to mods\\_modloader.ini");
}

// --- Mise à jour des textes et lignes de la vue C++ Mods -------------------------------------

void UpdateSubtitle() {
    if (!g_st.subtitle) return;
    std::string termState = IsLogTerminalOpen() ? "OPEN" : "CLOSED";

    if (g_st.tab == TabMode::SettingsDebug) {
        std::string s = "TAB: SETTINGS & DEBUG   |   Log Terminal: " + termState +
                        "   |   Config saved in mods\\_modloader.ini";
        SetText(g_st.subtitle, s);
        return;
    }
    if (g_st.tab == TabMode::LiveLogs) {
        auto logs = GetRecentLogs(250);
        std::string s = "TAB: LIVE LOG VIEWER (" + std::to_string(logs.size()) +
                        " recent entries, newest first)   |   Log Terminal: " + termState +
                        "   |   File: StormworksModLoader.log";
        SetText(g_st.subtitle, s);
        return;
    }

    int total = 0, enabledCount = 0, loadedCount = 0, errCount = 0;
    for (const ModEntry& m : g_st.mods) {
        if (!m.Toggleable()) continue;
        ++total;
        if (m.enabled) {
            ++enabledCount;
            if (m.IsLoaded()) ++loadedCount;
            else ++errCount;
        }
    }
    std::string s = "TAB: MODS LIST   |   " + std::to_string(total) +
                    (total == 1 ? " mod" : " mods") + "  -  " +
                    std::to_string(enabledCount) + " enabled  -  " +
                    std::to_string(loadedCount) + " loaded";
    if (errCount > 0) s += "  -  " + std::to_string(errCount) + " issue(s)";
    s += "   |   Terminal: " + termState;
    SetText(g_st.subtitle, s);
}

void ApplyModRowVisuals(Row& r) {
    const ModEntry& m = g_st.mods[r.index];
    const bool isLoaded = m.loadState == ModLoadState::Loaded;
    const bool isDisabled = !m.enabled && m.Toggleable();
    const bool isError = !isLoaded && !isDisabled;

    SetVisible(r.nameOn, isLoaded || m.enabled);
    SetVisible(r.nameOff, !(isLoaded || m.enabled));

    SetVisible(r.stateLoaded, isLoaded);
    SetVisible(r.stateDisabled, isDisabled);
    if (isError && r.stateError) SetText(r.stateError, SafeText(m.statusDetail));
    SetVisible(r.stateError, isError);
}

void ApplySettingRowVisuals(Row& r) {
    const bool on = GetSettingValue(r.index);
    if (r.check) Field<uint8_t>(r.check, sw::off::kCheckboxState) = on ? 1 : 0;
    SetVisible(r.nameOn, on);
    SetVisible(r.nameOff, !on);
    SetVisible(r.stateLoaded, on);
    SetVisible(r.stateDisabled, !on);
}

void SyncAndRefreshVisuals() {
    if (GetConfig().hotReloadMods) {
        SyncLoadedMods(g_st.mods, g_api.base, g_st.ctx, g_st.ui);
    } else {
        Notify(LogLevel::Warn, "Hot-Reload Disabled",
               "State saved in _modloader.ini (press Refresh or restart to apply)");
    }
    if (g_st.tab == TabMode::ModsList) {
        for (auto& r : g_st.rows) {
            if (r->isSetting || r->index < 0 || r->index >= (int)g_st.mods.size()) continue;
            const ModEntry& m = g_st.mods[r->index];
            if (r->check) Field<uint8_t>(r->check, sw::off::kCheckboxState) = m.enabled ? 1 : 0;
            ApplyModRowVisuals(*r);
        }
    }
    UpdateSubtitle();
}

void DestroyList() {
    g_st.pendingModToggle = -1;
    g_st.pendingSettingToggle = -1;
    g_st.rows.clear();
    if (!g_st.scroll) return;
    Field<void*>(g_st.ctx, sw::off::kCtxHovered) = nullptr;
    Field<void*>(g_st.ctx, sw::off::kCtxFocused) = nullptr;
    g_api.remove_child(g_st.panel, g_st.ctx, g_st.scroll);
    g_st.scroll = nullptr;
}

void BuildModRow(void* scroll, int index, float rowW) {
    const ModEntry& m = g_st.mods[index];
    auto row = std::make_unique<Row>();
    row->index = index;
    row->isSetting = false;
    void* ctx = g_st.ctx;

    void* root = NewComposite(0.0f, 0.0f, rowW, kRowH, 0);

    void* bg = g_api.make_rect_button(g_api.op_new(200), 0.0f, 0.0f, rowW, kRowH, 0, 0,
                                      kColRow, kColRowHover, kColRowPress, false);
    if (m.Toggleable()) {
        Field<void*>(bg, sw::off::kRectBtnCallback) = reinterpret_cast<void*>(&OnModRowClicked);
        Field<void*>(bg, sw::off::kRectBtnUserData) = row.get();
    }
    g_api.add_child(root, ctx, bg);

    if (m.Toggleable()) {
        row->check = g_api.make_checkbox(g_api.op_new(0x78), 0.04f, 0.0f, 0.07f, 3);
        Field<uint8_t>(row->check, sw::off::kCheckboxState) = m.enabled;
        g_api.add_child(root, ctx, row->check);
    }

    std::string header = m.DisplayName();
    if (!m.version.empty()) header += "  v" + m.version;
    if (!m.author.empty()) header += "   by " + m.author;
    header = SafeText(Truncate(header, 68));

    std::string subLine;
    if (!m.description.empty()) {
        subLine = Truncate(m.description, 60);
    } else if (m.hasJson && !m.id.empty()) {
        subLine = "id: " + m.id;
    }
    if (!m.dependencies.empty() || !m.nativeDeps.empty()) {
        std::string depList;
        for (const std::string& d : m.dependencies) {
            if (!depList.empty()) depList += ", ";
            depList += d;
        }
        for (const std::string& nd : m.nativeDeps) {
            if (!depList.empty()) depList += ", ";
            depList += nd;
        }
        if (!subLine.empty()) subLine += "   |   ";
        subLine += "deps: " + Truncate(depList, 40);
    }

    const float titleY = subLine.empty() ? 0.0f : 0.022f;
    row->nameOn = NewText(root, 0.16f, titleY, rowW * 0.56f, 0.07f, 3, header, 3, kColText, 0.00072f, 0.8f);
    row->nameOff = NewText(root, 0.16f, titleY, rowW * 0.56f, 0.07f, 3, header, 3, kColOff, 0.00072f, 0.8f);

    if (!subLine.empty()) {
        NewText(root, 0.16f, -0.028f, rowW * 0.56f, 0.05f, 3, SafeText(subLine), 3, kColDim,
                0.00046f, 0.8f);
    }

    std::string techInfo = m.detail;
    if (m.sizeBytes) techInfo += "   -   " + HumanSize(m.sizeBytes);
    NewText(root, -0.40f, 0.0f, 0.85f, 0.06f, 5, SafeText(techInfo), 5, kColDim, 0.00048f, 0.8f);

    row->stateLoaded =
        NewText(root, -0.04f, 0.0f, 0.34f, 0.06f, 5, "LOADED", 5, kColAccent, 0.00052f, 0.8f);
    row->stateDisabled =
        NewText(root, -0.04f, 0.0f, 0.34f, 0.06f, 5, "DISABLED", 5, kColOff, 0.00052f, 0.8f);
    row->stateError = NewText(root, -0.04f, 0.0f, 0.34f, 0.06f, 5, SafeText(m.statusDetail), 5,
                              kColError, 0.00050f, 0.8f);

    g_api.scroll_add(scroll, ctx, root);
    g_st.rows.push_back(std::move(row));
    ApplyModRowVisuals(*g_st.rows.back());
}

void BuildSettingToggleRow(void* scroll, int settingIdx, float rowW, const char* title,
                           const char* desc, const char* categoryTag) {
    auto row = std::make_unique<Row>();
    row->index = settingIdx;
    row->isSetting = true;
    void* ctx = g_st.ctx;
    const bool val = GetSettingValue(settingIdx);

    void* root = NewComposite(0.0f, 0.0f, rowW, kRowH, 0);
    void* bg = g_api.make_rect_button(g_api.op_new(200), 0.0f, 0.0f, rowW, kRowH, 0, 0,
                                      kColRow, kColRowHover, kColRowPress, false);
    Field<void*>(bg, sw::off::kRectBtnCallback) = reinterpret_cast<void*>(&OnSettingRowClicked);
    Field<void*>(bg, sw::off::kRectBtnUserData) = row.get();
    g_api.add_child(root, ctx, bg);

    row->check = g_api.make_checkbox(g_api.op_new(0x78), 0.04f, 0.0f, 0.07f, 3);
    Field<uint8_t>(row->check, sw::off::kCheckboxState) = val ? 1 : 0;
    g_api.add_child(root, ctx, row->check);

    row->nameOn = NewText(root, 0.16f, 0.022f, rowW * 0.65f, 0.07f, 3, title, 3, kColText, 0.00070f, 0.8f);
    row->nameOff = NewText(root, 0.16f, 0.022f, rowW * 0.65f, 0.07f, 3, title, 3, kColOff, 0.00070f, 0.8f);
    NewText(root, 0.16f, -0.028f, rowW * 0.68f, 0.05f, 3, desc, 3, kColDim, 0.00046f, 0.8f);

    NewText(root, -0.36f, 0.0f, 0.50f, 0.06f, 5, categoryTag, 5, kColDim, 0.00046f, 0.8f);
    row->stateLoaded =
        NewText(root, -0.04f, 0.0f, 0.25f, 0.06f, 5, "ON", 5, kColAccent, 0.00054f, 0.8f);
    row->stateDisabled =
        NewText(root, -0.04f, 0.0f, 0.25f, 0.06f, 5, "OFF", 5, kColOff, 0.00054f, 0.8f);

    g_api.scroll_add(scroll, ctx, root);
    g_st.rows.push_back(std::move(row));
    ApplySettingRowVisuals(*g_st.rows.back());
}

void BuildDebugActionRow(void* scroll, float rowW, const char* title, const char* desc,
                         const char* badge, Cmd cmd) {
    void* ctx = g_st.ctx;
    void* root = NewComposite(0.0f, 0.0f, rowW, kRowH, 0);
    void* bg = g_api.make_rect_button(g_api.op_new(200), 0.0f, 0.0f, rowW, kRowH, 0, 0,
                                      kColRow, kColRowHover, kColRowPress, false);
    Field<void*>(bg, sw::off::kRectBtnCallback) = reinterpret_cast<void*>(&OnCommand);
    Field<void*>(bg, sw::off::kRectBtnUserData) = reinterpret_cast<void*>(cmd);
    g_api.add_child(root, ctx, bg);

    NewText(root, 0.05f, 0.022f, rowW * 0.72f, 0.07f, 3, title, 3, kColAccent, 0.00070f, 0.8f);
    NewText(root, 0.05f, -0.028f, rowW * 0.72f, 0.05f, 3, desc, 3, kColDim, 0.00046f, 0.8f);
    NewText(root, -0.04f, 0.0f, 0.34f, 0.06f, 5, badge, 5, kColText, 0.00052f, 0.8f);

    g_api.scroll_add(scroll, ctx, root);
}

void BuildLogEntryRow(void* scroll, const LogEntry& e, float rowW) {
    void* ctx = g_st.ctx;
    constexpr float kLogRowH = 0.095f;
    void* root = NewComposite(0.0f, 0.0f, rowW, kLogRowH, 0);
    NewRect(root, 0.0f, 0.0f, rowW, kLogRowH, 0, kColRow);

    uint32_t col = kColText;
    const char* tag = "[INFO]";
    if (e.level == LogLevel::Error) {
        col = kColError;
        tag = "[ERROR]";
    } else if (e.level == LogLevel::Warn) {
        col = kColWarn;
        tag = "[WARN]";
    } else if (e.level == LogLevel::Debug) {
        col = kColDim;
        tag = "[DEBUG]";
    } else {
        col = kColAccent;
        tag = "[INFO]";
    }

    NewRect(root, 0.0f, 0.0f, 0.012f, kLogRowH, 0, col);
    std::string left = "[" + e.timeStr + "]  " + tag;
    NewText(root, 0.03f, 0.018f, 0.60f, 0.04f, 3, left, 3, col, 0.00046f, 0.8f);
    NewText(root, 0.03f, -0.018f, rowW - 0.06f, 0.045f, 3, SafeText(Truncate(e.message, 115)), 3,
            e.level == LogLevel::Debug ? kColDim : kColText, 0.00046f, 0.8f);

    g_api.scroll_add(scroll, ctx, root);
}

void BuildCurrentTabView() {
    DestroyList();
    const float top = g_st.headingH + 0.10f;
    const float h = kPanelH - top - 0.34f;
    const float spacing = (g_st.tab == TabMode::LiveLogs) ? 0.012f : 0.02f;
    g_st.scroll = g_api.make_scroll(g_api.op_new(0xb0), 0.0f, -top, kPanelW, h, 0, spacing);
    g_api.add_child(g_st.panel, g_st.ctx, g_st.scroll);
    const float rowW = kPanelW - 0.07f;

    if (g_st.tab == TabMode::ModsList) {
        for (int i = 0; i < (int)g_st.mods.size(); ++i) BuildModRow(g_st.scroll, i, rowW);
        if (g_st.mods.empty()) {
            NewText(g_st.scroll, 0.2f, -0.1f, kPanelW - 0.4f, 0.1f, 0,
                    "No mod detected. Put plugin subfolders (with mod.json + DLL) in the 'mods' "
                    "folder next to stormworks64.exe, then press Refresh.",
                    0, kColDim, 0.0007f, 0.8f);
        }
    } else if (g_st.tab == TabMode::SettingsDebug) {
        BuildSettingToggleRow(
            g_st.scroll, 0, rowW, "Open Log Terminal on Game Startup",
            "Opens a real-time colored Windows console at game launch (toggles live now)",
            "TERMINAL");
        BuildSettingToggleRow(
            g_st.scroll, 1, rowW, "Enable Top-Left Notifications Overlay",
            "Displays native toast notifications in the top-left corner for ModLoader events",
            "OVERLAY");
        BuildSettingToggleRow(
            g_st.scroll, 2, rowW, "Notifications Filter : Show ERROR Alerts",
            "Red alerts for DLL load failures, 32-bit mismatch, SwModInit crashes or dep cycles",
            "NOTIF");
        BuildSettingToggleRow(
            g_st.scroll, 3, rowW, "Notifications Filter : Show WARN Alerts",
            "Amber alerts for missing/disabled dependencies, auto-enabled deps or empty folders",
            "NOTIF");
        BuildSettingToggleRow(
            g_st.scroll, 4, rowW, "Notifications Filter : Show INFO Alerts",
            "Info alerts when mods are loaded, unloaded, hot-reloaded or refreshed",
            "NOTIF");
        BuildSettingToggleRow(
            g_st.scroll, 5, rowW, "Verbose Debug Logging",
            "Log RVA signatures, vtable hooks, AddDllDirectory paths, HMODULEs and SDK callbacks",
            "DEBUG");
        BuildSettingToggleRow(
            g_st.scroll, 6, rowW, "Dynamic Hot-Reload Mods in UI",
            "Immediately call LoadLibrary / FreeLibrary when toggling mods in the Mods List",
            "LOADER");
        BuildSettingToggleRow(
            g_st.scroll, 7, rowW, "Auto-Open C++ Mods Menu on Startup",
            "Automatically open this C++ Mods screen when entering the Stormworks main menu",
            "STARTUP");

        // Actions de debug interactives
        BuildDebugActionRow(
            g_st.scroll, rowW, "[Debug Action]  Send Test Notifications (INFO + WARN + ERROR)",
            "Triggers 3 sample notifications in the top-left corner to test the overlay",
            "[TEST NOTIFS]", kCmdTestNotifs);
        BuildDebugActionRow(
            g_st.scroll, rowW, "[Debug Action]  Toggle External Log Terminal Window Now",
            "Opens or closes the real-time Windows debug terminal window immediately",
            "[TERMINAL]", kCmdToggleTerminal);
        BuildDebugActionRow(
            g_st.scroll, rowW, "[Debug Action]  Force Hot-Reload All Enabled Mods",
            "Unloads and reloads all active mod DLLs from disk (useful after recompiling a mod)",
            "[HOT-RELOAD]", kCmdForceReload);
        BuildDebugActionRow(
            g_st.scroll, rowW, "[Debug Action]  Dump Engine & Loaded HMODULE Diagnostics",
            "Writes ASLR base, UI context, vtable slots and loaded mod HMODULEs to log & terminal",
            "[DUMP DIAG]", kCmdDumpDiag);
        BuildDebugActionRow(
            g_st.scroll, rowW, "[Debug Action]  Open StormworksModLoader.log in Text Editor",
            "Opens the log file next to stormworks64.exe in your default Windows editor",
            "[OPEN LOG]", kCmdOpenLogFile);
    } else if (g_st.tab == TabMode::LiveLogs) {
        BuildDebugActionRow(
            g_st.scroll, rowW, "[Log Action]  Clear Log History & Truncate Log File",
            "Clears the in-memory log viewer and resets StormworksModLoader.log",
            "[CLEAR LOGS]", kCmdClearLogs);
        auto logs = GetRecentLogs(75);
        for (const LogEntry& e : logs) {
            BuildLogEntryRow(g_st.scroll, e, rowW);
        }
    }

    UpdateSubtitle();
}

void BuildPanel(void* ui) {
    LogDebug("creation de la vue MODS (interface %p, ctx %p)", ui, g_st.ctx);
    void* ctx = g_st.ctx;
    void* panel = NewComposite(0.0f, 0.0f, kPanelW, kPanelH, /*anchor centre*/ 4);
    g_api.add_child(ui, ctx, panel);
    SetVisible(panel, false);
    g_st.panel = panel;

    // Fond plein écran (comme c_ui_element_view_container : rectangle 1000 x 1000 centré)
    NewRect(panel, 0, 0, 1000.0f, 1000.0f, 4, kColBackground);

    // Titre natif (texte + filet)
    sw::EStr title;
    sw::StrMake(g_api, title, "C++ Mods & Debug");
    void* heading = g_api.make_heading(g_api.op_new(0x80), 0.0f, 0.0f, kPanelW, 0, &title,
                                       FloatFromBits(0x3a1d4952), FloatFromBits(0x3dcccccd));
    g_api.add_child(panel, ctx, heading);
    g_st.heading = heading;
    g_st.headingH = Field<float>(heading, sw::off::kH);
    if (g_st.headingH < 0.05f || g_st.headingH > 0.5f) g_st.headingH = 0.1f;

    g_st.subtitle = NewText(panel, 0.0f, -(g_st.headingH + 0.015f), kPanelW, 0.05f, 0, "", 0, kColDim,
                            0.0005f, 0.8f);

    // Barre de 9 boutons ronds natifs en bas à gauche (ancre 6)
    constexpr float kStep = 0.19f, kY = 0.03f;
    g_st.buttons[0] = NewButton(panel, kStep * 0, kY, 6, "arrow_left", nullptr, 0x22b, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdBack));
    g_st.buttons[1] = NewButton(panel, kStep * 1, kY, 6, "cube_wrench", "Mods list", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdTabMods));
    g_st.buttons[2] = NewButton(panel, kStep * 2, kY, 6, "mechanics", "Settings & Debug", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdTabSettings));
    g_st.buttons[3] = NewButton(panel, kStep * 3, kY, 6, "bugs", "Live Logs", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdTabLogs));
    g_st.buttons[4] = NewButton(panel, kStep * 4, kY, 6, "action_tick", "Enable all", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdEnableAll));
    g_st.buttons[5] = NewButton(panel, kStep * 5, kY, 6, "action_cross", "Disable all", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdDisableAll));
    g_st.buttons[6] = NewButton(panel, kStep * 6, kY, 6, "redo", "Refresh", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdRefresh));
    g_st.buttons[7] = NewButton(panel, kStep * 7, kY, 6, "microprocessor_editor",
                                "Toggle Log Terminal", -1, 0, OnCommand,
                                reinterpret_cast<void*>(kCmdToggleTerminal));
    g_st.buttons[8] = NewButton(panel, kStep * 8, kY, 6, "load", "Open mods folder", -1, 0,
                                OnCommand, reinterpret_cast<void*>(kCmdOpenFolder));

    for (void* b : g_st.buttons) CallSetAlpha(b, ctx, 0xff);

    // Construit l'overlay de notifications en haut à gauche APRÈS le panneau pour qu'il soit
    // dessiné par-dessus toutes les vues (accueil ET vue C++ Mods).
    BuildNotifOverlay(ui);

    // Scan initial et chargement réel des DLLs des mods actifs dès l'arrivée au menu principal
    g_st.mods = ScanMods();
    SyncLoadedMods(g_st.mods, g_api.base, g_st.ctx, ui);
    LogInfo("demarrage : %zu mod(s) detecte(s), %d charge(s) en memoire", g_st.mods.size(),
            LoadedModsCount());
    BuildCurrentTabView();
}

void TriggerTestNotifications() {
    Notify(LogLevel::Info, "Info Notification Test",
           "ModLoader UI overlay is active (" + std::to_string(LoadedModsCount()) +
               " mod(s) currently loaded)");
    Notify(LogLevel::Warn, "Warning Notification Test",
           "Example warning: optional dependency 'extra_hud' not installed");
    Notify(LogLevel::Error, "Error Notification Test",
           "Example error: plugin 'broken_mod.dll' failed SwModInit()");
}

void DumpEngineDiagnostics() {
    LogInfo("=== Diagnostic Moteur UI & ModLoader ===");
    LogInfo("  stormworks64.exe base : %p", (void*)g_api.base);
    LogInfo("  default_view vtable   : %p (init=%p, set_alpha=%p)", (void*)g_api.default_view_vt,
            (void*)g_origViewInit, (void*)g_origViewAlpha);
    LogInfo("  interface vtable      : %p (update=%p)", (void*)g_api.interface_vt,
            (void*)g_origUiUpdate);
    LogInfo("  UI ctx=%p | interface=%p | default_view=%p | panel=%p", g_st.ctx, g_st.ui,
            g_st.view, g_st.panel);
    DumpLoadedModsDiagnostics();
    Notify(LogLevel::Info, "Diagnostics Dumped",
           "Engine addresses and loaded HMODULEs written to log & terminal");
}

void ProcessCommands() {
    bool modsDirty = false;

    if (g_st.pendingModToggle >= 0) {
        int i = g_st.pendingModToggle;
        g_st.pendingModToggle = -1;
        if (i < (int)g_st.mods.size() && g_st.mods[i].Toggleable()) {
            SetModEnabledWithDeps(g_st.mods, i, !g_st.mods[i].enabled);
            modsDirty = true;
        }
    }

    if (g_st.pendingSettingToggle >= 0) {
        int idx = g_st.pendingSettingToggle;
        g_st.pendingSettingToggle = -1;
        SetSettingValue(idx, !GetSettingValue(idx));
        for (auto& r : g_st.rows) {
            if (r->isSetting && r->index == idx) ApplySettingRowVisuals(*r);
        }
        UpdateSubtitle();
    }

    const Cmd cmd = g_st.pendingCmd;
    g_st.pendingCmd = kCmdNone;
    switch (cmd) {
        case kCmdBack:
            if (g_st.tab != TabMode::ModsList) {
                g_st.tab = TabMode::ModsList;
                BuildCurrentTabView();
            } else {
                g_api.set_view(g_st.ui, g_st.ctx, nullptr, 0, nullptr);
            }
            return;
        case kCmdTabMods:
            g_st.tab = TabMode::ModsList;
            BuildCurrentTabView();
            return;
        case kCmdTabSettings:
            g_st.tab = TabMode::SettingsDebug;
            BuildCurrentTabView();
            return;
        case kCmdTabLogs:
            g_st.tab = TabMode::LiveLogs;
            BuildCurrentTabView();
            return;
        case kCmdEnableAll:
        case kCmdDisableAll: {
            const bool want = (cmd == kCmdEnableAll);
            for (ModEntry& m : g_st.mods) {
                if (m.Toggleable()) m.enabled = want;
            }
            SaveAllEnabled(g_st.mods);
            g_st.tab = TabMode::ModsList;
            SyncLoadedMods(g_st.mods, g_api.base, g_st.ctx, g_st.ui);
            BuildCurrentTabView();
            return;
        }
        case kCmdRefresh:
            if (g_st.tab == TabMode::LiveLogs) {
                BuildCurrentTabView();
                return;
            }
            g_st.mods = ScanMods();
            SyncLoadedMods(g_st.mods, g_api.base, g_st.ctx, g_st.ui);
            Notify(LogLevel::Info, "Mods List Refreshed",
                   std::to_string(g_st.mods.size()) + " detected, " +
                       std::to_string(LoadedModsCount()) + " loaded in memory");
            BuildCurrentTabView();
            return;
        case kCmdOpenFolder:
            OpenModsFolder();
            break;
        case kCmdToggleTerminal: {
            bool nowOpen = !IsLogTerminalOpen();
            SetLogTerminalOpen(nowOpen);
            Notify(LogLevel::Info,
                   nowOpen ? "Log Terminal Opened" : "Log Terminal Closed",
                   nowOpen ? "Real-time debug console is now active"
                           : "External console window closed");
            UpdateSubtitle();
            break;
        }
        case kCmdTestNotifs:
            TriggerTestNotifications();
            if (g_st.tab == TabMode::LiveLogs) BuildCurrentTabView();
            break;
        case kCmdForceReload:
            g_st.mods = ScanMods();
            ForceReloadAllMods(g_st.mods, g_api.base, g_st.ctx, g_st.ui);
            if (g_st.tab == TabMode::ModsList || g_st.tab == TabMode::LiveLogs) {
                BuildCurrentTabView();
            } else {
                UpdateSubtitle();
            }
            return;
        case kCmdDumpDiag:
            DumpEngineDiagnostics();
            if (g_st.tab == TabMode::LiveLogs) BuildCurrentTabView();
            break;
        case kCmdOpenLogFile:
            OpenLogFile();
            break;
        case kCmdClearLogs:
            ClearLogs();
            if (g_st.tab == TabMode::LiveLogs) BuildCurrentTabView();
            break;
        default:
            break;
    }

    // Si le joueur a cliqué directement sur une case à cocher : l'état du widget fait foi.
    if (!modsDirty) {
        for (auto& r : g_st.rows) {
            if (!r->check) continue;
            const bool cur = Field<uint8_t>(r->check, sw::off::kCheckboxState) != 0;
            if (r->isSetting) {
                if (cur != GetSettingValue(r->index)) {
                    SetSettingValue(r->index, cur);
                    ApplySettingRowVisuals(*r);
                    UpdateSubtitle();
                    break;
                }
            } else if (r->index >= 0 && r->index < (int)g_st.mods.size()) {
                if (cur != g_st.mods[r->index].enabled) {
                    SetModEnabledWithDeps(g_st.mods, r->index, cur);
                    modsDirty = true;
                    break;
                }
            }
        }
    }

    if (modsDirty) {
        SyncAndRefreshVisuals();
    }
}

// --- Hooks de vtable ------------------------------------------------------------------------

void AddModsButton(void* view, void* ctx) {
    void* quit = Field<void*>(view, sw::off::kDvQuit);
    void* container = Field<void*>(quit, sw::off::kParent);
    void* first = Field<void*>(view, sw::off::kDvContinue);
    void* second = Field<void*>(view, sw::off::kDvNewGame);
    if (!quit || !container || !first || !second) {
        LogError("AddModsButton: structure inattendue (quit=%p container=%p)", quit, container);
        return;
    }
    g_st.compositeVt = *reinterpret_cast<void**>(container);
    const float quitX = Field<float>(quit, sw::off::kX);
    const float spacing = Field<float>(second, sw::off::kX) - Field<float>(first, sw::off::kX);

    sw::EStr icon, label;
    sw::StrMake(g_api, icon, "cube_wrench");
    sw::StrMake(g_api, label, "MODS");
    const float size = Field<float>(quit, sw::off::kW);
    void* btn = g_api.make_button(ctx, quitX, 0.0f, /*ancre haut-gauche*/ 0, &icon, &label,
                                  /*libellé dessous*/ 1, /*action*/ 0, 0.42f, size);
    g_api.add_child(container, ctx, btn);
    Field<void*>(btn, sw::off::kBtnCallback) = reinterpret_cast<void*>(&OnModsClicked);
    Field<void*>(btn, sw::off::kBtnUserData) = nullptr;
    Field<float>(quit, sw::off::kX) = quitX + spacing;
    Field<float>(container, sw::off::kW) += spacing;
    g_st.button = btn;
    LogDebug("bouton MODS ajoute (x=%.3f, pas=%.3f, conteneur=%p, bouton=%p)", quitX, spacing,
             container, btn);
}

void HkViewInit(void* view, void* ctx, void* a, void* b) {
    g_origViewInit(view, ctx, a, b);
    if (GetConfig().openTerminalOnStartup && !IsLogTerminalOpen()) {
        OpenLogTerminal();
    }
    g_st.ctx = ctx;
    g_st.view = view;
    g_st.button = nullptr;
    g_st.ui = nullptr;
    g_st.panel = nullptr;
    g_st.heading = nullptr;
    g_st.scroll = nullptr;
    g_st.subtitle = nullptr;
    g_st.notifContainer = nullptr;
    g_st.lastNotifRev = 0;
    g_st.rows.clear();
    g_st.openRequested = false;
    g_st.wasActive = false;
    g_st.pendingModToggle = -1;
    g_st.pendingSettingToggle = -1;
    g_st.pendingCmd = kCmdNone;
    if (Field<uint8_t>(view, sw::off::kDvLaunchMode) != 0) {
        LogDebug("vue d'accueil en mode compte a rebours : pas de bouton MODS");
        return;
    }
    AddModsButton(view, ctx);
}

void HkViewAlpha(void* view, void* ctx, uint8_t alpha) {
    g_origViewAlpha(view, ctx, alpha);
    if (view == g_st.view && g_st.button) CallSetAlpha(g_st.button, ctx, alpha);
}

void HkUiUpdate(void* ui, void* a, void* b, void* c) {
    g_origUiUpdate(ui, a, b, c);
    if (!g_st.view || !g_st.button || !g_st.ctx) return;
    if (g_st.ui != ui) {
        g_st.ui = ui;
        g_st.panel = nullptr;
        g_st.scroll = nullptr;
        g_st.notifContainer = nullptr;
        g_st.lastNotifRev = 0;
        g_st.rows.clear();
    }
    if (!g_st.panel) {
        BuildPanel(ui);
        if (GetConfig().autoOpenModsMenu) g_st.openRequested = true;
    }

    // Met à jour l'overlay de notifications en haut à gauche (visible sur tout le menu principal)
    UpdateNotifOverlay();
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const bool keyboardCaptured = Field<void*>(g_st.ctx, sw::off::kCtxFocused) != nullptr;
    PumpModRuntime(SW_SURFACE_MENU, foregroundPid == GetCurrentProcessId(), keyboardCaptured);
    UpdateModUi(SW_SURFACE_MENU, ui);

    // Tick à chaque frame des mods chargés qui exportent SwModTick
    TickLoadedMods(g_st.ctx);

    const bool busy = Field<int>(g_st.ctx, 0x5a4) == 2;
    if (g_st.openRequested) {
        g_st.openRequested = false;
        if (!busy) {
            g_st.mods = ScanMods();
            SyncLoadedMods(g_st.mods, g_api.base, g_st.ctx, ui);
            g_st.tab = TabMode::ModsList;
            LogDebug("ouverture vue MODS : %zu mod(s) detecte(s), %d charge(s)", g_st.mods.size(),
                     LoadedModsCount());
            BuildCurrentTabView();
            g_api.set_view(ui, g_st.ctx, nullptr, kModsView, nullptr);
        }
    }

    const bool active = Field<int>(ui, sw::off::kUiActiveView) == kModsView;
    SetVisible(g_st.panel, active);
    if (active) {
        if (!g_st.wasActive) {
            for (void* b2 : g_st.buttons) CallSetAlpha(b2, g_st.ctx, 0xff);
        }
        ProcessCommands();
    }
    g_st.wasActive = active;
}

bool PatchSlot(void** vt, size_t slot, void* hook, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = vt[slot];
    vt[slot] = hook;
    DWORD ignored = 0;
    VirtualProtect(&vt[slot], sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));
    return true;
}

}  // namespace

bool InstallModsUi() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    // Exact reviewed profiles have their own code/layout witnesses. Do not run
    // the older build's semantic contracts and mislabel this reviewed build UNKNOWN.
    // Unknown files still get the read-only scan, but never a hook installation.
    if (!IsSupportedEngineBuild(base)) {
        swml::discovery::DiscoverLoadedEngine(base);
        LogError("No reviewed binding/layout profile for this executable; no hooks installed");
        return false;
    }
    LogInfo("Discovery: reviewed build selected; authenticating file and complete dependency set");
    if (!AuthenticateEngineExecutable(base)) {
        LogError("No authenticated reviewed binding/layout profile for this executable; no hooks installed");
        return false;
    }
    if (!sw::ResolveApi(g_api)) {
        Notify(LogLevel::Error, "Incompatible Game Version",
               "Signature check failed: StormworksModLoader disabled for safety");
        return false;
    }

    InitializeEngineApi(g_api);
    const SlotPatch patches[] = {
        {&g_api.default_view_vt[sw::slot::kInit], g_api.base+EngineRva(g_api.base,0x6fbff0), reinterpret_cast<void*>(&HkViewInit), reinterpret_cast<void**>(&g_origViewInit)},
        {&g_api.default_view_vt[sw::slot::kSetAlpha], g_api.base+EngineRva(g_api.base,0x6fd420), reinterpret_cast<void*>(&HkViewAlpha), reinterpret_cast<void**>(&g_origViewAlpha)},
        {&g_api.interface_vt[sw::slot::kUpdate], g_api.base+EngineRva(g_api.base,0x702b80), reinterpret_cast<void*>(&HkUiUpdate), reinterpret_cast<void**>(&g_origUiUpdate)}
    };
    bool ok = PatchSlots(patches, _countof(patches));
    if (!ok) {
        LogError("ERREUR: VirtualProtect/patch de vtable impossible");
        return false;
    }
    if (!InstallIngameNotifications(g_api)) {
        LogError("Native in-game notifications unavailable; both notification interfaces left untouched");
    }
    LogDebug("hooks installes : default_view.init %p -> %p | default_view.set_alpha %p -> %p | "
             "interface.update %p -> %p",
             (void*)g_origViewInit, (void*)&HkViewInit, (void*)g_origViewAlpha,
             (void*)&HkViewAlpha, (void*)g_origUiUpdate, (void*)&HkUiUpdate);
    return true;
}

}  // namespace swml
