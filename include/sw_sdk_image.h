#pragma once
// Native image overlay (appended to SwEngineApi as `image`; check structSize).
//
// A mod creates an image, writes pixels into it, and the loader draws it over the game
// window at every frame, right before the game presents. Typical producers: a web view
// rendered off-screen (CEF/Ultralight paint callbacks), a video decoder, a chart.
//
//   * No game address is involved: the overlay is drawn from the game's own
//     GDI32!SwapBuffers import, found by name, with OpenGL state saved and restored.
//   * Every function may be called from ANY thread (an off-screen browser paints on its own
//     thread). Pixels are copied into loader memory: the mod's buffer is free on return.
//   * Images are drawn on top of the whole frame (3D view, native UI and cursor included),
//     sorted by `order` then creation. They are shown only while their surface is active.
//   * Handles are owner-scoped; unloading the mod destroys its images.
//   * Available when runtime->capabilities() has SW_CAP_NATIVE_IMAGE.
//   * SW_KEY_CTRL/SHIFT/ALT come from sw_sdk_p0.h.
#ifdef __cplusplus
extern "C" {
#endif

typedef enum SwImageFormat {
    // 4 bytes per pixel, B,G,R,A in memory, colour already multiplied by alpha
    // (what CEF OnPaint and most Windows APIs produce).
    SW_IMAGE_BGRA8_PREMULTIPLIED = 0,
    // 4 bytes per pixel, R,G,B,A in memory, straight (not premultiplied) alpha.
    SW_IMAGE_RGBA8 = 1
} SwImageFormat;

enum {
    SW_IMAGE_FLAG_NEAREST = 1 // nearest-neighbour sampling when scaled (default: linear)
};

// Limits enforced by the loader (SW_LIMIT_REACHED / SW_INVALID_ARGUMENT otherwise).
#define SW_IMAGE_MAX_DIMENSION 4096u
#define SW_IMAGE_MAX_PER_OWNER 64u
#define SW_IMAGE_MAX_BYTES_PER_OWNER (256ull * 1024 * 1024)

// Placement in PIXELS of the game window's client area, +Y pointing DOWN.
// anchor: 3x3 grid, row-major: 0 top-left, 1 top, 2 top-right, 3 left, 4 centre, 5 right,
// 6 bottom-left, 7 bottom, 8 bottom-right. The same point of the image is placed on that
// point of the window, then moved by (x, y). width/height <= 0 mean the image's own size
// (one texel per screen pixel, the right choice for text and web pages).
typedef struct SwImageRect {
    float x, y, width, height;
    int32_t anchor;
} SwImageRect;

// A sub-rectangle of the image, in image pixels (origin top-left).
typedef struct SwImageRegion {
    uint32_t x, y, width, height;
} SwImageRegion;

// ---- Input (mouse and keyboard on an image) ------------------------------------------------
// An image with an input callback is interactive. While the cursor is visible and over it
// (topmost interactive image wins), mouse messages go to the image instead of the game; a
// button pressed on the image keeps sending mouse events to it until every button is up.
// Clicking an interactive image gives it the keyboard focus: key presses and characters then
// go to it instead of the game (key releases reach both, so the game never keeps a key
// stuck); clicking elsewhere, setKeyboardFocus(false) or destroying the image ends it.
// Alt+F4 always reaches the game.
typedef enum SwImageInputType {
    SW_IMAGE_INPUT_MOUSE_MOVE = 1,
    SW_IMAGE_INPUT_MOUSE_DOWN = 2,
    SW_IMAGE_INPUT_MOUSE_UP = 3,
    SW_IMAGE_INPUT_MOUSE_WHEEL = 4,
    SW_IMAGE_INPUT_MOUSE_LEAVE = 5, // the cursor left the image (or got hidden)
    SW_IMAGE_INPUT_KEY_DOWN = 6,
    SW_IMAGE_INPUT_KEY_UP = 7,
    SW_IMAGE_INPUT_CHAR = 8,        // one Unicode code point (surrogate pairs combined)
    SW_IMAGE_INPUT_FOCUS = 9,       // keyboard focus gained
    SW_IMAGE_INPUT_BLUR = 10        // keyboard focus lost
} SwImageInputType;

enum { SW_IMAGE_BUTTON_LEFT = 0, SW_IMAGE_BUTTON_RIGHT = 1, SW_IMAGE_BUTTON_MIDDLE = 2 };

typedef struct SwImageInput {
    uint32_t structSize;
    SwImageInputType type;
    SwHandle image;
    float x, y;             // mouse events: position in IMAGE pixels (outside while captured)
    uint32_t button;        // MOUSE_DOWN/UP: SW_IMAGE_BUTTON_*
    uint32_t buttons;       // mouse events: pressed buttons, bit (1 << SW_IMAGE_BUTTON_*)
    uint32_t clickCount;    // MOUSE_DOWN/UP: 1, or 2 for a double click
    float wheelX, wheelY;   // MOUSE_WHEEL: notches (+Y = away from the user, +X = right)
    uint32_t virtualKey;    // KEY_*: Windows virtual-key code
    uint32_t nativeKeyData; // KEY_* / CHAR: the message's raw lParam (scan code, flags)
    uint32_t codepoint;     // CHAR: Unicode code point
    uint32_t modifiers;     // SW_KEY_CTRL | SW_KEY_SHIFT | SW_KEY_ALT
    bool repeat;            // KEY_DOWN: auto-repeat
    bool systemKey;         // KEY_*: WM_SYSKEY* (Alt held)
} SwImageInput;

// Runs on the game window's message thread (not necessarily the UI/game thread): keep it
// short, never block on other threads, no native UI calls unless runtime->isGameThread().
// Image functions may be called from it.
typedef void (*SwImageInputCallback)(const SwImageInput* input, void* userData);

typedef struct SwImageApi {
    uint32_t structSize;
    // surface: SW_SURFACE_MENU, SW_SURFACE_HUD or SW_SURFACE_EDITOR show the image only while
    // that interface is active; SW_SURFACE_NONE shows it everywhere. A new image is
    // transparent, visible, placed at {0, 0, 0, 0, anchor 0}, with opacity 1 and order 0.
    SwResult (*create)(const SwModHost*, SwUiSurface surface, uint32_t width, uint32_t height,
                       SwImageFormat format, uint32_t flags, SwHandle* out);
    // Copies `region` (NULL = whole image) from `pixels`, whose rows are `strideBytes`
    // apart (>= region width * 4). The new pixels appear on one of the next frames.
    SwResult (*update)(const SwModHost*, SwHandle, const SwImageRegion* region,
                       const void* pixels, uint32_t strideBytes);
    SwResult (*setRect)(const SwModHost*, SwHandle, const SwImageRect*);
    SwResult (*setVisible)(const SwModHost*, SwHandle, bool visible);
    SwResult (*setOpacity)(const SwModHost*, SwHandle, float opacity); // 0..1
    SwResult (*setOrder)(const SwModHost*, SwHandle, int32_t order);  // higher = on top
    SwResult (*destroy)(const SwModHost*, SwHandle);
    // Size of the game window's client area in pixels, as seen at the last presented frame.
    // SW_UNAVAILABLE until the overlay has presented a frame.
    SwResult (*getScreenSize)(const SwModHost*, uint32_t* width, uint32_t* height);
    // Where `handle` was drawn at the last frame, in window pixels (to map mouse
    // coordinates onto the image). SW_UNAVAILABLE if it has not been drawn yet.
    SwResult (*getDrawnRect)(const SwModHost*, SwHandle, float* x, float* y,
                             float* width, float* height);
    // Appended (check structSize). callback NULL makes the image non-interactive again.
    SwResult (*setInputCallback)(const SwModHost*, SwHandle, SwImageInputCallback callback, void* userData);
    // Gives (true) or removes (false) the keyboard focus. SW_INVALID_ARGUMENT when the image
    // has no input callback.
    SwResult (*setKeyboardFocus)(const SwModHost*, SwHandle, bool focus);
} SwImageApi;

#ifdef __cplusplus
}
#endif
