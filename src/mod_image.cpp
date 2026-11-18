// Native image overlay: mods write pixels from any thread; the loader uploads them and draws
// them over the frame from the game's GDI32!SwapBuffers import (found by name, no game RVA).
#include "mod_image.h"
#include "image_model.h"
#include "mod_runtime.h"
#include "log.h"
#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")

namespace swml {
namespace {
namespace model = image_model;

// ---- OpenGL enums missing from the 1.1 Windows header -------------------------------------
constexpr GLenum kBgra = 0x80E1, kClampToEdge = 0x812F, kTextureMaxLevel = 0x813D;
constexpr GLenum kTexture0 = 0x84C0, kActiveTexture = 0x84E0, kCurrentProgram = 0x8B8D;
constexpr GLenum kVertexArrayBinding = 0x85B5, kSamplerBinding = 0x8919;
constexpr GLenum kDrawFramebuffer = 0x8CA9, kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kPixelUnpackBuffer = 0x88EC, kPixelUnpackBufferBinding = 0x88EF;
constexpr GLenum kFramebufferSrgb = 0x8DB9;
constexpr GLenum kBlendDstRgb = 0x80C8, kBlendSrcRgb = 0x80C9, kBlendDstAlpha = 0x80CA, kBlendSrcAlpha = 0x80CB;
constexpr GLenum kBlendEquationRgb = 0x8009, kBlendEquationAlpha = 0x883D, kFuncAdd = 0x8006;
constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82;

// ---- Image store (any thread) -------------------------------------------------------------
struct Image {
    // Immutable after create.
    const SwModHost* owner = nullptr;
    SwHandle id = 0;
    SwUiSurface surface = SW_SURFACE_NONE;
    uint32_t width = 0, height = 0, flags = 0;
    SwImageFormat format = SW_IMAGE_BGRA8_PREMULTIPLIED;
    uint64_t serial = 0;
    // Guarded by `pixelsMutex`: the producer copies here, the render thread uploads from here.
    std::mutex pixelsMutex;
    std::vector<uint8_t> pixels;
    model::Dirty dirty;
    // Guarded by g_mutex.
    SwImageRect rect = {0, 0, 0, 0, 0};
    bool visible = true;
    float opacity = 1;
    int32_t order = 0;
    bool drawn = false;
    model::Box drawnBox;
    SwImageInputCallback input = nullptr;
    void* inputData = nullptr;
    // Render thread only.
    HGLRC textureContext = nullptr;
    GLuint texture = 0;
    GLuint flipped = 0;        // bottom row first (GL orientation), for native monitor draws
    bool flippedStale = true, flippedSrgb = false;
};
std::mutex g_mutex;
std::map<SwHandle, std::shared_ptr<Image>> g_images;
SwHandle g_next = 1;
uint64_t g_serial = 0;
std::map<const SwModHost*, uint64_t> g_ownerBytes;
// Destroyed images whose texture (render-thread state) still has to be freed.
std::vector<std::shared_ptr<Image>> g_retired;
std::atomic<uint64_t> g_pulse[3] = {};
std::atomic<uint32_t> g_screenWidth{0}, g_screenHeight{0};
std::atomic<size_t> g_liveCount{0}, g_retiredCount{0};

// ---- Input state ------------------------------------------------------------------------------
// Callbacks run while g_inputMutex is held, so releasing an owner (which takes it first)
// waits for any callback in flight. Lock order: g_inputMutex, then g_mutex.
std::recursive_mutex g_inputMutex;
std::atomic<size_t> g_interactiveCount{0};     // images with an input callback
std::atomic<HWND> g_window{nullptr};           // game window (from the presenting DC)
std::atomic<bool> g_subclassWanted{false}, g_subclassed{false};
WNDPROC g_originalProc = nullptr;
bool g_checkCursor = true;                     // tests turn this off
// Window thread only (guarded by g_inputMutex):
SwHandle g_hover = 0, g_capture = 0, g_focus = 0;
uint32_t g_buttons = 0;
uint32_t g_lastClickButton = 99; DWORD g_lastClickTime = 0; LPARAM g_lastClickPos = 0;
wchar_t g_highSurrogate = 0;
void InstallSubclass(HWND window);

// ---- SwapBuffers import --------------------------------------------------------------------
using SwapBuffersFn = BOOL (WINAPI*)(HDC);
SwapBuffersFn g_swapBuffers = nullptr;
std::mutex g_installMutex;
bool g_installTried = false, g_installed = false;
std::atomic<bool> g_disabled{false};

void** FindImportSlot(const char* dll, const char* function) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return nullptr;
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return nullptr;
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (!strcmp(reinterpret_cast<const char*>(byName->Name), function))
                    return reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
void* NativeSwapBuffers() {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    return gdi ? reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers")) : nullptr;
}

// ---- Renderer (render thread, current GL context only) -------------------------------------
struct Gl {
#define GL_PROC(name, ret, ...) using name##Fn = ret (APIENTRY*)(__VA_ARGS__); name##Fn name = nullptr;
    GL_PROC(CreateShader, GLuint, GLenum)
    GL_PROC(ShaderSource, void, GLuint, GLsizei, const char* const*, const GLint*)
    GL_PROC(CompileShader, void, GLuint)
    GL_PROC(GetShaderiv, void, GLuint, GLenum, GLint*)
    GL_PROC(GetShaderInfoLog, void, GLuint, GLsizei, GLsizei*, char*)
    GL_PROC(DeleteShader, void, GLuint)
    GL_PROC(CreateProgram, GLuint, void)
    GL_PROC(AttachShader, void, GLuint, GLuint)
    GL_PROC(LinkProgram, void, GLuint)
    GL_PROC(GetProgramiv, void, GLuint, GLenum, GLint*)
    GL_PROC(DeleteProgram, void, GLuint)
    GL_PROC(UseProgram, void, GLuint)
    GL_PROC(GetUniformLocation, GLint, GLuint, const char*)
    GL_PROC(Uniform1i, void, GLint, GLint)
    GL_PROC(Uniform1f, void, GLint, GLfloat)
    GL_PROC(Uniform2f, void, GLint, GLfloat, GLfloat)
    GL_PROC(Uniform4f, void, GLint, GLfloat, GLfloat, GLfloat, GLfloat)
    GL_PROC(ActiveTexture, void, GLenum)
    GL_PROC(GenVertexArrays, void, GLsizei, GLuint*)
    GL_PROC(BindVertexArray, void, GLuint)
    GL_PROC(BindFramebuffer, void, GLenum, GLuint)
    GL_PROC(BindBuffer, void, GLenum, GLuint)
    GL_PROC(BlendFuncSeparate, void, GLenum, GLenum, GLenum, GLenum)
    GL_PROC(BlendEquationSeparate, void, GLenum, GLenum)
    GL_PROC(BindSampler, void, GLuint, GLuint) // optional (GL 3.3)
    GL_PROC(GenFramebuffers, void, GLsizei, GLuint*)
    GL_PROC(FramebufferTexture2D, void, GLenum, GLenum, GLenum, GLuint, GLint)
    GL_PROC(BlitFramebuffer, void, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
    GL_PROC(CheckFramebufferStatus, GLenum, GLenum)
#undef GL_PROC
    bool tried = false, ready = false;
    GLuint program = 0, vao = 0;
    GLint uScreen = -1, uRect = -1, uOpacity = -1, uStraight = -1, uTexture = -1;
    GLuint blitFramebuffers[2] = {0, 0};
};
std::map<HGLRC, Gl> g_contexts; // render thread only

const char* kVertex = R"GLSL(#version 330 core
uniform vec2 u_screen;
uniform vec4 u_rect; // x, y, width, height in window pixels, +Y down
out vec2 v_uv;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    v_uv = corner;
    vec2 p = u_rect.xy + corner * u_rect.zw;
    gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0);
}
)GLSL";
const char* kFragment = R"GLSL(#version 330 core
uniform sampler2D u_texture;
uniform float u_opacity;
uniform int u_straight;
in vec2 v_uv;
out vec4 color_out;
void main() {
    vec4 c = texture(u_texture, v_uv);
    if (u_straight != 0) c.rgb *= c.a;
    color_out = c * u_opacity;
}
)GLSL";

template<class T> T Proc(const char* name) {
    auto p = wglGetProcAddress(name);
    if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) ||
        p == reinterpret_cast<PROC>(3) || p == reinterpret_cast<PROC>(-1)) return nullptr;
    return reinterpret_cast<T>(p);
}
GLuint Compile(Gl& g, GLenum type, const char* source) {
    GLuint s = g.CreateShader(type);
    if (!s) return 0;
    g.ShaderSource(s, 1, &source, nullptr);
    g.CompileShader(s);
    GLint ok = 0;
    g.GetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
        char log[512] = {};
        g.GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        LogError("Image: overlay shader compilation failed: %s", log);
        g.DeleteShader(s);
        return 0;
    }
    return s;
}
Gl& CurrentGl(HGLRC context) {
    Gl& g = g_contexts[context];
    if (g.tried) return g;
    g.tried = true;
#define LOAD(name) g.name = Proc<Gl::name##Fn>("gl" #name);
    LOAD(CreateShader) LOAD(ShaderSource) LOAD(CompileShader) LOAD(GetShaderiv) LOAD(GetShaderInfoLog)
    LOAD(DeleteShader) LOAD(CreateProgram) LOAD(AttachShader) LOAD(LinkProgram) LOAD(GetProgramiv)
    LOAD(DeleteProgram) LOAD(UseProgram) LOAD(GetUniformLocation) LOAD(Uniform1i) LOAD(Uniform1f)
    LOAD(Uniform2f) LOAD(Uniform4f) LOAD(ActiveTexture) LOAD(GenVertexArrays) LOAD(BindVertexArray)
    LOAD(BindFramebuffer) LOAD(BindBuffer) LOAD(BlendFuncSeparate) LOAD(BlendEquationSeparate) LOAD(BindSampler)
    LOAD(GenFramebuffers) LOAD(FramebufferTexture2D) LOAD(BlitFramebuffer) LOAD(CheckFramebufferStatus)
#undef LOAD
    const bool procs = g.CreateShader && g.ShaderSource && g.CompileShader && g.GetShaderiv && g.GetShaderInfoLog &&
        g.DeleteShader && g.CreateProgram && g.AttachShader && g.LinkProgram && g.GetProgramiv && g.DeleteProgram &&
        g.UseProgram && g.GetUniformLocation && g.Uniform1i && g.Uniform1f && g.Uniform2f && g.Uniform4f &&
        g.ActiveTexture && g.GenVertexArrays && g.BindVertexArray && g.BindFramebuffer && g.BindBuffer &&
        g.BlendFuncSeparate && g.BlendEquationSeparate;
    if (!procs) { LogError("Image: OpenGL 3.3 entry points missing; overlay disabled for this context"); return g; }
    // The game's shaders are GLSL 330 core: the same version is valid on its context.
    GLuint vs = Compile(g, kVertexShader, kVertex), fs = vs ? Compile(g, kFragmentShader, kFragment) : 0;
    if (!vs || !fs) { if (vs) g.DeleteShader(vs); return g; }
    GLuint program = g.CreateProgram();
    g.AttachShader(program, vs);
    g.AttachShader(program, fs);
    g.LinkProgram(program);
    g.DeleteShader(vs);
    g.DeleteShader(fs);
    GLint linked = 0;
    g.GetProgramiv(program, kLinkStatus, &linked);
    if (!linked) { LogError("Image: overlay program link failed"); g.DeleteProgram(program); return g; }
    g.program = program;
    g.uScreen = g.GetUniformLocation(program, "u_screen");
    g.uRect = g.GetUniformLocation(program, "u_rect");
    g.uOpacity = g.GetUniformLocation(program, "u_opacity");
    g.uStraight = g.GetUniformLocation(program, "u_straight");
    g.uTexture = g.GetUniformLocation(program, "u_texture");
    g.GenVertexArrays(1, &g.vao); // a core context draws nothing without a bound VAO
    g.ready = g.vao != 0;
    LogInfo("Image: overlay renderer ready on GL context %p (%s)", context,
            reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return g;
}

// Everything the overlay touches, restored exactly after drawing.
struct SavedState {
    GLint program, vao, arrayBuffer, activeTexture, texture0, sampler0, drawFramebuffer, unpackBuffer;
    GLint unpackAlignment, unpackRowLength, unpackSkipPixels, unpackSkipRows;
    GLint viewport[4], blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha, blendEqRgb, blendEqAlpha;
    GLboolean blend, depth, cull, scissor, stencil, srgb, colorMask[4];
};
void Save(const Gl& g, SavedState& s) {
    glGetIntegerv(kCurrentProgram, &s.program);
    glGetIntegerv(kVertexArrayBinding, &s.vao);
    glGetIntegerv(0x8894 /*GL_ARRAY_BUFFER_BINDING*/, &s.arrayBuffer);
    glGetIntegerv(kActiveTexture, &s.activeTexture);
    g.ActiveTexture(kTexture0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture0);
    s.sampler0 = 0;
    if (g.BindSampler) glGetIntegerv(kSamplerBinding, &s.sampler0);
    glGetIntegerv(kDrawFramebufferBinding, &s.drawFramebuffer);
    glGetIntegerv(kPixelUnpackBufferBinding, &s.unpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s.unpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &s.unpackSkipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &s.unpackSkipRows);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(kBlendSrcRgb, &s.blendSrcRgb);
    glGetIntegerv(kBlendDstRgb, &s.blendDstRgb);
    glGetIntegerv(kBlendSrcAlpha, &s.blendSrcAlpha);
    glGetIntegerv(kBlendDstAlpha, &s.blendDstAlpha);
    glGetIntegerv(kBlendEquationRgb, &s.blendEqRgb);
    glGetIntegerv(kBlendEquationAlpha, &s.blendEqAlpha);
    s.blend = glIsEnabled(GL_BLEND);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.srgb = glIsEnabled(kFramebufferSrgb);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
}
void Toggle(GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); }
void Restore(const Gl& g, const SavedState& s) {
    Toggle(GL_BLEND, s.blend); Toggle(GL_DEPTH_TEST, s.depth); Toggle(GL_CULL_FACE, s.cull);
    Toggle(GL_SCISSOR_TEST, s.scissor); Toggle(GL_STENCIL_TEST, s.stencil); Toggle(kFramebufferSrgb, s.srgb);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    g.BlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcAlpha, s.blendDstAlpha);
    g.BlendEquationSeparate(s.blendEqRgb, s.blendEqAlpha);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s.unpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, s.unpackSkipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, s.unpackSkipRows);
    g.BindBuffer(kPixelUnpackBuffer, s.unpackBuffer);
    g.BindFramebuffer(kDrawFramebuffer, s.drawFramebuffer);
    if (g.BindSampler) g.BindSampler(0, s.sampler0);
    glBindTexture(GL_TEXTURE_2D, s.texture0); // unit 0 is still active here
    g.ActiveTexture(s.activeTexture);
    g.BindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, s.arrayBuffer);
    g.BindVertexArray(s.vao);
    g.UseProgram(s.program);
}

struct DrawItem { std::shared_ptr<Image> image; model::Box box; float opacity; };

// Uploads the pending pixels of `image` into its texture (bound on the active unit afterwards).
// Returns true when the texture content changed.
bool Upload(Image& image, HGLRC context) {
    std::lock_guard<std::mutex> lock(image.pixelsMutex);
    const GLenum format = image.format == SW_IMAGE_RGBA8 ? GL_RGBA : kBgra;
    if (!image.texture || image.textureContext != context) {
        glGenTextures(1, &image.texture);
        image.textureContext = context;
        glBindTexture(GL_TEXTURE_2D, image.texture);
        const GLint filter = image.flags & SW_IMAGE_FLAG_NEAREST ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, format, GL_UNSIGNED_BYTE, image.pixels.data());
        image.dirty = {};
        image.flipped = 0; // belonged to the previous context, if any
        image.flippedStale = true;
        return true;
    }
    glBindTexture(GL_TEXTURE_2D, image.texture);
    if (image.dirty.Empty()) return false;
    const auto& d = image.dirty;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, image.width);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, d.x0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, d.y0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0, format, GL_UNSIGNED_BYTE, image.pixels.data());
    image.dirty = {};
    image.flippedStale = true;
    return true;
}

// Copies image.texture upside down into image.flipped (GPU blit). Texture binding of the
// active unit, framebuffer bindings, scissor and sRGB state are restored.
bool RefreshFlipped(Gl& g, Image& image, bool srgb) {
    if (!g.GenFramebuffers || !g.FramebufferTexture2D || !g.BlitFramebuffer) return false;
    constexpr GLenum kRead = 0x8CA8, kDraw = 0x8CA9, kReadBinding = 0x8CAA, kColor0 = 0x8CE0;
    if (!g.blitFramebuffers[0]) g.GenFramebuffers(2, g.blitFramebuffers);
    if (!g.blitFramebuffers[0] || !g.blitFramebuffers[1]) return false;
    GLint bound = 0, readFb = 0, drawFb = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (image.flipped && image.flippedSrgb != srgb) { glDeleteTextures(1, &image.flipped); image.flipped = 0; }
    if (!image.flipped) {
        glGenTextures(1, &image.flipped);
        glBindTexture(GL_TEXTURE_2D, image.flipped);
        const GLint filter = image.flags & SW_IMAGE_FLAG_NEAREST ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, 0);
        // Same bytes either way (blits copy raw values while FRAMEBUFFER_SRGB is off).
        glTexImage2D(GL_TEXTURE_2D, 0, srgb ? 0x8C43 /*GL_SRGB8_ALPHA8*/ : GL_RGBA8, image.width, image.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        image.flippedSrgb = srgb;
        image.flippedStale = true;
    }
    if (image.flippedStale) {
        glGetIntegerv(kReadBinding, &readFb);
        glGetIntegerv(kDrawFramebufferBinding, &drawFb);
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), srgbWrite = glIsEnabled(kFramebufferSrgb);
        GLboolean mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glDisable(GL_SCISSOR_TEST); glDisable(kFramebufferSrgb); // blits honour both
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        g.BindFramebuffer(kRead, g.blitFramebuffers[0]);
        g.FramebufferTexture2D(kRead, kColor0, GL_TEXTURE_2D, image.texture, 0);
        g.BindFramebuffer(kDraw, g.blitFramebuffers[1]);
        g.FramebufferTexture2D(kDraw, kColor0, GL_TEXTURE_2D, image.flipped, 0);
        const GLint w = GLint(image.width), h = GLint(image.height);
        static std::atomic<bool> checked{false};
        if (g.CheckFramebufferStatus && !checked.exchange(true)) {
            const GLenum readStatus = g.CheckFramebufferStatus(kRead), drawStatus = g.CheckFramebufferStatus(kDraw);
            LogInfo("Image: monitor flip framebuffers read 0x%X draw 0x%X (0x8CD5 = complete)", readStatus, drawStatus);
        }
        g.BlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g.BindFramebuffer(kRead, GLuint(readFb));
        g.BindFramebuffer(kDraw, GLuint(drawFb));
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (srgbWrite) glEnable(kFramebufferSrgb);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        image.flippedStale = false;
        // Diagnostic: one pixel from the middle of the flipped copy, once content exists.
        static std::atomic<int> samples{0};
        if (samples < 3) {
            uint8_t px[4] = {};
            GLint packAlign = 4, packBuffer = 0;
            glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
            glGetIntegerv(0x88ED /*GL_PIXEL_PACK_BUFFER_BINDING*/, &packBuffer);
            g.BindBuffer(0x88EB, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            g.BindFramebuffer(kRead, g.blitFramebuffers[1]);
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            g.BindFramebuffer(kRead, GLuint(readFb));
            glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
            g.BindBuffer(0x88EB, GLuint(packBuffer));
            if (px[3]) LogInfo("Image: monitor copy centre pixel #%d = %u,%u,%u,%u", ++samples, px[0], px[1], px[2], px[3]);
        }
    }
    glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    return true;
}

void DrawOverlayCpp(HDC dc) {
    HGLRC context = wglGetCurrentContext();
    if (!context || wglGetCurrentDC() != dc) return;
    RECT client = {};
    HWND window = WindowFromDC(dc);
    if (!window || !GetClientRect(window, &client)) return;
    const uint32_t sw = uint32_t(client.right - client.left), sh = uint32_t(client.bottom - client.top);
    if (!sw || !sh) return;
    g_screenWidth = sw;
    g_screenHeight = sh;
    g_window = window;
    InstallSubclass(window);

    std::vector<DrawItem> items;
    std::vector<GLuint> dead;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Only this thread touches textures. An image retired after the previous snapshot
        // is no longer drawn, so its texture can go now (if it belongs to this context).
        for (auto it = g_retired.begin(); it != g_retired.end();) {
            Image& image = **it;
            if (image.texture && image.textureContext != context) { ++it; continue; }
            if (image.texture) dead.push_back(image.texture);
            if (image.flipped) dead.push_back(image.flipped);
            image.texture = image.flipped = 0;
            it = g_retired.erase(it);
        }
        g_retiredCount = g_retired.size();
        uint64_t pulses[3] = {g_pulse[0].load(), g_pulse[1].load(), g_pulse[2].load()};
        const uint64_t now = GetTickCount64();
        for (auto& entry : g_images) {
            Image& image = *entry.second;
            image.drawn = false;
            if (!image.visible || image.opacity <= 0 || !model::SurfaceActive(image.surface, pulses, now)) continue;
            image.drawnBox = model::Place(image.rect, image.width, image.height, sw, sh);
            image.drawn = true;
            items.push_back({entry.second, image.drawnBox, image.opacity});
        }
    }
    if (!dead.empty()) glDeleteTextures(GLsizei(dead.size()), dead.data());
    if (items.empty()) return;
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        return a.image->order != b.image->order ? a.image->order < b.image->order : a.image->serial < b.image->serial;
    });
    Gl& g = CurrentGl(context);
    if (!g.ready) return;

    SavedState saved;
    Save(g, saved);
    g.BindFramebuffer(kDrawFramebuffer, 0);
    g.BindBuffer(kPixelUnpackBuffer, 0);
    if (g.BindSampler) g.BindSampler(0, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glViewport(0, 0, GLsizei(sw), GLsizei(sh));
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(kFramebufferSrgb);
    glEnable(GL_BLEND);
    g.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g.UseProgram(g.program);
    g.BindVertexArray(g.vao);
    g.Uniform2f(g.uScreen, float(sw), float(sh));
    g.Uniform1i(g.uTexture, 0);
    for (auto& item : items) {
        Image& image = *item.image;
        Upload(image, context);
        g.Uniform4f(g.uRect, item.box.x, item.box.y, item.box.width, item.box.height);
        g.Uniform1f(g.uOpacity, item.opacity);
        g.Uniform1i(g.uStraight, image.format == SW_IMAGE_RGBA8 ? 1 : 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    Restore(g, saved);
}
bool DrawOverlayGuarded(HDC dc) { try { DrawOverlayCpp(dc); return true; } catch (...) { return false; } }
bool DrawOverlaySeh(HDC dc) { __try { return DrawOverlayGuarded(dc); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

BOOL WINAPI HkSwapBuffers(HDC dc) {
    if ((g_liveCount.load(std::memory_order_relaxed) || g_retiredCount.load(std::memory_order_relaxed)) && !g_disabled.load(std::memory_order_relaxed) && !DrawOverlaySeh(dc)) {
        g_disabled = true; // GL state may be partially changed for this one frame; never retry
        LogError("Image: overlay drawing failed; image overlay disabled for this session");
    }
    return g_swapBuffers(dc);
}

bool Install() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_installTried) return g_installed;
    g_installTried = true;
    void** slot = FindImportSlot("GDI32.dll", "SwapBuffers");
    void* native = NativeSwapBuffers();
    if (!slot || !native) { LogError("Image: GDI32!SwapBuffers is not imported; overlay unavailable"); return false; }
    if (*slot != native) { LogError("Image: GDI32!SwapBuffers import already redirected; overlay unavailable"); return false; }
    g_swapBuffers = reinterpret_cast<SwapBuffersFn>(native);
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HkSwapBuffers));
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    g_installed = true;
    LogInfo("Image: overlay installed on the game's GDI32!SwapBuffers import");
    return true;
}

// ---- Input routing (window thread) ----------------------------------------------------------
struct Target { SwHandle id = 0; SwImageInputCallback fn = nullptr; void* data = nullptr; model::Box box; uint32_t w = 0, h = 0; };
bool TargetOf(SwHandle id, Target& t) { // under g_mutex
    auto it = g_images.find(id);
    if (it == g_images.end() || !it->second->input) return false;
    const Image& i = *it->second;
    t = {id, i.input, i.inputData, i.drawnBox, i.width, i.height};
    return true;
}
bool HitTest(int x, int y, Target& t) { // under g_mutex: topmost drawn interactive image
    const Image* best = nullptr;
    for (const auto& entry : g_images) {
        const Image& i = *entry.second;
        if (!i.input || !i.drawn) continue;
        const auto& b = i.drawnBox;
        if (x < b.x || y < b.y || x >= b.x + b.width || y >= b.y + b.height) continue;
        if (!best || i.order > best->order || (i.order == best->order && i.serial > best->serial)) best = &i;
    }
    return best && TargetOf(best->id, t);
}
uint32_t Modifiers() {
    return (GetKeyState(VK_CONTROL) < 0 ? SW_KEY_CTRL : 0) | (GetKeyState(VK_SHIFT) < 0 ? SW_KEY_SHIFT : 0) |
           (GetKeyState(VK_MENU) < 0 ? SW_KEY_ALT : 0);
}
bool CallbackCpp(SwImageInputCallback fn, const SwImageInput* e, void* data) { try { fn(e, data); return true; } catch (...) { return false; } }
bool CallbackSeh(SwImageInputCallback fn, const SwImageInput* e, void* data) {
    __try { return CallbackCpp(fn, e, data); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Deliver(const Target& t, SwImageInput e) {
    e.structSize = sizeof(SwImageInput);
    e.image = t.id;
    if (!CallbackSeh(t.fn, &e, t.data)) {
        LogError("Image: input callback of image %llu crashed; input disabled for it", static_cast<unsigned long long>(t.id));
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_images.find(t.id);
        if (it != g_images.end() && it->second->input == t.fn) { it->second->input = nullptr; --g_interactiveCount; }
    }
}
SwImageInput MouseEvent(SwImageInputType type, const Target& t, int cx, int cy) {
    SwImageInput e = {};
    e.type = type;
    e.x = t.box.width > 0 ? (cx - t.box.x) * t.w / t.box.width : 0;
    e.y = t.box.height > 0 ? (cy - t.box.y) * t.h / t.box.height : 0;
    e.buttons = g_buttons;
    e.modifiers = Modifiers();
    return e;
}
void FocusImage(SwHandle id) { // window thread, g_inputMutex held
    if (g_focus == id) return;
    Target old, now;
    bool hasOld, hasNow;
    { std::lock_guard<std::mutex> lock(g_mutex); hasOld = g_focus && TargetOf(g_focus, old); hasNow = id && TargetOf(id, now); }
    g_focus = hasNow ? id : 0;
    g_highSurrogate = 0;
    SwImageInput e = {};
    e.modifiers = Modifiers();
    if (hasOld) { e.type = SW_IMAGE_INPUT_BLUR; Deliver(old, e); }
    if (hasNow) { e.type = SW_IMAGE_INPUT_FOCUS; Deliver(now, e); }
}
bool CursorVisible() {
    if (!g_checkCursor) return true;
    CURSORINFO info = {sizeof(info)};
    return GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) && info.hCursor;
}
int ButtonOf(UINT msg) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return SW_IMAGE_BUTTON_LEFT;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return SW_IMAGE_BUTTON_RIGHT;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return SW_IMAGE_BUTTON_MIDDLE;
    default: return -1;
    }
}
// Returns true when the message is consumed (not passed to the game).
bool RouteMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) ScreenToClient(hwnd, &pt); // wheel uses screen coordinates
    const bool visible = CursorVisible();
    Target hit, captured, hovered;
    bool hasHit = false, hasCapture = false, hasHover = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hasHit = visible && HitTest(pt.x, pt.y, hit);
        hasCapture = g_capture && TargetOf(g_capture, captured);
        hasHover = g_hover && TargetOf(g_hover, hovered);
    }
    if (!hasCapture) g_capture = 0;
    const Target* target = hasCapture ? &captured : hasHit ? &hit : nullptr;
    // Leave events when the hovered image changes.
    if (hasHover && (!target || target->id != g_hover)) Deliver(hovered, MouseEvent(SW_IMAGE_INPUT_MOUSE_LEAVE, hovered, pt.x, pt.y));
    g_hover = target ? target->id : 0;
    const int button = ButtonOf(msg);
    const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
    if (!target) {
        if (down) FocusImage(0); // a click on the game takes the focus back
        if (button >= 0 && !down) g_buttons &= ~(1u << button);
        return false;
    }
    if (msg == WM_MOUSEMOVE) { Deliver(*target, MouseEvent(SW_IMAGE_INPUT_MOUSE_MOVE, *target, pt.x, pt.y)); return true; }
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        SwImageInput e = MouseEvent(SW_IMAGE_INPUT_MOUSE_WHEEL, *target, pt.x, pt.y);
        const float notches = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
        if (msg == WM_MOUSEWHEEL) e.wheelY = notches; else e.wheelX = notches;
        Deliver(*target, e);
        return true;
    }
    if (button < 0) return false;
    if (down) {
        const DWORD now = GetMessageTime();
        const int dx = static_cast<short>(LOWORD(lp)) - static_cast<short>(LOWORD(g_lastClickPos));
        const int dy = static_cast<short>(HIWORD(lp)) - static_cast<short>(HIWORD(g_lastClickPos));
        const bool twice = g_lastClickButton == uint32_t(button) && now - g_lastClickTime <= GetDoubleClickTime() &&
            std::abs(dx) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 && std::abs(dy) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        g_lastClickButton = twice ? 99 : uint32_t(button);
        g_lastClickTime = now;
        g_lastClickPos = lp;
        g_buttons |= 1u << button;
        g_capture = target->id;
        SetCapture(hwnd);
        FocusImage(target->id);
        SwImageInput e = MouseEvent(SW_IMAGE_INPUT_MOUSE_DOWN, *target, pt.x, pt.y);
        e.button = uint32_t(button);
        e.clickCount = twice ? 2 : 1;
        Deliver(*target, e);
        return true;
    }
    g_buttons &= ~(1u << button);
    SwImageInput e = MouseEvent(SW_IMAGE_INPUT_MOUSE_UP, *target, pt.x, pt.y);
    e.button = uint32_t(button);
    e.clickCount = g_lastClickButton == 99 ? 2 : 1;
    if (!g_buttons) { g_capture = 0; if (GetCapture() == hwnd) ReleaseCapture(); }
    Deliver(*target, e);
    return true;
}
bool RouteKeyboard(UINT msg, WPARAM wp, LPARAM lp) {
    Target focused;
    { std::lock_guard<std::mutex> lock(g_mutex); if (!g_focus || !TargetOf(g_focus, focused)) { g_focus = 0; return false; } }
    SwImageInput e = {};
    e.modifiers = Modifiers();
    e.nativeKeyData = uint32_t(lp);
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) return false; // Alt+F4 stays the game's
        e.type = SW_IMAGE_INPUT_KEY_DOWN;
        e.virtualKey = uint32_t(wp);
        e.repeat = (lp & (1 << 30)) != 0;
        e.systemKey = msg == WM_SYSKEYDOWN;
        Deliver(focused, e);
        return true;
    case WM_KEYUP: case WM_SYSKEYUP:
        e.type = SW_IMAGE_INPUT_KEY_UP;
        e.virtualKey = uint32_t(wp);
        e.systemKey = msg == WM_SYSKEYUP;
        Deliver(focused, e);
        return false; // the game also sees releases: no key can stay stuck there
    case WM_CHAR: {
        const wchar_t c = wchar_t(wp);
        if (c >= 0xD800 && c <= 0xDBFF) { g_highSurrogate = c; return true; }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!g_highSurrogate) return true;
            cp = 0x10000 + ((uint32_t(g_highSurrogate) - 0xD800) << 10) + (uint32_t(c) - 0xDC00);
        }
        g_highSurrogate = 0;
        e.type = SW_IMAGE_INPUT_CHAR;
        e.codepoint = cp;
        Deliver(focused, e);
        return true;
    }
    default: return false;
    }
}
bool RouteCpp(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    switch (msg) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return RouteMouse(hwnd, msg, wp, lp);
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR:
        return RouteKeyboard(msg, wp, lp);
    case WM_KILLFOCUS:
        g_buttons = 0; g_capture = 0;
        return false;
    default: return false;
    }
}
bool RouteGuarded(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool* consumed) {
    try { *consumed = RouteCpp(hwnd, msg, wp, lp); return true; } catch (...) { return false; }
}
bool RouteSeh(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool* consumed) {
    __try { return RouteGuarded(hwnd, msg, wp, lp, consumed); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
LRESULT CALLBACK HkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_interactiveCount.load(std::memory_order_relaxed) && !g_disabled.load(std::memory_order_relaxed)) {
        bool consumed = false;
        if (!RouteSeh(hwnd, msg, wp, lp, &consumed)) {
            g_disabled = true;
            LogError("Image: input routing failed; image overlay disabled for this session");
        } else if (consumed) {
            return 0;
        }
    }
    return CallWindowProcW(g_originalProc, hwnd, msg, wp, lp);
}
// Render thread, once the window is known and an image wants input.
void InstallSubclass(HWND window) {
    if (g_subclassed || !g_subclassWanted || !window) return;
    g_subclassed = true; // one attempt per session
    SetLastError(0);
    const LONG_PTR previous = GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (!previous) { LogError("Image: game window procedure not readable; image input unavailable"); return; }
    g_originalProc = reinterpret_cast<WNDPROC>(previous);
    if (!SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HkWndProc)) && GetLastError()) {
        LogError("Image: game window subclassing failed (%lu); image input unavailable", GetLastError());
        return;
    }
    LogInfo("Image: input routing installed on the game window %p", window);
}

// ---- SDK functions --------------------------------------------------------------------------
std::shared_ptr<Image> Find(const SwModHost* owner, SwHandle id) {
    auto it = g_images.find(id);
    return it != g_images.end() && it->second->owner == owner ? it->second : nullptr;
}
void DestroyLocked(std::map<SwHandle, std::shared_ptr<Image>>::iterator it) {
    Image& image = *it->second;
    g_ownerBytes[image.owner] -= model::Bytes(image.width, image.height);
    if (image.input) { image.input = nullptr; --g_interactiveCount; }
    // The render thread frees the texture (it may be drawing this image right now).
    if (g_retired.size() < 4096) g_retired.push_back(it->second);
    g_retiredCount = g_retired.size();
    g_images.erase(it);
    --g_liveCount;
}

SwResult Create(const SwModHost* owner, SwUiSurface surface, uint32_t width, uint32_t height,
                SwImageFormat format, uint32_t flags, SwHandle* out) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    if (!out || !model::ValidSurface(surface) || !model::ValidSize(width, height) ||
        !model::ValidFormat(format) || !model::ValidFlags(flags)) return SW_INVALID_ARGUMENT;
    if (g_disabled || !Install()) return SW_UNAVAILABLE;
    const uint64_t bytes = model::Bytes(width, height);
    auto image = std::make_shared<Image>();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        size_t count = 0;
        for (const auto& entry : g_images) if (entry.second->owner == owner) ++count;
        if (count >= SW_IMAGE_MAX_PER_OWNER || g_ownerBytes[owner] + bytes > SW_IMAGE_MAX_BYTES_PER_OWNER || !g_next)
            return SW_LIMIT_REACHED;
        g_ownerBytes[owner] += bytes; // reserved before the allocation below
    }
    try { image->pixels.assign(size_t(bytes), 0); }
    catch (...) { std::lock_guard<std::mutex> lock(g_mutex); g_ownerBytes[owner] -= bytes; return SW_LIMIT_REACHED; }
    image->owner = owner;
    image->surface = surface;
    image->width = width;
    image->height = height;
    image->format = format;
    image->flags = flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!RuntimeOwnerValid(owner)) { g_ownerBytes[owner] -= bytes; return SW_INVALID_OWNER; } // unloaded meanwhile
    image->id = g_next++;
    image->serial = ++g_serial;
    g_images[image->id] = image;
    ++g_liveCount;
    *out = image->id;
    return SW_OK;
}
bool CopyGuarded(Image& image, const SwImageRegion& r, const void* pixels, uint32_t stride) {
    __try { model::CopyRegion(image.pixels.data(), image.width, r, pixels, stride); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; } // unreadable source buffer
}
SwResult Update(const SwModHost* owner, SwHandle id, const SwImageRegion* region, const void* pixels, uint32_t stride) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    if (!pixels) return SW_INVALID_ARGUMENT;
    std::shared_ptr<Image> image;
    { std::lock_guard<std::mutex> lock(g_mutex); image = Find(owner, id); }
    if (!image) return SW_NOT_FOUND;
    SwImageRegion r;
    if (!model::ResolveRegion(region, image->width, image->height, stride, r)) return SW_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(image->pixelsMutex);
    if (!CopyGuarded(*image, r, pixels, stride)) return SW_INVALID_ARGUMENT;
    model::Merge(image->dirty, r);
    return SW_OK;
}
template<class Fn> SwResult Modify(const SwModHost* owner, SwHandle id, Fn fn) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto image = Find(owner, id);
    if (!image) return SW_NOT_FOUND;
    return fn(*image);
}
SwResult SetRect(const SwModHost* owner, SwHandle id, const SwImageRect* rect) {
    if (!model::ValidRect(rect)) return RuntimeOwnerValid(owner) ? SW_INVALID_ARGUMENT : SW_INVALID_OWNER;
    const SwImageRect copy = *rect;
    return Modify(owner, id, [&](Image& image) { image.rect = copy; return SW_OK; });
}
SwResult SetVisible(const SwModHost* owner, SwHandle id, bool visible) {
    return Modify(owner, id, [&](Image& image) { image.visible = visible; return SW_OK; });
}
SwResult SetOpacity(const SwModHost* owner, SwHandle id, float opacity) {
    const float value = model::ClampOpacity(opacity);
    if (value < 0) return RuntimeOwnerValid(owner) ? SW_INVALID_ARGUMENT : SW_INVALID_OWNER;
    return Modify(owner, id, [&](Image& image) { image.opacity = value; return SW_OK; });
}
SwResult SetOrder(const SwModHost* owner, SwHandle id, int32_t order) {
    return Modify(owner, id, [&](Image& image) { image.order = order; return SW_OK; });
}
SwResult Destroy(const SwModHost* owner, SwHandle id) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_images.find(id);
    if (it == g_images.end() || it->second->owner != owner) return SW_NOT_FOUND;
    DestroyLocked(it);
    return SW_OK;
}
SwResult ScreenSize(const SwModHost* owner, uint32_t* width, uint32_t* height) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    if (!width || !height) return SW_INVALID_ARGUMENT;
    const uint32_t w = g_screenWidth, h = g_screenHeight;
    if (!w || !h) return SW_UNAVAILABLE;
    *width = w; *height = h;
    return SW_OK;
}
SwResult DrawnRect(const SwModHost* owner, SwHandle id, float* x, float* y, float* width, float* height) {
    if (!x || !y || !width || !height) return RuntimeOwnerValid(owner) ? SW_INVALID_ARGUMENT : SW_INVALID_OWNER;
    return Modify(owner, id, [&](Image& image) {
        if (!image.drawn) return SW_UNAVAILABLE;
        *x = image.drawnBox.x; *y = image.drawnBox.y; *width = image.drawnBox.width; *height = image.drawnBox.height;
        return SW_OK;
    });
}

SwResult SetInputCallback(const SwModHost* owner, SwHandle id, SwImageInputCallback fn, void* data) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto image = Find(owner, id);
    if (!image) return SW_NOT_FOUND;
    if (!image->input && fn) ++g_interactiveCount;
    if (image->input && !fn) --g_interactiveCount;
    image->input = fn;
    image->inputData = data;
    if (fn) g_subclassWanted = true; // the render thread subclasses the game window next frame
    return SW_OK;
}
SwResult SetKeyboardFocus(const SwModHost* owner, SwHandle id, bool focus) {
    if (!RuntimeOwnerValid(owner)) return SW_INVALID_OWNER;
    std::lock_guard<std::recursive_mutex> input(g_inputMutex);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto image = Find(owner, id);
        if (!image) return SW_NOT_FOUND;
        if (!image->input) return SW_INVALID_ARGUMENT;
    }
    if (focus) FocusImage(id);
    else if (g_focus == id) FocusImage(0);
    return SW_OK;
}

const SwImageApi g_api = {sizeof(SwImageApi), &Create, &Update, &SetRect, &SetVisible, &SetOpacity,
    &SetOrder, &Destroy, &ScreenSize, &DrawnRect, &SetInputCallback, &SetKeyboardFocus};
}

const SwImageApi* ImagePrimitives() { return &g_api; }
bool ImageOverlayAvailable() {
    void** slot = FindImportSlot("GDI32.dll", "SwapBuffers");
    void* native = NativeSwapBuffers();
    return slot && native && (*slot == native || *slot == reinterpret_cast<void*>(&HkSwapBuffers));
}
void NoteImageSurfacePulse(SwUiSurface surface) {
    if (surface >= SW_SURFACE_MENU && surface <= SW_SURFACE_EDITOR) g_pulse[surface] = GetTickCount64();
}
void SetImageInputCursorCheck(bool enabled) { g_checkCursor = enabled; }
bool ImageTextureForDraw(const SwModHost* owner, uint64_t id, bool srgb, unsigned* texture) {
    if (!texture || g_disabled) return false;
    HGLRC context = wglGetCurrentContext();
    if (!context) return false;
    std::shared_ptr<Image> image;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        image = Find(owner, SwHandle(id));
    }
    if (!image) return false;
    Gl& g = CurrentGl(context);
    if (!g.ready) return false;
    // Upload binds the texture on the active unit and changes unpack state: put them back.
    GLint bound = 0, unpackBuffer = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(kPixelUnpackBufferBinding, &unpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    g.BindBuffer(kPixelUnpackBuffer, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    Upload(*image, context);
    // Native monitor UVs expect a render target (bottom row first): hand out a flipped copy.
    *texture = RefreshFlipped(g, *image, srgb) ? image->flipped : 0;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        LogInfo("Image: first monitor source %llu (%ux%u) -> texture %u", static_cast<unsigned long long>(id),
                image->width, image->height, *texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    g.BindBuffer(kPixelUnpackBuffer, GLuint(unpackBuffer));
    glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    return *texture != 0;
}
bool DrawImageOverlay(void* hdc) { return DrawOverlaySeh(static_cast<HDC>(hdc)); }
void ReleaseImageOwner(const SwModHost* owner) {
    // Waits for an input callback in flight: none of the owner's code runs after this returns.
    std::lock_guard<std::recursive_mutex> input(g_inputMutex);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_images.begin(); it != g_images.end();) {
        auto next = std::next(it);
        if (it->second->owner == owner) DestroyLocked(it);
        it = next;
    }
    g_ownerBytes.erase(owner);
}
}
