#include "notification_overlay.h"
#include "log.h"
#include "mods_scan.h"
#include <string>
#include <vector>

namespace swml {
namespace {
const uint32_t colors[] = {0xff5678c0, 0xff2cb8f5, 0xff4444f2};
std::string Limit(const std::string& text, size_t n) {
    return SafeText(text.size() <= n ? text : text.substr(0, n - 3) + "...");
}
}
void NotificationOverlay::Forget() {
    context_ = nullptr;
    container_ = nullptr;
    for (auto& slot : slots_) slot = {};
    revision_ = UINT64_MAX;
}
void NotificationOverlay::Update(const SwUiApi& ui, void* context, void* parent) {
    if (!context || !parent) return;
    // A live native element cannot migrate between contexts: refuse stale handles.
    if (container_ && context_ != context) return;
    std::vector<NotificationItem> active;
    const uint64_t revision = GetActiveNotifications(active, 4);
    if (!container_) {
        if (active.empty()) return; // no allocation while there is nothing to display
        context_ = context;
        SwUiLayout layout = {.025f, -.025f, .94f, .432f, 0};
        container_ = ui.composite(context, parent, &layout);
        for (size_t i = 0; i < 4; ++i) {
            Slot& slot = slots_[i];
            layout = {0, -static_cast<float>(i) * .108f, .94f, .096f, 0};
            slot.root = ui.composite(context, container_, &layout);
            layout = {0, 0, .94f, .096f, 0};
            ui.rectangle(context, slot.root, &layout, 0xf0181a1d);
            for (size_t c = 0; c < 3; ++c) {
                layout = {0, 0, .015f, .096f, 0};
                slot.bars[c] = ui.rectangle(context, slot.root, &layout, colors[c]);
                layout = {.030f, -.012f, .895f, .042f, 0};
                slot.headings[c] = ui.text(context, slot.root, &layout, "", 0, colors[c], .00050f, .8f);
            }
            layout = {.030f, -.052f, .895f, .038f, 0};
            slot.detail = ui.text(context, slot.root, &layout, "", 0, 0xffe6e6e6, .00044f, .8f);
            ui.setVisible(slot.root, false);
        }
        ui.setVisible(container_, true);
    }
    if (revision == revision_) return;
    revision_ = revision;
    for (size_t i = 0; i < 4; ++i) {
        Slot& slot = slots_[i];
        if (i >= active.size()) { ui.setVisible(slot.root, false); continue; }
        const auto& item = active[i];
        size_t severity = item.level == LogLevel::Error ? 2 : item.level == LogLevel::Warn ? 1 : 0;
        const char* prefix = severity == 2 ? "[ERROR]  " : severity == 1 ? "[WARN]  " : "[INFO]  ";
        for (size_t c = 0; c < 3; ++c) {
            ui.setVisible(slot.bars[c], c == severity);
            ui.setVisible(slot.headings[c], c == severity);
        }
        ui.setText(context, slot.headings[severity], Limit(prefix + item.title, 72).c_str());
        ui.setText(context, slot.detail, Limit(item.detail, 88).c_str());
        ui.setVisible(slot.root, true);
    }
}
}
