#pragma once
// sf_browser service: a Chromium (CEF) browser drawn over the game, for other mods.
//
//   1. List "sf_browser" in your mod.json "dependencies".
//   2. services->get(host, SF_BROWSER_SERVICE, SF_BROWSER_VERSION, &table, ...).
//   3. create() a browser with a URL, a size and a placement; it is drawn with the loader's
//      image overlay and, when interactive, receives the mouse and the keyboard.
//
// Chromium runs in its own process (sf_browser_host.exe): no CEF DLL is loaded into the game,
// and a browser crash cannot take the game down.
//
// Page <-> mod messages (strings, e.g. JSON):
//   page -> mod : window.sfBrowser.send(text)      => SF_BROWSER_EVENT_MESSAGE
//   mod  -> page: postMessage(text)                => window.sfBrowser.onmessage(text)
//
// Every function may be called from any thread. Event callbacks run on sf_browser's own IPC
// thread: keep them short and never wait there for another thread. Browsers belong to the
// mod that created them and are closed automatically when that mod unloads.
// Include sw_mod_sdk.h before this header.
#include <stdint.h>

#define SF_BROWSER_SERVICE "browser"
#define SF_BROWSER_VERSION SW_SERVICE_VERSION(1, 0)

typedef uint64_t SfBrowserHandle;

typedef struct SfBrowserOptions {
    uint32_t structSize;
    const char* url;        // UTF-8: http(s)://, file:///, data:... (see fileUrl)
    uint32_t width, height; // page size in pixels, 1..4096
    SwUiSurface surface;    // where it is shown (SW_SURFACE_NONE: everywhere)
    SwImageRect rect;       // placement in window pixels (sw_sdk_image.h)
    int32_t order;          // stacking order among images
    bool interactive;       // mouse and keyboard go to the page
    bool transparent;       // transparent page background (otherwise white)
} SfBrowserOptions;

typedef enum SfBrowserEventType {
    SF_BROWSER_EVENT_MESSAGE = 1,    // text: what the page passed to window.sfBrowser.send
    SF_BROWSER_EVENT_LOAD_START = 2, // text: URL
    SF_BROWSER_EVENT_LOAD_END = 3,   // text: URL, code: HTTP status
    SF_BROWSER_EVENT_LOAD_ERROR = 4, // text: URL, code: CEF error code (negative)
    SF_BROWSER_EVENT_TITLE = 5,      // text: page title
    SF_BROWSER_EVENT_URL = 6,        // text: new address
    SF_BROWSER_EVENT_CRASHED = 7,    // the page's renderer died; loadUrl/navigate RELOAD recovers
    SF_BROWSER_EVENT_CLOSED = 8      // the browser process went away (all browsers are dead)
} SfBrowserEventType;

typedef struct SfBrowserEvent {
    uint32_t structSize;
    SfBrowserEventType type;
    SfBrowserHandle browser;
    const char* text; // UTF-8, valid during the callback only (never NULL)
    int32_t code;
} SfBrowserEvent;

typedef void (*SfBrowserEventCallback)(const SfBrowserEvent* event, void* userData);

enum { SF_BROWSER_BACK = 0, SF_BROWSER_FORWARD = 1, SF_BROWSER_RELOAD = 2, SF_BROWSER_STOP = 3 };

typedef struct SfBrowserService {
    uint32_t structSize;
    // callback may be NULL. The page starts loading at once; it appears when first painted.
    SwResult (*create)(const SwModHost* caller, const SfBrowserOptions* options,
                       SfBrowserEventCallback callback, void* userData, SfBrowserHandle* out);
    SwResult (*destroy)(const SwModHost* caller, SfBrowserHandle browser);
    SwResult (*loadUrl)(const SwModHost* caller, SfBrowserHandle browser, const char* url);
    SwResult (*executeJavaScript)(const SwModHost* caller, SfBrowserHandle browser, const char* code);
    SwResult (*postMessage)(const SwModHost* caller, SfBrowserHandle browser, const char* text);
    SwResult (*navigate)(const SwModHost* caller, SfBrowserHandle browser, int32_t action);
    SwResult (*resize)(const SwModHost* caller, SfBrowserHandle browser, uint32_t width, uint32_t height);
    SwResult (*setRect)(const SwModHost* caller, SfBrowserHandle browser, const SwImageRect* rect);
    SwResult (*setVisible)(const SwModHost* caller, SfBrowserHandle browser, bool visible);
    SwResult (*setOrder)(const SwModHost* caller, SfBrowserHandle browser, int32_t order);
    SwResult (*setOpacity)(const SwModHost* caller, SfBrowserHandle browser, float opacity);
    // Keyboard focus (needs `interactive`). Clicking the page also gives it the focus.
    SwResult (*setFocus)(const SwModHost* caller, SfBrowserHandle browser, bool focus);
    // file:/// URL of a file inside the CALLER's mod folder (relative path, no "..").
    SwResult (*fileUrl)(const SwModHost* caller, const char* relativePath, char* out, size_t capacity);
} SfBrowserService;
