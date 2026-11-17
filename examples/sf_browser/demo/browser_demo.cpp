// browser_demo: uses the sf_browser "browser" service to show demo.html (from this mod's
// folder) in the middle of the window. F10 shows/hides it. The page talks to the mod:
//   "ping"        -> the mod answers "pong #n" through postMessage
//   "hide"        -> the mod hides the browser (F10 brings it back)
//   "open:<url>"  -> the mod loads that URL
#include "../../../include/sw_mod_sdk.h"
#include "../sf_browser_api.h"
#include <atomic>
#include <stdio.h>
#include <string>

static const SwModHost* g_host = nullptr;
static const SfBrowserService* g_browser = nullptr;
static SfBrowserHandle g_page = 0;
static std::atomic<bool> g_visible{true};
static std::atomic<int> g_pings{0};

static void Log(const std::string& s) { if (g_host) g_host->log(g_host->modId, s.c_str()); }

// Runs on sf_browser's IPC thread: short work only.
static void OnEvent(const SfBrowserEvent* e, void*) {
    switch (e->type) {
    case SF_BROWSER_EVENT_MESSAGE: {
        const std::string text = e->text;
        if (text == "ping") {
            const std::string reply = "pong #" + std::to_string(++g_pings);
            g_browser->postMessage(g_host, g_page, reply.c_str());
        } else if (text == "hide") {
            g_visible = false;
            g_browser->setVisible(g_host, g_page, false);
        } else if (text.rfind("open:", 0) == 0) {
            g_browser->loadUrl(g_host, g_page, text.c_str() + 5);
        }
        Log("page says: " + text);
        break;
    }
    case SF_BROWSER_EVENT_LOAD_END: Log("loaded " + std::string(e->text) + " (HTTP " + std::to_string(e->code) + ")"); break;
    case SF_BROWSER_EVENT_LOAD_ERROR: Log("load error " + std::to_string(e->code) + " for " + e->text); break;
    case SF_BROWSER_EVENT_CRASHED: Log("page renderer crashed"); break;
    case SF_BROWSER_EVENT_CLOSED: Log("browser process closed"); break;
    default: break;
    }
}

static void OnAction(const SwEvent* event, void*) {
    if (event->type != SW_EVENT_ACTION || event->value != 1 || !g_page) return;
    g_visible = !g_visible;
    g_browser->setVisible(g_host, g_page, g_visible);
    if (g_visible) g_browser->setFocus(g_host, g_page, true);
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->runtime) return false;
    const SwEngineApi* api = host->api;
    if (api->structSize < offsetof(SwEngineApi, services) + sizeof(api->services) || !api->services) return false;
    const void* table = nullptr;
    if (api->services->get(host, SF_BROWSER_SERVICE, SF_BROWSER_VERSION, &table, nullptr, nullptr) != SW_OK) {
        host->log(host->modId, "the browser service is unavailable (is sf_browser enabled?)");
        return false;
    }
    g_host = host;
    g_browser = static_cast<const SfBrowserService*>(table);
    char url[1024];
    if (g_browser->fileUrl(host, "demo.html", url, sizeof(url)) != SW_OK) return false;
    SfBrowserOptions o = {};
    o.structSize = sizeof(o);
    o.url = url;
    o.width = 900;
    o.height = 560;
    o.surface = SW_SURFACE_NONE;
    o.rect = {0, 0, 0, 0, 4}; // centred, one page pixel per screen pixel
    o.order = 100;
    o.interactive = true;
    o.transparent = true;     // the page draws its own rounded panel
    if (g_browser->create(host, &o, &OnEvent, nullptr, &g_page) != SW_OK) {
        host->log(host->modId, "cannot create the browser");
        return false;
    }
    if (api->input && (api->runtime->capabilities() & SW_CAP_KEYBOARD_ACTIONS)) {
        SwHandle action = 0, sub = 0;
        if (api->input->registerAction(host, "toggle_browser", &action) == SW_OK && api->input->bindKey(host, action, 0x79 /*VK_F10*/, 0) == SW_OK)
            api->runtime->subscribe(host, SW_EVENT_MASK(SW_EVENT_ACTION), &OnAction, nullptr, &sub);
    }
    host->log(host->modId, "browser demo opened (F10 toggles)");
    return true;
}

// sf_browser closes our browser itself when we unload (owner-release callback).
SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
