#include "native_video.h"
#include "native_lua.h"
#include "video_profile.h"
#include "engine_bindings.h"
#include "video_model.h"
#include "video_shaders.h"
#include "log.h"
#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace swml {
namespace {
using namespace video_profile;
// Native argument 8 is an integer flag (MOV [RSP+38h],R11D / CMP dword),
// while arguments 6 and 7 are floats, in both reviewed Windows x64 builds.
using DrawMonitorFn = void (*)(void*, void*, void*, void*, int*, float, float, int, void*, int*, void*);
using DrawGpuFn = void (*)(void*, void*, void*, void*, void*, float, float, uint32_t);
using DestroyFn = void* (*)(void*, uint32_t);
using DrawElementsFn = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*);
DrawMonitorFn g_drawMonitor = nullptr;
DrawGpuFn g_drawGpu = nullptr;
DestroyFn g_destroy = nullptr;
DrawElementsFn g_drawElements = nullptr;
bool g_installed = false;
std::mutex g_mutex;
struct Lease { std::atomic<bool> active{true}; };
std::map<const SwModHost*, std::shared_ptr<Lease>> g_owners;
struct Slot { GLuint pbo = 0; void* fence = nullptr; size_t bytes = 0; uint32_t sequence = 0; };
struct Record {
    std::atomic<bool> active{true};
    void* overlay = nullptr;
    int width = 0, height = 0;
    VideoEffects effects;
    std::shared_ptr<Lease> effectOwner, captureOwner;
    std::shared_ptr<const VideoCapture> latest;
    // GPU fields below are used ONLY on the native render thread/context.
    HGLRC context = nullptr;
    GLuint source = 0;
    int sourceWidth = 0, sourceHeight = 0;
    std::array<Slot, 2> slots;
};
std::map<void*, std::shared_ptr<Record>> g_monitors;
std::map<void*, std::weak_ptr<Record>> g_overlays;
std::vector<std::shared_ptr<Record>> g_retired;
constexpr size_t kMaxMonitors = 64, kMaxCaptureBytes = 4 * 1024 * 1024, kGpuBudget = 32 * 1024 * 1024;
std::atomic<uint32_t> g_sequence{0};
struct DrawContext {
    std::shared_ptr<Record> record;
    std::shared_ptr<const VideoCapture> latest;
    VideoEffects effects;
    std::shared_ptr<Lease> effectOwner, captureOwner;
};
thread_local DrawContext* g_draw = nullptr;
struct GpuContext {
    VideoEffects effects;
    int width = 0, height = 0;
    std::shared_ptr<Lease> owner;
};
thread_local GpuContext* g_gpu = nullptr;

// Extension entrypoints are resolved by public OpenGL names, not game RVAs.
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
    GL_PROC(BindAttribLocation, void, GLuint, GLuint, const char*)
    GL_PROC(LinkProgram, void, GLuint)
    GL_PROC(GetProgramiv, void, GLuint, GLenum, GLint*)
    GL_PROC(GetProgramInfoLog, void, GLuint, GLsizei, GLsizei*, char*)
    GL_PROC(DeleteProgram, void, GLuint)
    GL_PROC(UseProgram, void, GLuint)
    GL_PROC(GetAttribLocation, GLint, GLuint, const char*)
    GL_PROC(GetUniformLocation, GLint, GLuint, const char*)
    GL_PROC(GetUniformfv, void, GLuint, GLint, GLfloat*)
    GL_PROC(GetUniformiv, void, GLuint, GLint, GLint*)
    GL_PROC(Uniform1i, void, GLint, GLint)
    GL_PROC(Uniform1f, void, GLint, GLfloat)
    GL_PROC(Uniform2f, void, GLint, GLfloat, GLfloat)
    GL_PROC(Uniform4f, void, GLint, GLfloat, GLfloat, GLfloat, GLfloat)
    GL_PROC(Uniform4fv, void, GLint, GLsizei, const GLfloat*)
    GL_PROC(UniformMatrix4fv, void, GLint, GLsizei, GLboolean, const GLfloat*)
    GL_PROC(ActiveTexture, void, GLenum)
    GL_PROC(GenBuffers, void, GLsizei, GLuint*)
    GL_PROC(DeleteBuffers, void, GLsizei, const GLuint*)
    GL_PROC(BindBuffer, void, GLenum, GLuint)
    GL_PROC(BufferData, void, GLenum, intptr_t, const void*, GLenum)
    GL_PROC(MapBufferRange, void*, GLenum, intptr_t, intptr_t, GLbitfield)
    GL_PROC(UnmapBuffer, GLboolean, GLenum)
    GL_PROC(FenceSync, void*, GLenum, GLbitfield)
    GL_PROC(ClientWaitSync, GLenum, void*, GLbitfield, uint64_t)
    GL_PROC(DeleteSync, void, void*)
#undef GL_PROC
    bool tried = false, effects = false, capture = false, readbackFailed = false;
    size_t allocated = 0;
    struct Program { GLuint id = 0; bool tried = false; std::array<GLint, 7> original{}, custom{}; };
    std::map<GLuint, Program> programs;
};
std::map<HGLRC, Gl> g_contexts; // render-thread owned; context resources live until process exit
template<class T> T Proc(const char* name) {
    auto p = wglGetProcAddress(name);
    if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) ||
        p == reinterpret_cast<PROC>(3) || p == reinterpret_cast<PROC>(-1)) return nullptr;
    return reinterpret_cast<T>(p);
}
Gl& CurrentGl() {
    Gl& g = g_contexts[wglGetCurrentContext()];
    if (g.tried) return g;
    g.tried = true;
#define LOAD(name) g.name = Proc<Gl::name##Fn>("gl" #name);
    LOAD(CreateShader) LOAD(ShaderSource) LOAD(CompileShader) LOAD(GetShaderiv) LOAD(GetShaderInfoLog)
    LOAD(DeleteShader) LOAD(CreateProgram) LOAD(AttachShader) LOAD(BindAttribLocation) LOAD(LinkProgram)
    LOAD(GetProgramiv) LOAD(GetProgramInfoLog) LOAD(DeleteProgram) LOAD(UseProgram) LOAD(GetAttribLocation)
    LOAD(GetUniformLocation) LOAD(GetUniformfv) LOAD(GetUniformiv) LOAD(Uniform1i) LOAD(Uniform1f)
    LOAD(Uniform2f) LOAD(Uniform4f) LOAD(Uniform4fv) LOAD(UniformMatrix4fv) LOAD(ActiveTexture)
    LOAD(GenBuffers) LOAD(DeleteBuffers) LOAD(BindBuffer) LOAD(BufferData) LOAD(MapBufferRange)
    LOAD(UnmapBuffer) LOAD(FenceSync) LOAD(ClientWaitSync) LOAD(DeleteSync)
#undef LOAD
    g.effects = g.CreateShader && g.ShaderSource && g.CompileShader && g.GetShaderiv && g.GetShaderInfoLog &&
        g.DeleteShader && g.CreateProgram && g.AttachShader && g.BindAttribLocation && g.LinkProgram &&
        g.GetProgramiv && g.GetProgramInfoLog && g.DeleteProgram && g.UseProgram && g.GetAttribLocation &&
        g.GetUniformLocation && g.GetUniformfv && g.GetUniformiv && g.Uniform1i && g.Uniform1f &&
        g.Uniform2f && g.Uniform4f && g.Uniform4fv && g.UniformMatrix4fv;
    g.capture = g.ActiveTexture && g.GenBuffers && g.DeleteBuffers && g.BindBuffer && g.BufferData &&
        g.MapBufferRange && g.UnmapBuffer && g.FenceSync && g.ClientWaitSync && g.DeleteSync;
    LogInfo("Video: OpenGL native context capabilities FX=%d async capture=%d", g.effects, g.capture);
    return g;
}
bool LeaseActive(const std::shared_ptr<Lease>& p) { return p && p->active.load(); }

// Keep fault containment away from objects needing C++ unwinding.
void* ResourceObject(void* storage) {
    __try {
        // onDraw receives &monitor.overlayHandle, NOT the deferred resource wrapper itself.
        auto resource = storage ? *static_cast<void**>(storage) : nullptr;
        return resource ? *reinterpret_cast<void**>(static_cast<uint8_t*>(resource) + kResourceObject) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
GLuint TextureId(void* target) {
    __try { return target ? *reinterpret_cast<GLuint*>(static_cast<uint8_t*>(target) + kTargetTexture) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool InDraw(void* state) {
    __try {
        if (!state || !g_draw) return false;
        auto self = *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(state) - 8);
        return self && *reinterpret_cast<void**>(self + 8) == state &&
            *reinterpret_cast<uint8_t*>(self + 0x304) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::shared_ptr<Lease> Owner(const SwModHost* host, void* state) {
    if (!InDraw(state)) return {};
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_owners.find(host);
    return it == g_owners.end() ? nullptr : it->second;
}
bool Enable(const SwModHost* host) {
    if (!host || !g_installed) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_owners.count(host)) g_owners[host] = std::make_shared<Lease>();
    return true;
}
bool Offset(const SwModHost* h, void* L, double x, double y) {
    auto o = Owner(h, L); if (!o || !g_draw->effects.offset(x,y)) return false;
    g_draw->effectOwner = o; return true;
}
bool Scale(const SwModHost* h, void* L, double x, double y) {
    auto o = Owner(h, L); if (!o || !g_draw->effects.scale(x,y)) return false;
    g_draw->effectOwner = o; return true;
}
bool Tint(const SwModHost* h, void* L, double r, double g, double b, double a) {
    auto o = Owner(h, L); if (!o || !g_draw->effects.tint(r,g,b,a)) return false;
    g_draw->effectOwner = o; return true;
}
bool Filter(const SwModHost* h, void* L, double gray, double invert) {
    auto o = Owner(h, L); if (!o || !g_draw->effects.filter(gray,invert)) return false;
    g_draw->effectOwner = o; return true;
}
bool Reset(const SwModHost* h, void* L) {
    auto o = Owner(h, L); if (!o) return false;
    g_draw->effects = {}; g_draw->effectOwner = o; return true;
}
bool Request(const SwModHost* h, void* L, bool enable) {
    auto o = Owner(h, L); if (!o) return false;
    g_draw->captureOwner = enable ? o : nullptr; return true;
}
bool Pixel(const SwModHost* h, void* L, int x, int y, SwVideoPixel* out) {
    auto o = Owner(h, L); if (!o) return false;
    if (!out || x < 0 || y < 0 || x >= g_draw->record->width || y >= g_draw->record->height) return false;
    g_draw->captureOwner = o; // opt-in, one whole source capture, never one GPU read per pixel
    return g_draw->latest && g_draw->latest->pixel(x,y,out);
}

void Retire(const std::shared_ptr<Record>& r) {
    r->active = false;
    if (r->overlay) g_overlays.erase(r->overlay);
    g_retired.push_back(r);
}
void* MonitorDestroy(void* self, uint32_t flags) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_monitors.find(self);
        if (it != g_monitors.end()) { Retire(it->second); g_monitors.erase(it); }
    }
    return g_destroy(self, flags);
}
void DrawMonitor(void* a, void* b, void* c, void* target, int* size,
                 float d, float e, int f, void* signal, int* error, void* text) {
    DrawContext frame;
    auto monitor = static_cast<uint8_t*>(signal) - kMonitorSignal;
    void* overlay = ResourceObject(target);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_owners.empty() && overlay && size && size[0] > 0 && size[1] > 0) {
            auto it = g_monitors.find(monitor);
            if (it == g_monitors.end() && g_monitors.size() < kMaxMonitors)
                it = g_monitors.emplace(monitor, std::make_shared<Record>()).first;
            if (it != g_monitors.end()) {
                auto r = it->second;
                if (r->overlay != overlay || r->width != size[0] || r->height != size[1]) {
                    Retire(r);
                    r = it->second = std::make_shared<Record>();
                }
                r->overlay = overlay; r->width = size[0]; r->height = size[1];
                g_overlays[overlay] = r;
                frame.record = r; frame.latest = r->latest;
            }
        }
    }
    auto previous = g_draw;
    g_draw = frame.record ? &frame : nullptr;
    // The original owns script ordering, errors, drawing permission and native overlay batches.
    g_drawMonitor(a,b,c,target,size,d,e,f,signal,error,text);
    g_draw = previous;
    if (frame.record) {
        std::lock_guard<std::mutex> lock(g_mutex);
        frame.record->effects = error && *error ? VideoEffects{} : frame.effects;
        frame.record->effectOwner = frame.effectOwner;
        frame.record->captureOwner = frame.captureOwner;
    }
}

GLuint Compile(Gl& g, GLenum type, const char* source) {
    GLuint s = g.CreateShader(type);
    if (!s) return 0;
    g.ShaderSource(s,1,&source,nullptr); g.CompileShader(s);
    GLint ok = 0; g.GetShaderiv(s,0x8b81,&ok);
    if (!ok) {
        char log[2048] = {}; g.GetShaderInfoLog(s,sizeof(log)-1,nullptr,log);
        LogError("Video shader compile failed, vanilla rendering retained: %s",log);
        g.DeleteShader(s); return 0;
    }
    return s;
}
Gl::Program& Program(Gl& g, GLuint original) {
    auto& p = g.programs[original];
    if (p.tried) return p;
    p.tried = true;
    const char* names[] = {"mat_view_proj","mat_world","texture_video","texture_overlay","alpha_video","alpha","multiply_color"};
    for (size_t i = 0; i < 7; ++i) {
        p.original[i] = g.GetUniformLocation(original,names[i]);
        if (p.original[i] < 0) { LogError("Video: native monitor uniform missing: %s",names[i]); return p; }
    }
    const char* attrs[] = {"vertexPosition_in","vertexCoord0_in","vertexColor_in"};
    GLint bindings[3];
    for (size_t i = 0; i < 3; ++i) {
        bindings[i] = g.GetAttribLocation(original,attrs[i]);
        if (bindings[i] < 0) return p;
    }
    GLuint vs = Compile(g,0x8b31,kVideoVertexShader), fs = Compile(g,0x8b30,kVideoFragmentShader);
    if (!vs || !fs) { if (vs) g.DeleteShader(vs); if (fs) g.DeleteShader(fs); return p; }
    GLuint id = g.CreateProgram();
    if (id) {
        g.AttachShader(id,vs); g.AttachShader(id,fs);
        for (size_t i = 0; i < 3; ++i) g.BindAttribLocation(id,GLuint(bindings[i]),attrs[i]);
        g.LinkProgram(id);
    }
    g.DeleteShader(vs); g.DeleteShader(fs);
    GLint ok = 0; if (id) g.GetProgramiv(id,0x8b82,&ok);
    if (!ok) {
        char log[2048] = {}; if (id) { g.GetProgramInfoLog(id,sizeof(log)-1,nullptr,log); g.DeleteProgram(id); }
        LogError("Video shader link failed, vanilla rendering retained: %s",log); return p;
    }
    for (size_t i = 0; i < 7; ++i) p.custom[i] = g.GetUniformLocation(id,names[i]);
    p.id = id;
    LogInfo("Video: native monitor FX shader ready");
    return p;
}
void APIENTRY DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!g_gpu || !LeaseActive(g_gpu->owner) || g_gpu->effects.identity() || !wglGetCurrentContext()) {
        g_drawElements(mode,count,type,indices); return;
    }
    auto& g = CurrentGl();
    GLint original = 0; glGetIntegerv(0x8b8d,&original);
    if (!g.effects || !original) { g_drawElements(mode,count,type,indices); return; }
    auto& p = Program(g,GLuint(original));
    if (!p.id) { g_drawElements(mode,count,type,indices); return; }
    // Copy the native uniforms after the original renderer has set world/alpha/color.
    GLfloat matrices[2][16] = {}, color[4] = {}, alpha[2] = {};
    GLint samplers[2] = {};
    for (int i = 0; i < 2; ++i) {
        g.GetUniformfv(original,p.original[i],matrices[i]);
        g.GetUniformiv(original,p.original[i+2],&samplers[i]);
        g.GetUniformfv(original,p.original[i+4],&alpha[i]);
    }
    g.GetUniformfv(original,p.original[6],color);
    g.UseProgram(p.id);
    for (int i = 0; i < 2; ++i) {
        g.UniformMatrix4fv(p.custom[i],1,GL_FALSE,matrices[i]);
        g.Uniform1i(p.custom[i+2],samplers[i]); g.Uniform1f(p.custom[i+4],alpha[i]);
    }
    g.Uniform4fv(p.custom[6],1,color);
    const auto& fx = g_gpu->effects;
    // Public coordinates are top-left pixels; native monitor UV Y is bottom-to-top.
    g.Uniform4f(g.GetUniformLocation(p.id,"fx_transform"),fx.offsetX / g_gpu->width,
                -fx.offsetY / g_gpu->height,fx.scaleX,fx.scaleY);
    g.Uniform4f(g.GetUniformLocation(p.id,"fx_tint"),fx.r,fx.g,fx.b,fx.a);
    g.Uniform2f(g.GetUniformLocation(p.id,"fx_filter"),fx.grayscale,fx.invert);
    g_drawElements(mode,count,type,indices);
    g.UseProgram(GLuint(original)); // native state cache remains valid
}

void DeleteSlots(Gl& g, Record& r) {
    for (auto& s : r.slots) {
        if (s.fence) g.DeleteSync(s.fence);
        if (s.pbo) g.DeleteBuffers(1,&s.pbo);
        g.allocated -= s.bytes;
        s = {};
    }
}
void CollectRetired(Gl& g, HGLRC context) {
    std::vector<std::shared_ptr<Record>> retired;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_retired.begin(); it != g_retired.end();) {
            if (!(*it)->context || (*it)->context == context) { retired.push_back(*it); it = g_retired.erase(it); }
            else ++it;
        }
    }
    if (g.capture) for (const auto& r : retired) DeleteSlots(g,*r);
}
struct PackState {
    GLint buffer = 0, active = 0, texture = 0;
    static constexpr std::array<GLenum, 6> params = {0x0d05,0x0d02,0x0d03,0x0d04,0x806c,0x806b};
    std::array<GLint, 6> values{};
    Gl& g;
    PackState(Gl& gl, GLuint source) : g(gl) {
        glGetIntegerv(0x88ed,&buffer); glGetIntegerv(0x84e0,&active);
        g.ActiveTexture(0x84c0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        glBindTexture(GL_TEXTURE_2D,source);
        for (size_t i = 0; i < params.size(); ++i) {
            glGetIntegerv(params[i],&values[i]); glPixelStorei(params[i],i == 0 ? 1 : 0);
        }
    }
    ~PackState() {
        g.BindBuffer(0x88eb,GLuint(buffer));
        for (size_t i = 0; i < params.size(); ++i) glPixelStorei(params[i],values[i]);
        glBindTexture(GL_TEXTURE_2D,GLuint(texture)); g.ActiveTexture(GLenum(active));
    }
};
void Capture(Gl& g, const std::shared_ptr<Record>& r, GLuint source, int logicalW, int logicalH) {
    if (!g.capture || g.readbackFailed || !source || !r->active) return;
    HGLRC context = wglGetCurrentContext();
    if (r->context && r->context != context) return; // no cross-context PBO access
    r->context = context;
    PackState restore(g,source);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&h);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    size_t bytes = size_t(w) * h * 4;
    if (bytes > kMaxCaptureBytes) return;
    if (r->source != source || r->sourceWidth != w || r->sourceHeight != h) {
        DeleteSlots(g,*r); r->source = source; r->sourceWidth = w; r->sourceHeight = h;
        std::lock_guard<std::mutex> lock(g_mutex); r->latest.reset();
    }
    // Poll without waiting. Map only after the GPU signals completion.
    for (auto& s : r->slots) if (s.fence) {
        GLenum status = g.ClientWaitSync(s.fence,0,0);
        if (status == 0x911a || status == 0x911c) {
            g.BindBuffer(0x88eb,s.pbo);
            void* data = g.MapBufferRange(0x88eb,0,intptr_t(s.bytes),1);
            if (data) {
                auto image = std::make_shared<VideoCapture>();
                image->width = w; image->height = h; image->logicalWidth = logicalW; image->logicalHeight = logicalH;
                image->rgba.resize(bytes); memcpy(image->rgba.data(),data,bytes);
                if (g.UnmapBuffer(0x88eb)) {
                    image->sequence = s.sequence;
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (r->active && (!r->latest || r->latest->sequence < image->sequence)) r->latest = image;
                }
            }
            g.DeleteSync(s.fence); s.fence = nullptr;
        } else if (status == 0x911d) {
            LogError("Video: GPU readback fence failed; capture disabled for this context");
            DeleteSlots(g,*r); g.readbackFailed = true; return;
        }
    }
    for (auto& s : r->slots) if (!s.fence) {
        if (!s.pbo) {
            if (g.allocated + bytes > kGpuBudget) return;
            g.GenBuffers(1,&s.pbo); if (!s.pbo) return;
            g.BindBuffer(0x88eb,s.pbo); g.BufferData(0x88eb,intptr_t(bytes),nullptr,0x88e1);
            GLint allocated = 0;
            // Buffer allocation is checked through glGetBufferParameteriv below when available.
            auto getSize = Proc<void (APIENTRY*)(GLenum, GLenum, GLint*)>("glGetBufferParameteriv");
            if (getSize) getSize(0x88eb,0x8764,&allocated);
            if (allocated != GLint(bytes)) { g.DeleteBuffers(1,&s.pbo); s.pbo = 0; return; }
            s.bytes = bytes; g.allocated += bytes;
        }
        g.BindBuffer(0x88eb,s.pbo);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        s.sequence = ++g_sequence;
        s.fence = g.FenceSync(0x9117,0);
        if (!s.fence) { DeleteSlots(g,*r); g.readbackFailed = true; LogError("Video: unable to create readback fence"); }
        return; // at most one capture per monitor/native draw
    }
}
void DrawGpu(void* shader, void* batch, void* video, void* overlay, void* world,
             float alphaVideo, float alpha, uint32_t color) {
    GpuContext frame;
    std::shared_ptr<Record> record;
    std::shared_ptr<Lease> capture;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_overlays.find(overlay);
        if (it != g_overlays.end()) record = it->second.lock();
        if (record && record->active) {
            frame.effects = record->effects; frame.width = record->width; frame.height = record->height;
            frame.owner = record->effectOwner; capture = record->captureOwner;
        }
    }
    if (wglGetCurrentContext()) {
        auto& gl = CurrentGl();
        CollectRetired(gl,wglGetCurrentContext());
        if (record && LeaseActive(capture)) Capture(gl,record,TextureId(video),frame.width,frame.height);
        else if (record && gl.capture && record->context == wglGetCurrentContext()) {
            DeleteSlots(gl,*record); record->source = 0;
            std::lock_guard<std::mutex> lock(g_mutex); record->latest.reset();
        }
    }
    auto previous = g_gpu; g_gpu = frame.width > 0 && frame.height > 0 ? &frame : nullptr;
    g_drawGpu(shader,batch,video,overlay,world,alphaVideo,alpha,color);
    g_gpu = previous;
}

// CALL rel32 -> loader-owned near JMP [RIP] stub. Original instructions are untouched
// except these two proven call sites; no guessed instruction-length trampoline.
void* NearStub(uint8_t* base, void* callback) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t start = reinterpret_cast<uintptr_t>(base) & ~(uintptr_t(si.dwAllocationGranularity)-1);
    for (uintptr_t distance = si.dwAllocationGranularity; distance < 0x70000000; distance += si.dwAllocationGranularity) {
        for (int direction : {1,-1}) {
            uintptr_t candidate = direction > 0 ? start + distance : (start > distance ? start - distance : 0);
            if (!candidate) continue;
            auto p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if (!p) continue;
            p[0] = 0xff; p[1] = 0x25; memset(p+2,0,4); memcpy(p+6,&callback,8);
            DWORD old; if (!VirtualProtect(p,4096,PAGE_EXECUTE_READ,&old)) { VirtualFree(p,0,MEM_RELEASE); return nullptr; }
            FlushInstructionCache(GetCurrentProcess(),p,14); return p;
        }
    }
    return nullptr;
}
bool Write(void* p, const void* data, size_t bytes) {
    DWORD old; if (!VirtualProtect(p,bytes,PAGE_EXECUTE_READWRITE,&old)) return false;
    memcpy(p,data,bytes); FlushInstructionCache(GetCurrentProcess(),p,bytes);
    DWORD ignored; VirtualProtect(p,bytes,old,&ignored); return true;
}
bool CallTo(uint8_t* base, uint32_t rva, void* stub) {
    intptr_t relative = reinterpret_cast<uint8_t*>(stub) - (base+rva+5);
    if (relative < INT32_MIN || relative > INT32_MAX) return false;
    int32_t value = int32_t(relative); return Write(base+rva+1,&value,4);
}
bool IsCallTo(uint8_t* base, uint32_t call, uint32_t target) {
    if (!call || !target || base[call] != 0xe8) return false;
    int32_t relative;
    memcpy(&relative,base+call+1,sizeof(relative));
    return int64_t(call) + 5 + relative == target;
}
bool CheckProfile(uint8_t* base) {
    __try {
        if (!base || !IsSupportedEngineBuild(base) || !ValidateEngineLayouts(base)) return false;
        // Keys are reference-build identities, never addresses in an unknown build.
        // Central profiles verify disk identity and build-specific bytes/layout witnesses.
        const uint32_t keys[] = {kMonitorDrawCall,kGpuDrawCall,kMonitorDraw,kGpuDraw,
                                kMonitorDestroy,kMonitorVtable,kDrawElementsIat};
        for (uint32_t key : keys) {
            if (!EngineRva(base,key) || !ValidateEngineBinding(base,key)) {
                LogError("Video: unsupported or changed engine binding 0x%X",key);
                return false;
            }
        }
        if (!IsCallTo(base,EngineRva(base,kMonitorDrawCall),EngineRva(base,kMonitorDraw)) ||
            !IsCallTo(base,EngineRva(base,kGpuDrawCall),EngineRva(base,kGpuDraw)) ||
            *reinterpret_cast<void**>(base+EngineRva(base,kMonitorVtable)) != base+EngineRva(base,kMonitorDestroy)) {
            LogError("Video: native call or destructor slot changed; refusing to overwrite another hook");
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
bool ValidateVideoProfile(uint8_t* base) { return CheckProfile(base); }
bool InstallNativeVideo(uint8_t* base) {
    if (g_installed) return true;
    if (!CheckProfile(base)) { LogError("Video: unsupported native profile; service disabled"); return false; }
    const uint32_t monitorCall = EngineRva(base,kMonitorDrawCall), gpuCall = EngineRva(base,kGpuDrawCall);
    const uint32_t monitorDraw = EngineRva(base,kMonitorDraw), gpuDraw = EngineRva(base,kGpuDraw);
    const uint32_t destroy = EngineRva(base,kMonitorDestroy), table = EngineRva(base,kMonitorVtable);
    // The central resolver finds this slot by OPENGL32.DLL!glDrawElements import name.
    const uint32_t drawElementsIat = EngineRva(base,kDrawElementsIat);
    auto nativeDraw = reinterpret_cast<DrawElementsFn>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"),"glDrawElements"));
    if (!nativeDraw || *reinterpret_cast<DrawElementsFn*>(base+drawElementsIat) != nativeDraw) {
        LogError("Video: unexpected glDrawElements import; refusing to overwrite another hook"); return false;
    }
    void* monitorStub = NearStub(base,reinterpret_cast<void*>(&DrawMonitor));
    void* gpuStub = NearStub(base,reinterpret_cast<void*>(&DrawGpu));
    if (!monitorStub || !gpuStub) {
        if (monitorStub) VirtualFree(monitorStub,0,MEM_RELEASE);
        if (gpuStub) VirtualFree(gpuStub,0,MEM_RELEASE);
        LogError("Video: unable to allocate native call relay near the game module");
        return false;
    }
    // Relay allocation can take time. Recheck all patch slots before the first write.
    if (!CheckProfile(base) || *reinterpret_cast<DrawElementsFn*>(base+drawElementsIat) != nativeDraw) {
        VirtualFree(monitorStub,0,MEM_RELEASE); VirtualFree(gpuStub,0,MEM_RELEASE);
        LogError("Video: engine bindings changed before installation; no hooks written"); return false;
    }
    g_drawMonitor = reinterpret_cast<DrawMonitorFn>(base+monitorDraw);
    g_drawGpu = reinterpret_cast<DrawGpuFn>(base+gpuDraw);
    g_destroy = reinterpret_cast<DestroyFn>(base+destroy);
    g_drawElements = nativeDraw;
    int32_t monitorOld, gpuOld;
    memcpy(&monitorOld,base+monitorCall+1,4); memcpy(&gpuOld,base+gpuCall+1,4);
    auto newDraw = &DrawElements; auto newDestroy = &MonitorDestroy;
    bool m = CallTo(base,monitorCall,monitorStub);
    bool gpu = m && CallTo(base,gpuCall,gpuStub);
    bool iat = gpu && Write(base+drawElementsIat,&newDraw,sizeof(newDraw));
    bool vt = iat && Write(base+table,&newDestroy,sizeof(newDestroy));
    if (!vt) {
        // Stubs remain allocated even after rollback: a native frame may have fetched a call target.
        if (m) Write(base+monitorCall+1,&monitorOld,4);
        if (gpu) Write(base+gpuCall+1,&gpuOld,4);
        if (iat) Write(base+drawElementsIat,&nativeDraw,sizeof(nativeDraw));
        LogError("Video: hook installation failed; rolled back"); return false;
    }
    g_installed = true;
    LogInfo("Video: loader-owned native monitor hooks installed (experimental runtime validation pending)");
    return true;
}
const SwVideoApi* NativeVideoPrimitives() {
    static const SwVideoApi api = {sizeof(SwVideoApi),&Enable,&ReleaseNativeVideoOwner,
        &Offset,&Scale,&Tint,&Filter,&Reset,&Request,&Pixel};
    return &api;
}
void ReleaseNativeVideoOwner(const SwModHost* owner) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_owners.find(owner);
    if (it != g_owners.end()) { it->second->active = false; g_owners.erase(it); }
    if (g_owners.empty()) {
        for (auto& item : g_monitors) Retire(item.second);
        g_monitors.clear(); g_overlays.clear();
    }
}
}
