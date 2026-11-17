// Image overlay demo: a 320x180 animated picture in the top-right corner of the window,
// painted from the mod's OWN thread (like an off-screen browser would), shown everywhere.
// Interactive: the cursor shows a ring, a left click leaves a dot, the wheel changes the
// speed, typed keys tint the picture while it has the keyboard focus (Escape releases it).
// F9 toggles it. No game address.
#include "../../include/sw_mod_sdk.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

static const SwModHost* g_host = nullptr;
static const SwImageApi* g_image = nullptr;
static SwHandle g_handle = 0;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_visible{true};
static std::thread g_painter;

static const uint32_t kWidth = 320, kHeight = 180;

// Shared between the input callback (window thread) and the painter thread.
static std::mutex g_stateMutex;
struct Dot { float x, y; };
static std::vector<Dot> g_dots;
static float g_cursorX = -100, g_cursorY = -100, g_speed = 1, g_tint = 0;
static bool g_focused = false;

// BGRA, premultiplied: a moving colour gradient, dots, a cursor ring and a focus frame.
static void Paint(std::vector<uint8_t>& pixels, double t) {
    std::vector<Dot> dots;
    float cx, cy, tint;
    bool focused;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        dots = g_dots; cx = g_cursorX; cy = g_cursorY; tint = g_tint; focused = g_focused;
    }
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* p = &pixels[(size_t(y) * kWidth + x) * 4];
            const double u = double(x) / kWidth, v = double(y) / kHeight;
            double r = .5 + .5 * std::sin(6.28 * (u + t * .2) + tint);
            double g = .5 + .5 * std::sin(6.28 * (v + t * .3) + 2 + tint);
            double b = .5 + .5 * std::sin(6.28 * (u + v + t * .1) + 4);
            const bool border = x < 4 || y < 4 || x >= kWidth - 4 || y >= kHeight - 4;
            double a = border ? 1.0 : .8;
            if (border && focused) { r = 1; g = 1; b = 1; }
            for (const Dot& d : dots)
                if ((x - d.x) * (x - d.x) + (y - d.y) * (y - d.y) <= 25) { r = g = b = 1; a = 1; }
            const float ring = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (ring >= 7 && ring <= 9) { r = g = b = 0; a = 1; }
            p[0] = uint8_t(b * a * 255); p[1] = uint8_t(g * a * 255);
            p[2] = uint8_t(r * a * 255); p[3] = uint8_t(a * 255);
        }
    }
}

static void PainterThread() {
    std::vector<uint8_t> pixels(size_t(kWidth) * kHeight * 4);
    double t = 0;
    while (g_running) {
        { std::lock_guard<std::mutex> lock(g_stateMutex); t += .033 * g_speed; }
        Paint(pixels, t);
        // Any thread: the loader copies the pixels before returning.
        if (g_image->update(g_host, g_handle, nullptr, pixels.data(), kWidth * 4) != SW_OK) break;
        Sleep(33);
    }
}

// Window thread: keep it short, only update shared state.
static void OnInput(const SwImageInput* e, void*) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    switch (e->type) {
    case SW_IMAGE_INPUT_MOUSE_MOVE: g_cursorX = e->x; g_cursorY = e->y; break;
    case SW_IMAGE_INPUT_MOUSE_LEAVE: g_cursorX = g_cursorY = -100; break;
    case SW_IMAGE_INPUT_MOUSE_DOWN:
        if (e->button == SW_IMAGE_BUTTON_LEFT) {
            if (e->clickCount == 2) g_dots.clear();          // double click clears
            else if (g_dots.size() < 64) g_dots.push_back({e->x, e->y});
        }
        break;
    case SW_IMAGE_INPUT_MOUSE_WHEEL:
        g_speed = (std::max)(0.f, (std::min)(8.f, g_speed + e->wheelY * .5f));
        break;
    case SW_IMAGE_INPUT_CHAR: g_tint += .4f; break;
    case SW_IMAGE_INPUT_KEY_DOWN:
        if (e->virtualKey == VK_ESCAPE) g_image->setKeyboardFocus(g_host, g_handle, false);
        break;
    case SW_IMAGE_INPUT_FOCUS: g_focused = true; break;
    case SW_IMAGE_INPUT_BLUR: g_focused = false; break;
    default: break;
    }
}

static void OnAction(const SwEvent* event, void*) {
    if (event->type != SW_EVENT_ACTION || event->value != 1) return;
    g_visible = !g_visible;
    g_image->setVisible(g_host, g_handle, g_visible);
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION ||
        !host->api->runtime) return false;
    const SwEngineApi* api = host->api;
    if (api->structSize < offsetof(SwEngineApi, image) + sizeof(api->image) || !api->image ||
        api->image->structSize < offsetof(SwImageApi, getDrawnRect) + sizeof(api->image->getDrawnRect) ||
        !(api->runtime->capabilities() & SW_CAP_NATIVE_IMAGE)) {
        host->log(host->modId, "image overlay unavailable on this loader/game");
        return false;
    }
    g_host = host;
    g_image = api->image;
    if (g_image->create(host, SW_SURFACE_NONE, kWidth, kHeight, SW_IMAGE_BGRA8_PREMULTIPLIED, 0, &g_handle) != SW_OK) {
        host->log(host->modId, "image create failed");
        return false;
    }
    const SwImageRect rect = {-16, 16, 0, 0, 2}; // top-right, 16 px margin, 1:1 pixels
    g_image->setRect(host, g_handle, &rect);

    // Input is a newer member: without it the demo still runs, just not interactive.
    if (g_image->structSize >= offsetof(SwImageApi, setKeyboardFocus) + sizeof(g_image->setKeyboardFocus))
        g_image->setInputCallback(host, g_handle, &OnInput, nullptr);

    // Optional F9 toggle.
    if (api->input && (api->runtime->capabilities() & SW_CAP_KEYBOARD_ACTIONS)) {
        SwHandle action = 0, sub = 0;
        if (api->input->registerAction(host, "toggle_image", &action) == SW_OK &&
            api->input->bindKey(host, action, VK_F9, 0) == SW_OK)
            api->runtime->subscribe(host, SW_EVENT_MASK(SW_EVENT_ACTION), &OnAction, nullptr, &sub);
    }
    g_running = true;
    g_painter = std::thread(&PainterThread);
    host->log(host->modId, "image overlay demo running (click, wheel, type; F9 toggles)");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {
    // The loader has already revoked the input callback. Stop our thread BEFORE the DLL goes
    // away; the loader destroys the image itself.
    g_running = false;
    if (g_painter.joinable()) g_painter.join();
    g_host = nullptr;
}
