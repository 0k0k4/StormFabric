// End-to-end test of sf_browser WITHOUT the game: this program plays the loader (real image
// overlay, real service registry, an OpenGL window), loads sf_browser.dll and checks that a
// page renders, receives clicks and keys, and exchanges messages; then unloads cleanly.
//   e2e_test.exe <dir containing sf_browser.dll and cef\>
#include "../../../src/mod_image.h"
#include "../../../src/mod_runtime.h"
#include "../../../src/mod_services.h"
#include "../sf_browser_api.h"
#include <windows.h>
#include <GL/gl.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace swml {
void LogInfo(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
void LogWarn(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
void LogError(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
}

namespace {
const int kW = 800, kH = 500;
std::mutex g_eventsMutex;
std::vector<std::string> g_events;
void ModLog(const char* id, const char* text) { printf("[%s] %s\n", id ? id : "?", text); }
void OnEvent(const SfBrowserEvent* e, void*) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    g_events.push_back(std::to_string(int(e->type)) + ":" + e->text);
    printf("  event %d code %d: %s\n", int(e->type), e->code, e->text);
}
bool HasEvent(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    for (auto& e : g_events) if (e == s) return true;
    return false;
}
bool HasEventPrefix(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    for (auto& e : g_events) if (e.rfind(s, 0) == 0) return true;
    return false;
}

HWND g_hwnd; HDC g_dc;
void Frame() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    swml::DrawImageOverlay(g_dc);
    glFinish();
}
void Pixel(int x, int y, uint8_t out[4]) {
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, kH - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}
template<class Pred> bool WaitFor(Pred pred, int ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) { Frame(); if (pred()) return true; SwapBuffers(g_dc); Sleep(16); }
    return false;
}
bool Near(int a, int b) { return a - b <= 6 && b - a <= 6; }
bool ColorAt(int x, int y, int r, int g, int b) { uint8_t p[4]; Pixel(x, y, p); return Near(p[0], r) && Near(p[1], g) && Near(p[2], b); }
int CountHostProcesses() {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (!_wcsicmp(e.szExeFile, L"sf_browser_host.exe")) ++n;
    CloseHandle(snap);
    return n;
}
#define CHECK(c) do { if (!(c)) { printf("FAILED: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: e2e_test <sf_browser build dir>\n"); return 2; }
    const std::string dir = argv[1];

    // Window + GL context, like the game's.
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"sfb_e2e";
    RegisterClassW(&wc);
    RECT r = {0, 0, kW, kH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"sf_browser e2e", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    g_dc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    HGLRC gl = wglCreateContext(g_dc);
    wglMakeCurrent(g_dc, gl);
    swml::SetImageInputCursorCheck(false);

    // The "loader": engine API with the image overlay and the service registry.
    swml::SetRuntimeCapabilities(SW_CAP_NATIVE_IMAGE);
    SwEngineApi api = {};
    api.abiVersion = SW_ENGINE_API_VERSION;
    api.structSize = sizeof(api);
    api.runtime = swml::RuntimePrimitives();
    api.image = swml::ImagePrimitives();
    api.services = swml::ServicePrimitives();
    SwModHost browserHost = {}, testHost = {};
    browserHost.sdkVersion = testHost.sdkVersion = SW_MOD_SDK_VERSION;
    browserHost.modId = "sf_browser"; testHost.modId = "e2e";
    browserHost.modDir = dir.c_str(); testHost.modDir = dir.c_str();
    browserHost.log = testHost.log = &ModLog;
    browserHost.api = testHost.api = &api;
    swml::RegisterRuntimeOwner(&browserHost);
    swml::RegisterRuntimeOwner(&testHost);
    swml::SetServiceOwnerIdentity(&browserHost, "sf_browser", {});
    swml::SetServiceOwnerIdentity(&testHost, "e2e", {"sf_browser"});

    const std::string dll = dir + "\\sf_browser.dll";
    HMODULE module = LoadLibraryA(dll.c_str());
    CHECK(module);
    auto init = reinterpret_cast<SwModInitFn>(GetProcAddress(module, "SwModInit"));
    auto unload = reinterpret_cast<SwModUnloadFn>(GetProcAddress(module, "SwModUnload"));
    CHECK(init && unload && init(&browserHost));

    const void* table = nullptr;
    CHECK(api.services->get(&testHost, SF_BROWSER_SERVICE, SF_BROWSER_VERSION, &table, nullptr, nullptr) == SW_OK);
    auto* browser = static_cast<const SfBrowserService*>(table);

    // A page: red; click anywhere -> "clicked"; typing -> "typed:<value>"; message "green" -> green.
    const char* page =
        "data:text/html,<html><body style='margin:0;background:rgb(200,30,30)'>"
        "<input id=i autofocus style='position:absolute;left:10px;top:10px;width:300px'>"
        "<script>"
        "document.body.addEventListener('mousedown',function(e){if(e.target.id!=='i')sfBrowser.send('clicked:'+e.clientX+','+e.clientY)});"
        "document.getElementById('i').addEventListener('input',function(e){sfBrowser.send('typed:'+e.target.value)});"
        "function hook(){if(!window.sfBrowser)return setTimeout(hook,20);"
        "sfBrowser.onmessage=function(t){if(t==='green')document.body.style.background='rgb(30,200,30)';};sfBrowser.send('ready')}hook();"
        "</script></body></html>";
    SfBrowserOptions o = {};
    o.structSize = sizeof(o);
    o.url = page;
    o.width = 400; o.height = 300;
    o.surface = SW_SURFACE_NONE;
    o.rect = {0, 0, 0, 0, 4}; // centred: page (0,0) at window (200,100)
    o.interactive = true;
    SfBrowserHandle h = 0;
    CHECK(browser->create(&testHost, &o, &OnEvent, nullptr, &h) == SW_OK && h);

    CHECK(WaitFor([] { return HasEvent("1:ready"); }, 30000));          // page loaded, bridge works
    CHECK(WaitFor([] { return ColorAt(400, 350, 200, 30, 30); }, 5000)); // red pixels on screen
    CHECK(ColorAt(100, 50, 0, 0, 0));                                     // outside the page: untouched
    printf("PASS: page rendered through shared memory and the overlay\n");

    // Mouse: a click at window (450,300) is page (250,200).
    SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(450, 300));
    SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(450, 300));
    SendMessageW(g_hwnd, WM_LBUTTONUP, 0, MAKELPARAM(450, 300));
    CHECK(WaitFor([] { return HasEvent("1:clicked:250,200"); }, 5000));
    printf("PASS: click forwarded with page coordinates\n");

    // Keyboard: click the input (page 20,20 -> window 220,120), type "ab".
    SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(230, 122));
    SendMessageW(g_hwnd, WM_LBUTTONUP, 0, MAKELPARAM(230, 122));
    WaitFor([] { return false; }, 300);
    for (wchar_t c : {L'a', L'b'}) {
        SendMessageW(g_hwnd, WM_KEYDOWN, WPARAM(towupper(c)), 1);
        SendMessageW(g_hwnd, WM_CHAR, WPARAM(c), 1);
        SendMessageW(g_hwnd, WM_KEYUP, WPARAM(towupper(c)), 0xC0000001);
    }
    CHECK(WaitFor([] { return HasEvent("1:typed:ab"); }, 5000));
    printf("PASS: keyboard focus and characters reach the page\n");

    // Mod -> page message, visible in the pixels.
    CHECK(browser->postMessage(&testHost, h, "green") == SW_OK);
    CHECK(WaitFor([] { return ColorAt(400, 350, 30, 200, 30); }, 5000));
    printf("PASS: postMessage reaches window.sfBrowser.onmessage\n");

    // Resize: the image follows (new shared memory generation).
    CHECK(browser->resize(&testHost, h, 600, 400) == SW_OK);
    // Page now starts at (100,50): page (5,100) is window (105,150), below the input; page
    // (590,390) is window (690,440), only inside the new, larger size.
    if (!WaitFor([] { return ColorAt(105, 150, 30, 200, 30) && ColorAt(690, 440, 30, 200, 30); }, 5000)) {
        for (POINT pt : {POINT{105, 150}, POINT{690, 440}, POINT{400, 250}}) {
            uint8_t p[4]; Pixel(pt.x, pt.y, p);
            printf("  pixel (%ld,%ld) = %u %u %u\n", pt.x, pt.y, p[0], p[1], p[2]);
        }
        CHECK(false);
    }
    printf("PASS: resize\n");

    CHECK(browser->setVisible(&testHost, h, false) == SW_OK);
    CHECK(WaitFor([] { return ColorAt(400, 350, 0, 0, 0); }, 2000));
    CHECK(api.services->get(&testHost, "browser", SW_SERVICE_VERSION(2, 0), &table, nullptr, nullptr) == SW_UNAVAILABLE);

    // Consumer unload (loader order): sf_browser closes the consumer's browsers.
    swml::ReleaseImageOwner(&testHost);
    swml::ReleaseServiceOwner(&testHost);
    CHECK(browser->loadUrl(&testHost, h, "about:blank") == SW_NOT_FOUND);
    printf("PASS: consumer release closes its browsers\n");

    // Provider unload: host process ends, DLL frees.
    CHECK(CountHostProcesses() >= 1);
    swml::ReleaseImageOwner(&browserHost);
    swml::ReleaseServiceOwner(&browserHost);
    unload();
    FreeLibrary(module);
    CHECK(WaitFor([] { return CountHostProcesses() == 0; }, 5000));
    printf("PASS: unload stops the Chromium processes\n");
    SwapBuffers(g_dc);
    printf("e2e_test: OK\n");
    return 0;
}
