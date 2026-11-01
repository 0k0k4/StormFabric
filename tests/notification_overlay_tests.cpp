#include "../src/notification_overlay.h"
#include "../src/log.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Node { bool visible = true; std::string text; };
std::vector<std::unique_ptr<Node>> nodes;
std::vector<swml::NotificationItem> active;
uint64_t revision = 0;
int textWrites = 0;
void* Make(void*, void*, const SwUiLayout*) {
    nodes.push_back(std::make_unique<Node>());
    return nodes.back().get();
}
void* MakeWithContext(void* ctx, void* parent, const SwUiLayout* l) {
    assert(ctx == reinterpret_cast<void*>(1)); // a game-state pointer must never get here
    return Make(ctx, parent, l);
}
void* Rect(void* ctx, void* parent, const SwUiLayout* l, uint32_t) { return MakeWithContext(ctx, parent, l); }
void* Text(void* ctx, void* parent, const SwUiLayout* l, const char*, int, uint32_t, float, float) {
    return MakeWithContext(ctx, parent, l);
}
void Visible(void* n, bool visible) { assert(n); static_cast<Node*>(n)->visible = visible; }
void SetText(void* ctx, void* n, const char* text) {
    assert(ctx == reinterpret_cast<void*>(1));
    assert(n); static_cast<Node*>(n)->text = text; ++textWrites;
}
}
namespace swml {
uint64_t GetActiveNotifications(std::vector<NotificationItem>& out, size_t max) {
    out.assign(active.begin(), active.begin() + (std::min)(max, active.size()));
    return revision;
}
std::string SafeText(std::string value) { return value; }
}
int main() {
    SwUiApi ui = {};
    ui.composite = &MakeWithContext;
    ui.rectangle = &Rect;
    ui.text = &Text;
    ui.setText = &SetText;
    ui.setVisible = &Visible;
    void* ctx = reinterpret_cast<void*>(1);
    void* parent = reinterpret_cast<void*>(2);
    swml::NotificationOverlay hud;
    hud.Update(ui, ctx, parent);
    assert(nodes.empty());
    active.push_back({1, swml::LogLevel::Error, "Lua test", "Clicked", 0});
    // Also covers a newly created interface with an unchanged queue revision of zero.
    hud.Update(ui, ctx, parent);
    assert(nodes.size() == 37 && textWrites == 2);
    assert(nodes[1]->visible && !nodes[10]->visible);
    assert(nodes[8]->text == "[ERROR]  Lua test" && nodes[9]->text == "Clicked");
    hud.Update(ui, ctx, parent);
    assert(nodes.size() == 37 && textWrites == 2);
    hud.Update(ui, reinterpret_cast<void*>(3), parent);
    assert(textWrites == 2); // no use of handles through a different context
    active.clear();
    ++revision;
    hud.Update(ui, ctx, parent);
    assert(!nodes[1]->visible);
    active.push_back({2, swml::LogLevel::Warn, "Warning", "New", 0});
    ++revision;
    hud.Update(ui, ctx, parent);
    assert(nodes[1]->visible && nodes[6]->visible && !nodes[8]->visible);
    // Forget metadata BEFORE the native tree is destroyed, then reuse the same parent.
    hud.Forget();
    nodes.clear();
    hud.Update(ui, ctx, parent);
    assert(nodes.size() == 37 && nodes[1]->visible && nodes[6]->text == "[WARN]  Warning");
    swml::NotificationOverlay editor;
    editor.Update(ui, ctx, reinterpret_cast<void*>(4));
    assert(nodes.size() == 74 && nodes[38]->visible);
    swml::NotificationInterfaces interfaces;
    size_t count = nodes.size();
    interfaces.Update(ui, parent); // unknown/update-before-init: no guessed UI context
    assert(nodes.size() == count);
    interfaces.Initialize(parent, ctx);
    interfaces.Update(ui, parent);
    assert(nodes.size() == count + 37);
    count = nodes.size();
    interfaces.Forget(parent); // slot 2 teardown, even if the interface is NOT freed
    interfaces.Update(ui, parent);
    assert(nodes.size() == count);
    interfaces.Initialize(parent, ctx); // same native address, entirely new children
    interfaces.Update(ui, parent);
    assert(nodes.size() == count + 37);
    interfaces.Forget(parent); // deleting destructor
    interfaces.Initialize(parent, nullptr);
    interfaces.Update(ui, parent);
    assert(nodes.size() == count + 37);
    puts("PASS: native widgets, expiry, context from init only, update-before-init, teardown, address reuse, independent editor");
}
