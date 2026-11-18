// Image overlay backend: SDK contract, owner scoping, and (when the machine has an OpenGL 3.3
// driver) real drawing into a window with GL state restoration checked.
#include "../src/mod_image.h"
#include "../src/mod_runtime.h"
#include <windows.h>
#include <GL/gl.h>
#include <cassert>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace swml {
void LogInfo(const char*, ...) {}
void LogWarn(const char*, ...) {}
void LogError(const char*, ...) {}
}

namespace {
void LogStub(const char*, const char*) {}

// Messages that reached the "game" (the test window's own procedure).
std::vector<UINT> g_passed;
LRESULT CALLBACK GameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)) g_passed.push_back(msg);
    return DefWindowProcW(hwnd, msg, wp, lp);
}
std::vector<SwImageInput> g_events;
void Record(const SwImageInput* e, void*) { g_events.push_back(*e); }
void Crash(const SwImageInput*, void*) { *static_cast<volatile int*>(nullptr) = 1; }
bool Passed(UINT msg) { for (UINT m : g_passed) if (m == msg) return true; return false; }

struct Window {
    HWND hwnd = nullptr; HDC dc = nullptr; HGLRC gl = nullptr;
    bool Open() {
        WNDCLASSW wc = {};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = GameProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"sf_image_test";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"image test", WS_POPUP | WS_VISIBLE,
                               0, 0, 64, 32, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;
        dc = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                     PFD_TYPE_RGBA, 32};
        pfd.cAlphaBits = 8;
        const int format = ChoosePixelFormat(dc, &pfd);
        if (!format || !SetPixelFormat(dc, format, &pfd)) return false;
        gl = wglCreateContext(dc);
        return gl && wglMakeCurrent(dc, gl);
    }
    ~Window() {
        if (gl) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(gl); }
        if (dc) ReleaseDC(hwnd, dc);
        if (hwnd) DestroyWindow(hwnd);
    }
};

bool Gl33() {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    int major = 0, minor = 0;
    return version && sscanf_s(version, "%d.%d", &major, &minor) == 2 && (major > 3 || (major == 3 && minor >= 3));
}
void Pixel(int x, int yFromTop, uint8_t out[4]) {
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, 32 - 1 - yFromTop, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}
bool Near(int a, int b) { return a - b <= 2 && b - a <= 2; }
}

int main() {
    const SwImageApi* api = swml::ImagePrimitives();
    assert(api && api->structSize == sizeof(SwImageApi));
    SwModHost owner = {}, other = {};
    owner.log = other.log = &LogStub;
    SwHandle image = 0;
    assert(api->create(&owner, SW_SURFACE_NONE, 4, 4, SW_IMAGE_RGBA8, 0, &image) == SW_INVALID_OWNER);
    swml::RegisterRuntimeOwner(&owner);
    swml::RegisterRuntimeOwner(&other);

    // Argument validation happens before installation.
    assert(api->create(&owner, SW_SURFACE_NONE, 0, 4, SW_IMAGE_RGBA8, 0, &image) == SW_INVALID_ARGUMENT);
    assert(api->create(&owner, SW_SURFACE_NONE, 4, 4, SwImageFormat(7), 0, &image) == SW_INVALID_ARGUMENT);
    assert(api->create(&owner, SwUiSurface(9), 4, 4, SW_IMAGE_RGBA8, 0, &image) == SW_INVALID_ARGUMENT);
    assert(api->create(&owner, SW_SURFACE_NONE, 4, 4, SW_IMAGE_RGBA8, 4, &image) == SW_INVALID_ARGUMENT);
    assert(api->create(&owner, SW_SURFACE_NONE, 4, 4, SW_IMAGE_RGBA8, 0, nullptr) == SW_INVALID_ARGUMENT);

    // This executable imports GDI32!SwapBuffers (see the call below): the overlay installs here.
    assert(swml::ImageOverlayAvailable());
    assert(api->create(&owner, SW_SURFACE_NONE, 2, 1, SW_IMAGE_RGBA8, SW_IMAGE_FLAG_NEAREST, &image) == SW_OK && image);
    assert(swml::ImageOverlayAvailable()); // still true once our own hook is in place

    // Owner scoping: another mod cannot see or touch the handle.
    const uint8_t red[] = {255, 0, 0, 255, 0, 0, 255, 255}; // red, blue (straight alpha)
    assert(api->update(&other, image, nullptr, red, 8) == SW_NOT_FOUND);
    assert(api->setVisible(&other, image, false) == SW_NOT_FOUND);
    assert(api->destroy(&other, image) == SW_NOT_FOUND);
    assert(api->update(&owner, image, nullptr, red, 7) == SW_INVALID_ARGUMENT);
    assert(api->update(&owner, image, nullptr, nullptr, 8) == SW_INVALID_ARGUMENT);
    assert(api->update(&owner, image, nullptr, red, 8) == SW_OK);
    SwImageRegion outside = {1, 0, 2, 1};
    assert(api->update(&owner, image, &outside, red, 8) == SW_INVALID_ARGUMENT);
    assert(api->update(&owner, image, nullptr, reinterpret_cast<const void*>(16), 8) == SW_INVALID_ARGUMENT); // unreadable
    SwImageRect rect = {4, 2, 8, 4, 0}; // 8x4 pixels at (4,2): left half red, right half blue
    assert(api->setRect(&owner, image, &rect) == SW_OK);
    rect.anchor = 12; assert(api->setRect(&owner, image, &rect) == SW_INVALID_ARGUMENT);
    assert(api->setOpacity(&owner, image, std::numeric_limits<float>::quiet_NaN()) == SW_INVALID_ARGUMENT);
    assert(api->setOrder(&owner, image, 3) == SW_OK);
    float x, y, w, h;
    assert(api->getDrawnRect(&owner, image, &x, &y, &w, &h) == SW_UNAVAILABLE);
    uint32_t sw = 0, sh = 0;
    assert(api->getScreenSize(&owner, &sw, &sh) == SW_UNAVAILABLE);

    // Per-owner count limit.
    std::vector<SwHandle> many;
    SwHandle extra = 0;
    while (api->create(&other, SW_SURFACE_HUD, 1, 1, SW_IMAGE_RGBA8, 0, &extra) == SW_OK) many.push_back(extra);
    assert(many.size() == SW_IMAGE_MAX_PER_OWNER);
    assert(api->create(&other, SW_SURFACE_HUD, 1, 1, SW_IMAGE_RGBA8, 0, &extra) == SW_LIMIT_REACHED);
    assert(api->destroy(&other, many.back()) == SW_OK);
    assert(api->destroy(&other, many.back()) == SW_NOT_FOUND);
    assert(api->create(&other, SW_SURFACE_HUD, 1, 1, SW_IMAGE_RGBA8, 0, &extra) == SW_OK);
    swml::ReleaseImageOwner(&other); // unload: every image of `other` is gone
    assert(api->setVisible(&other, extra, true) == SW_NOT_FOUND);
    assert(api->create(&other, SW_SURFACE_HUD, 1, 1, SW_IMAGE_RGBA8, 0, &extra) == SW_OK);

    Window window;
    if (!window.Open() || !Gl33()) {
        std::puts("image_backend_tests: OK ([SKIP] no OpenGL 3.3 driver: drawing not exercised)");
        SwapBuffers(window.dc); // keeps the GDI32!SwapBuffers import in this executable
        return 0;
    }
    // Hand-picked state that the overlay must restore.
    glViewport(1, 2, 3, 4);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    GLuint mine = 0;
    glGenTextures(1, &mine);
    glBindTexture(GL_TEXTURE_2D, mine);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);

    // Clear (outside the scissor would not clear: disable it for the clear only).
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    assert(swml::DrawImageOverlay(window.dc));

    uint8_t p[4];
    Pixel(5, 3, p);  assert(Near(p[0], 255) && Near(p[1], 0) && Near(p[2], 0));   // red half
    Pixel(10, 3, p); assert(Near(p[0], 0) && Near(p[1], 0) && Near(p[2], 255));   // blue half
    Pixel(20, 3, p); assert(Near(p[0], 0) && Near(p[2], 0));                      // outside
    Pixel(5, 10, p); assert(Near(p[0], 0));                                       // below

    GLint viewport[4], texture = 0, alignment = 0, rowLength = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    assert(viewport[0] == 1 && viewport[1] == 2 && viewport[2] == 3 && viewport[3] == 4);
    assert(!glIsEnabled(GL_BLEND) && glIsEnabled(GL_SCISSOR_TEST));
    assert(GLuint(texture) == mine && alignment == 1 && rowLength == 7);
    assert(glGetError() == GL_NO_ERROR);

    assert(api->getScreenSize(&owner, &sw, &sh) == SW_OK && sw == 64 && sh == 32);
    assert(api->getDrawnRect(&owner, image, &x, &y, &w, &h) == SW_OK && x == 4 && y == 2 && w == 8 && h == 4);

    // Partial update, half opacity, then hidden: checked on fresh frames.
    const uint8_t green[] = {0, 255, 0, 255};
    SwImageRegion left = {0, 0, 1, 1};
    assert(api->update(&owner, image, &left, green, 4) == SW_OK);
    assert(api->setOpacity(&owner, image, .5f) == SW_OK);
    glDisable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT);
    assert(swml::DrawImageOverlay(window.dc));
    Pixel(5, 3, p);  assert(Near(p[0], 0) && Near(p[1], 128) && Near(p[2], 0));
    Pixel(10, 3, p); assert(Near(p[2], 128));
    assert(api->setVisible(&owner, image, false) == SW_OK);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(swml::DrawImageOverlay(window.dc));
    Pixel(5, 3, p); assert(Near(p[1], 0));
    assert(api->getDrawnRect(&owner, image, &x, &y, &w, &h) == SW_UNAVAILABLE);

    // HUD images stay hidden until the HUD surface pulses.
    SwHandle hud = 0;
    const uint8_t white[] = {255, 255, 255, 255};
    assert(api->create(&owner, SW_SURFACE_HUD, 1, 1, SW_IMAGE_BGRA8_PREMULTIPLIED, 0, &hud) == SW_OK);
    assert(api->update(&owner, hud, nullptr, white, 4) == SW_OK);
    SwImageRect corner = {0, 0, 0, 0, 8}; // bottom-right pixel
    assert(api->setRect(&owner, hud, &corner) == SW_OK);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(swml::DrawImageOverlay(window.dc));
    Pixel(63, 31, p); assert(Near(p[0], 0));
    swml::NoteImageSurfacePulse(SW_SURFACE_HUD);
    assert(swml::DrawImageOverlay(window.dc));
    Pixel(63, 31, p); assert(Near(p[0], 255) && Near(p[1], 255) && Near(p[2], 255));

    // ---- Texture for a native monitor draw: uploaded, other bindings untouched ----
    {
        SwHandle source = 0;
        assert(api->create(&owner, SW_SURFACE_HUD, 2, 2, SW_IMAGE_RGBA8, 0, &source) == SW_OK);
        assert(api->setVisible(&owner, source, false) == SW_OK); // monitor only, never on screen
        const uint8_t quad[] = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
        assert(api->update(&owner, source, nullptr, quad, 8) == SW_OK);
        glBindTexture(GL_TEXTURE_2D, mine);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
        unsigned sourceTex = 0;
        assert(!swml::ImageTextureForDraw(&other, source, false, &sourceTex)); // another mod's image
        assert(swml::ImageTextureForDraw(&owner, source, false, &sourceTex) && sourceTex && sourceTex != mine);
        GLint bound = 0, row = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        assert(GLuint(bound) == mine && row == 7);
        uint8_t back[16] = {};
        glBindTexture(GL_TEXTURE_2D, sourceTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
        glBindTexture(GL_TEXTURE_2D, mine);
        // GL orientation for native monitor UVs: the image's bottom row comes first.
        assert(!memcmp(back, quad + 8, 8) && !memcmp(back + 8, quad, 8));
        const uint8_t redPixel[] = {255, 0, 0, 255};
        SwImageRegion lastPixel = {1, 1, 1, 1};
        assert(api->update(&owner, source, &lastPixel, redPixel, 4) == SW_OK);
        assert(swml::ImageTextureForDraw(&owner, source, false, &sourceTex)); // dirty region re-uploaded
        glBindTexture(GL_TEXTURE_2D, sourceTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
        glBindTexture(GL_TEXTURE_2D, mine);
        assert(back[4] == 255 && back[5] == 0 && back[0] == 70 && back[8] == 10); // image (1,1) = first row, x 1
        // sRGB copy for an sRGB native texture: same stored bytes, sRGB internal format.
        assert(swml::ImageTextureForDraw(&owner, source, true, &sourceTex));
        GLint internal = 0;
        glBindTexture(GL_TEXTURE_2D, sourceTex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1003, &internal);
        memset(back, 0, sizeof(back));
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
        glBindTexture(GL_TEXTURE_2D, mine);
        assert(internal == 0x8C43 && back[4] == 255 && back[0] == 70 && back[8] == 10);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        assert(api->destroy(&owner, source) == SW_OK);
        assert(!swml::ImageTextureForDraw(&owner, source, false, &sourceTex));
        assert(glGetError() == GL_NO_ERROR);
    }

    // ---- Input: mouse and keyboard routed to interactive images ----
    swml::SetImageInputCursorCheck(false); // the test may run with a hidden system cursor
    SwHandle box = 0, top = 0;
    assert(api->create(&owner, SW_SURFACE_NONE, 10, 10, SW_IMAGE_RGBA8, 0, &box) == SW_OK);
    SwImageRect at = {20, 10, 20, 10, 0}; // drawn 20x10 at (20,10): x scale 0.5, y scale 1
    assert(api->setRect(&owner, box, &at) == SW_OK);
    assert(api->setKeyboardFocus(&owner, box, true) == SW_INVALID_ARGUMENT); // not interactive yet
    assert(api->setInputCallback(&other, box, &Record, nullptr) == SW_NOT_FOUND);
    assert(api->setInputCallback(&owner, box, &Record, nullptr) == SW_OK);
    assert(swml::DrawImageOverlay(window.dc)); // installs the window subclass, then draws
    HWND hwnd = window.hwnd;
    auto send = [&](UINT msg, WPARAM wp, int x, int y) {
        g_events.clear(); g_passed.clear();
        SendMessageW(hwnd, msg, wp, MAKELPARAM(x, y));
    };
    send(WM_MOUSEMOVE, 0, 30, 12);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_MOUSE_MOVE && g_events[0].image == box);
    assert(g_events[0].x == 5 && g_events[0].y == 2 && g_events[0].structSize == sizeof(SwImageInput));
    assert(!Passed(WM_MOUSEMOVE));
    send(WM_LBUTTONDOWN, MK_LBUTTON, 30, 12);
    assert(g_events.size() == 2 && g_events[0].type == SW_IMAGE_INPUT_FOCUS && g_events[1].type == SW_IMAGE_INPUT_MOUSE_DOWN);
    assert(g_events[1].button == SW_IMAGE_BUTTON_LEFT && g_events[1].clickCount == 1 && g_events[1].buttons == 1);
    assert(!Passed(WM_LBUTTONDOWN));
    send(WM_MOUSEMOVE, MK_LBUTTON, 60, 30); // dragged outside: still captured by the image
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_MOUSE_MOVE && g_events[0].x == 20 && !Passed(WM_MOUSEMOVE));
    send(WM_LBUTTONUP, 0, 60, 30);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_MOUSE_UP && g_events[0].buttons == 0);
    send(WM_LBUTTONDOWN, MK_LBUTTON, 30, 12); // second click within the double-click time
    assert(g_events.back().type == SW_IMAGE_INPUT_MOUSE_DOWN && g_events.back().clickCount == 2);
    send(WM_LBUTTONUP, 0, 30, 12);
    send(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * 2), 0, 0); // wheel: screen coordinates
    POINT origin = {0, 0}; ClientToScreen(hwnd, &origin);
    send(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * 2), origin.x + 30, origin.y + 12);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_MOUSE_WHEEL && g_events[0].wheelY == 2);

    // Keyboard while focused: presses and characters are consumed, releases reach both.
    send(WM_KEYDOWN, 'A', 0, 0);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_KEY_DOWN && g_events[0].virtualKey == 'A' && !Passed(WM_KEYDOWN));
    send(WM_CHAR, 0xD83D, 0, 0); assert(g_events.empty() && !Passed(WM_CHAR)); // high surrogate held
    send(WM_CHAR, 0xDE00, 0, 0);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_CHAR && g_events[0].codepoint == 0x1F600);
    send(WM_KEYUP, 'A', 0, 0);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_KEY_UP && Passed(WM_KEYUP));
    g_events.clear(); g_passed.clear();
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_F4, 0);
    assert(g_events.empty() && Passed(WM_SYSKEYDOWN)); // Alt+F4 stays the game's

    // Leaving the image, then clicking the game: leave + blur, and the game gets the messages.
    send(WM_MOUSEMOVE, 0, 5, 5);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_MOUSE_LEAVE && Passed(WM_MOUSEMOVE));
    send(WM_LBUTTONDOWN, MK_LBUTTON, 5, 5);
    assert(g_events.size() == 1 && g_events[0].type == SW_IMAGE_INPUT_BLUR && Passed(WM_LBUTTONDOWN));
    send(WM_LBUTTONUP, 0, 5, 5);
    send(WM_KEYDOWN, 'B', 0, 0);
    assert(g_events.empty() && Passed(WM_KEYDOWN)); // no focus any more
    assert(api->setKeyboardFocus(&owner, box, true) == SW_OK);
    send(WM_KEYDOWN, 'C', 0, 0);
    assert(g_events.size() == 1 && g_events[0].virtualKey == 'C');
    assert(api->setKeyboardFocus(&owner, box, false) == SW_OK);

    // The topmost interactive image wins; a crashing callback only loses its own input.
    assert(api->create(&owner, SW_SURFACE_NONE, 4, 4, SW_IMAGE_RGBA8, 0, &top) == SW_OK);
    SwImageRect over = {28, 10, 0, 0, 0};
    assert(api->setRect(&owner, top, &over) == SW_OK && api->setOrder(&owner, top, 10) == SW_OK);
    assert(api->setInputCallback(&owner, top, &Crash, nullptr) == SW_OK);
    assert(swml::DrawImageOverlay(window.dc));
    send(WM_MOUSEMOVE, 0, 29, 11);
    assert(g_events.empty()); // went to `top`, which crashed and lost its callback
    send(WM_MOUSEMOVE, 0, 29, 11);
    assert(g_events.size() == 1 && g_events[0].image == box); // now reaches the image below
    assert(api->setInputCallback(&owner, box, nullptr, nullptr) == SW_OK);
    send(WM_MOUSEMOVE, 0, 29, 11);
    assert(g_events.empty() && Passed(WM_MOUSEMOVE)); // nothing interactive left

    // Destroyed and unloaded images free their textures on the next frame without GL errors.
    assert(api->destroy(&owner, hud) == SW_OK);
    swml::ReleaseImageOwner(&owner);
    assert(swml::DrawImageOverlay(window.dc));
    assert(glGetError() == GL_NO_ERROR);
    SwapBuffers(window.dc); // goes through the installed import hook
    std::puts("image_backend_tests: OK (drawing exercised)");
    return 0;
}
