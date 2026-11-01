#include "custom_display_vehicle_observer.h"
#include "custom_display_dialog_layout.h"
#include "custom_display_dialog_native.h"
#include "custom_display_registry.h"
#include "engine_bindings.h"
#include "hook_transaction.h"
#include "log.h"
#include <windows.h>
#include <GL/gl.h>
#include <atomic>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <fstream>

namespace swml {
namespace {
using InitFn = void (*)(void*, void*);
using UpdateFn = void (*)(void*, void*, void*, void*);
using DestroyFn = void* (*)(void*, uint32_t);
using TeardownFn = void (*)(void*);
using ActivityFn = void (*)(void*, void*, void*, void*);
using ActionsFn = void (*)(void*, void*, void*, void*, uintptr_t, int32_t);

InitFn originalInit = nullptr, originalUiTeardown = nullptr;
UpdateFn originalUpdate = nullptr;
DestroyFn originalDestroy = nullptr;
TeardownFn originalStateTeardown = nullptr;
ActionsFn originalActions = nullptr;
ActivityFn originalEnter = nullptr, originalLeave = nullptr;

CustomDisplayVehicleObserverLayout layout;
std::atomic<uint32_t> events{0}, frames{0};
constexpr uint32_t maxEvents = 4096;
bool installed = false;
uintptr_t g_base = 0;

CustomDisplayDialogNativeApi g_dialogApi{};
CustomDisplayDialogNativeTree g_dialogTree{};
void* g_dialogEditor = nullptr;
void** g_vehicleUiTable = nullptr;
void** g_vehicleStateTable = nullptr;
uint32_t g_stateEnterSlot = 0;
uint32_t g_stateLeaveSlot = 0;

bool Pointer(const void* base, size_t offset, void*& out) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    if (!address || offset > UINTPTR_MAX - address || sizeof(out) > UINTPTR_MAX - (address + offset))
        return false;
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + offset),
        &out, sizeof(out), &count) && count == sizeof(out);
}

void* StateContext(void* state) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(state);
    const size_t offset = size_t(layout.stateRoot) + layout.widgets.rootContext;
    return address && offset <= UINTPTR_MAX - address ? reinterpret_cast<void*>(address + offset) : nullptr;
}

uint32_t Begin() noexcept {
    uint32_t event = events.load(std::memory_order_relaxed);
    do {
        if (event >= maxEvents) return UINT32_MAX;
    } while (!events.compare_exchange_weak(event, event + 1, std::memory_order_relaxed));
    return event;
}

void Observe(uint32_t event, const char* phase, void* object, void* context,
        bool readUi, int32_t action = -1) noexcept {
    if (event == UINT32_MAX) return;
    try {
        void *parent = nullptr, *modal = nullptr;
        const bool valid = context && Pointer(context, layout.widgets.contextModal, modal) &&
            (!readUi || Pointer(object, layout.widgets.parent, parent));
        LogInfo("CDVEH event=%u phase=%s thread=%lu object=%p context=%p parent=%p modal=%p valid=%d action=%d",
            event, phase, GetCurrentThreadId(), object, context, parent, modal, valid, action);
    } catch (...) {}
}

void* g_editorState = nullptr;

void* FindComponentDefinitionInRegistry(void* reg, const char* name) {
    if (!reg || !name) return nullptr;

    uint8_t* uReg = reinterpret_cast<uint8_t*>(reg);
    void** allDefs = nullptr;
    uint32_t cap = 0;
    int32_t head = 0;
    uint32_t count = 0;
    SIZE_T read = 0;

    // Method 1: Check the global ring buffer at reg + 0xf8
    if (ReadProcessMemory(GetCurrentProcess(), uReg + 0xf8, &allDefs, sizeof(allDefs), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0x100, &cap, sizeof(cap), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0x104, &head, sizeof(head), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0x108, &count, sizeof(count), &read) &&
        allDefs && cap > 0 && count > 0 && count <= 5000) {

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = (head + i) % cap;
            void* def = nullptr;
            if (!ReadProcessMemory(GetCurrentProcess(), &allDefs[idx], &def, sizeof(def), &read) || !def)
                continue;
            const char* defIdPtr = nullptr;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(def) + 0x288, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                char buf[64] = {};
                if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                    buf[sizeof(buf) - 1] = '\0';
                    if (_stricmp(buf, name) == 0) {
                        return def;
                    }
                }
            }
        }
    }

    // Method 1b: Check the variant ring buffer at reg + 0xc8
    void** vDefs = nullptr;
    uint32_t vCap = 0;
    int32_t vHead = 0;
    uint32_t vCount = 0;
    if (ReadProcessMemory(GetCurrentProcess(), uReg + 0xc8, &vDefs, sizeof(vDefs), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xd0, &vCap, sizeof(vCap), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xd4, &vHead, sizeof(vHead), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xd8, &vCount, sizeof(vCount), &read) &&
        vDefs && vCap > 0 && vCount > 0 && vCount <= 50000) {

        for (uint32_t i = 0; i < vCount; ++i) {
            uint32_t idx = (vHead + i) % vCap;
            void* def = nullptr;
            if (!ReadProcessMemory(GetCurrentProcess(), &vDefs[idx], &def, sizeof(def), &read) || !def)
                continue;
            const char* defIdPtr = nullptr;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(def) + 0x288, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                char buf[64] = {};
                if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                    buf[sizeof(buf) - 1] = '\0';
                    if (_stricmp(buf, name) == 0) {
                        return def;
                    }
                }
            }
        }
    }

    // Method 2: Check Category buckets (reg + 0x110 + cat * 24)
    for (int cat = 0; cat < 16; ++cat) {
        uint8_t* bucket = uReg + 0x110 + cat * 24;
        void** buffer = nullptr;
        uint32_t capacity = 0;
        int32_t bHead = 0;
        uint32_t bCount = 0;

        if (ReadProcessMemory(GetCurrentProcess(), bucket, &buffer, sizeof(buffer), &read) &&
            ReadProcessMemory(GetCurrentProcess(), bucket + 8, &capacity, sizeof(capacity), &read) &&
            ReadProcessMemory(GetCurrentProcess(), bucket + 12, &bHead, sizeof(bHead), &read) &&
            ReadProcessMemory(GetCurrentProcess(), bucket + 16, &bCount, sizeof(bCount), &read) &&
            buffer && capacity > 0 && bCount > 0 && bCount <= 5000) {

            for (uint32_t i = 0; i < bCount; ++i) {
                uint32_t idx = (bHead + i) % capacity;
                void* def = nullptr;
                if (!ReadProcessMemory(GetCurrentProcess(), &buffer[idx], &def, sizeof(def), &read) || !def)
                    continue;
                const char* defIdPtr = nullptr;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(def) + 0x288, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                    char buf[64] = {};
                    if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                        buf[sizeof(buf) - 1] = '\0';
                        if (_stricmp(buf, name) == 0) {
                            return def;
                        }
                    }
                }
            }
        }
    }

    return nullptr;
}

void* FindComponentDefinition(void* self, const char* name) {
    if (!self || !name) return nullptr;

    void* world = nullptr;
    if (Pointer(self, 0x70, world) && world) {
        void* def = FindComponentDefinitionInRegistry(reinterpret_cast<uint8_t*>(world) + 0xbb670, name);
        if (def) return def;
    }

    // Method 3: Check catalogue at *(ui + 0x150)
    void* ui = nullptr;
    if (Pointer(self, 0x8a8, ui) && ui) {
        void* catalogue = nullptr;
        if (Pointer(ui, 0x150, catalogue) && catalogue) {
            void** buffer = nullptr;
            uint32_t capacity = 0;
            int32_t head = 0;
            int32_t count = 0;
            SIZE_T read = 0;

            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(catalogue) + 0x60, &buffer, sizeof(buffer), &read) &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(catalogue) + 0x68, &capacity, sizeof(capacity), &read) &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(catalogue) + 0x6c, &head, sizeof(head), &read) &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(catalogue) + 0x70, &count, sizeof(count), &read) &&
                buffer && capacity > 0 && count > 0 && count <= 5000) {

                for (int32_t i = 0; i < count; ++i) {
                    uint32_t idx = (head + i) % capacity;
                    void* entry = nullptr;
                    if (!ReadProcessMemory(GetCurrentProcess(), &buffer[idx], &entry, sizeof(entry), &read) || !entry)
                        continue;
                    void* def = nullptr;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(entry) + 8, &def, sizeof(def), &read) || !def)
                        continue;
                    const char* defIdPtr = nullptr;
                    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(def) + 0x288, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                        char buf[64] = {};
                        if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                            buf[sizeof(buf) - 1] = '\0';
                            if (_stricmp(buf, name) == 0) {
                                return def;
                            }
                        }
                    }
                }
            }
        }
    }

    return nullptr;
}

#pragma pack(push, 1)
struct VoxelEntry {
    int32_t x;                      // 0x00
    int32_t y;                      // 0x04
    int32_t z;                      // 0x08
    uint32_t flags;                 // 0x0c: 1 = physics active
    uint32_t physics_shape;         // 0x10: 0
    int32_t rot[9];                 // 0x14: identity matrix {1,0,0, 0,1,0, 0,0,1}
    uint32_t buoy_pipes;            // 0x38: 0
};
#pragma pack(pop)
static_assert(sizeof(VoxelEntry) == 60, "VoxelEntry size must be 60 bytes");

struct DefRingBuffer {
    void* data;      // +0x00
    uint32_t cap;    // +0x08
    int32_t head;    // +0x0c
    uint32_t count;  // +0x10
    uint32_t pad;    // +0x14
};
static_assert(sizeof(DefRingBuffer) == 24, "DefRingBuffer size must be 24 bytes");

bool BuildCustomDisplayMeshBuffers(int w, int h, std::vector<uint8_t>& frameBuf, std::vector<uint8_t>& arrowBuf) {
    int xmin = -(w / 2);
    int xmax = (w - 1) / 2;
    int zmin = -((h - 1) / 2);
    int zmax = h / 2;

    // 1. Frame mesh from component_screen_1.mesh
    FILE* fIn = nullptr;
    if (fopen_s(&fIn, "rom/meshes/component_screen_1.mesh", "rb") == 0 && fIn) {
        fseek(fIn, 0, SEEK_END);
        long sz = ftell(fIn);
        fseek(fIn, 0, SEEK_SET);
        if (sz == 2730) {
            frameBuf.resize(sz);
            fread(frameBuf.data(), 1, sz, fIn);
            fclose(fIn);

            for (int i = 0; i < 84; ++i) {
                float* pX = reinterpret_cast<float*>(frameBuf.data() + 14 + i * 28 + 0);
                float* pZ = reinterpret_cast<float*>(frameBuf.data() + 14 + i * 28 + 8);
                *pX += 0.25f * (*pX < 0.0f ? xmin : xmax);
                *pZ += 0.25f * (*pZ < 0.0f ? zmin : zmax);
            }
            float* bounds = reinterpret_cast<float*>(frameBuf.data() + 2684);
            bounds[0] = xmin * 0.25f - 0.125f;
            bounds[1] = 0.1028f;
            bounds[2] = zmin * 0.25f - 0.125f;
            bounds[3] = xmax * 0.25f + 0.125f;
            bounds[4] = 0.1250f;
            bounds[5] = zmax * 0.25f + 0.125f;
        } else {
            fclose(fIn);
            return false;
        }
    } else {
        return false;
    }

    // 2. Arrow mesh from arrow_screen_1.mesh
    if (fopen_s(&fIn, "rom/meshes/arrow_screen_1.mesh", "rb") == 0 && fIn) {
        fseek(fIn, 0, SEEK_END);
        long sz = ftell(fIn);
        fseek(fIn, 0, SEEK_SET);
        if (sz == 1397) {
            arrowBuf.resize(sz);
            fread(arrowBuf.data(), 1, sz, fIn);
            fclose(fIn);

            float dx = (xmin + xmax) * 0.125f;
            float dz = (zmin + zmax) * 0.125f;

            for (int i = 0; i < 42; ++i) {
                float* pX = reinterpret_cast<float*>(arrowBuf.data() + 14 + i * 28 + 0);
                float* pZ = reinterpret_cast<float*>(arrowBuf.data() + 14 + i * 28 + 8);
                *pX += dx;
                *pZ += dz;
            }
            float* bounds = reinterpret_cast<float*>(arrowBuf.data() + 1352);
            bounds[0] += dx;
            bounds[2] += dz;
            bounds[3] += dx;
            bounds[5] += dz;
        } else {
            fclose(fIn);
            return false;
        }
    } else {
        return false;
    }

    // Also write to disk cache rom/meshes/swml_cd_... so any filesystem queries find them
    char meshRel[128], arrowRel[128];
    sprintf_s(meshRel, "rom/meshes/swml_cd_%dx%d.mesh", w, h);
    sprintf_s(arrowRel, "rom/meshes/swml_cd_%dx%d_arrow.mesh", w, h);
    FILE* fOut = nullptr;
    if (fopen_s(&fOut, meshRel, "wb") == 0 && fOut) {
        fwrite(frameBuf.data(), 1, frameBuf.size(), fOut);
        fclose(fOut);
    }
    if (fopen_s(&fOut, arrowRel, "wb") == 0 && fOut) {
        fwrite(arrowBuf.data(), 1, arrowBuf.size(), fOut);
        fclose(fOut);
    }

    return true;
}

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif

using PFNGLGENBUFFERSPROC = void (APIENTRY *)(GLsizei n, GLuint *buffers);
using PFNGLBINDBUFFERPROC = void (APIENTRY *)(GLenum target, GLuint buffer);
using PFNGLBUFFERDATAPROC = void (APIENTRY *)(GLenum target, ptrdiff_t size, const void *data, GLenum usage);

static PFNGLGENBUFFERSPROC s_glGenBuffers = nullptr;
static PFNGLBINDBUFFERPROC s_glBindBuffer = nullptr;
static PFNGLBUFFERDATAPROC s_glBufferData = nullptr;

static bool EnsureOpenGLExtensions() {
    if (s_glGenBuffers && s_glBindBuffer && s_glBufferData) return true;

    s_glGenBuffers = reinterpret_cast<PFNGLGENBUFFERSPROC>(wglGetProcAddress("glGenBuffers"));
    s_glBindBuffer = reinterpret_cast<PFNGLBINDBUFFERPROC>(wglGetProcAddress("glBindBuffer"));
    s_glBufferData = reinterpret_cast<PFNGLBUFFERDATAPROC>(wglGetProcAddress("glBufferData"));

    if (!s_glGenBuffers && g_base) {
        s_glGenBuffers = *reinterpret_cast<PFNGLGENBUFFERSPROC*>(g_base + 0xCE63F0);
    }
    if (!s_glBindBuffer && g_base) {
        s_glBindBuffer = *reinterpret_cast<PFNGLBINDBUFFERPROC*>(g_base + 0xCE6358);
    }
    if (!s_glBufferData && g_base) {
        s_glBufferData = *reinterpret_cast<PFNGLBUFFERDATAPROC*>(g_base + 0xCE6370);
    }

    return (s_glGenBuffers && s_glBindBuffer && s_glBufferData);
}

struct CustomGpuMesh {
    uint8_t* gpuBuffers = nullptr;     // 16 bytes: c_resource_object_geometry
    uint8_t* geom = nullptr;           // 0x68 bytes: mm_resource_geometry
    uint8_t* cpuVertices = nullptr;    // persistent CPU vertex buffer
    uint16_t* cpuIndices = nullptr;    // persistent CPU index buffer
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vbo = 0;
    uint32_t ibo = 0;
    bool uploaded = false;
};

struct CustomDisplayGpuPair {
    CustomGpuMesh frame;
    CustomGpuMesh arrow;
};

static std::map<std::pair<int, int>, CustomDisplayGpuPair> s_gpuMeshCache;

static void FlushPendingCustomDisplayGpuBuffers() {
    HGLRC ctx = wglGetCurrentContext();
    if (!ctx) return;
    if (!EnsureOpenGLExtensions()) return;

    GLint prevVbo = 0, prevIbo = 0;
    bool checkedBindings = false;

    for (auto& kv : s_gpuMeshCache) {
        auto& pair = kv.second;
        if (!pair.frame.uploaded && pair.frame.geom && pair.frame.cpuVertices && pair.frame.cpuIndices) {
            if (!checkedBindings) {
                glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &prevVbo);
                glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &prevIbo);
                checkedBindings = true;
            }
            GLuint bufs[2] = {0, 0};
            s_glGenBuffers(2, bufs);
            if (bufs[0] && bufs[1]) {
                s_glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
                s_glBufferData(GL_ARRAY_BUFFER, 2352, pair.frame.cpuVertices, GL_STATIC_DRAW);

                s_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs[1]);
                s_glBufferData(GL_ELEMENT_ARRAY_BUFFER, 300, pair.frame.cpuIndices, GL_STATIC_DRAW);

                pair.frame.vbo = bufs[0];
                pair.frame.ibo = bufs[1];
                *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x60) = bufs[0];
                *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x64) = bufs[1];
                pair.frame.uploaded = true;
                LogInfo("CDVEH: Deferred upload frame mesh %dx%d -> VBO=%u, IBO=%u",
                        kv.first.first, kv.first.second, bufs[0], bufs[1]);
            }
        }
        if (!pair.arrow.uploaded && pair.arrow.geom && pair.arrow.cpuVertices && pair.arrow.cpuIndices) {
            if (!checkedBindings) {
                glGetIntegerv(0x8894, &prevVbo);
                glGetIntegerv(0x8895, &prevIbo);
                checkedBindings = true;
            }
            GLuint bufs[2] = {0, 0};
            s_glGenBuffers(2, bufs);
            if (bufs[0] && bufs[1]) {
                s_glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
                s_glBufferData(GL_ARRAY_BUFFER, 1176, pair.arrow.cpuVertices, GL_STATIC_DRAW);

                s_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs[1]);
                s_glBufferData(GL_ELEMENT_ARRAY_BUFFER, 144, pair.arrow.cpuIndices, GL_STATIC_DRAW);

                pair.arrow.vbo = bufs[0];
                pair.arrow.ibo = bufs[1];
                *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x60) = bufs[0];
                *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x64) = bufs[1];
                pair.arrow.uploaded = true;
                LogInfo("CDVEH: Deferred upload arrow mesh %dx%d -> VBO=%u, IBO=%u",
                        kv.first.first, kv.first.second, bufs[0], bufs[1]);
            }
        }
    }

    if (checkedBindings) {
        s_glBindBuffer(GL_ARRAY_BUFFER, prevVbo);
        s_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevIbo);
    }
}

static CustomDisplayGpuPair GetOrCreateCustomDisplayGpuPair(int w, int h, void* baseMon) {
    auto key = std::make_pair(w, h);
    auto it = s_gpuMeshCache.find(key);
    if (it != s_gpuMeshCache.end()) {
        if (!it->second.frame.uploaded) {
            FlushPendingCustomDisplayGpuBuffers();
        }
        return it->second;
    }

    std::vector<uint8_t> frameBuf;
    std::vector<uint8_t> arrowBuf;
    if (!BuildCustomDisplayMeshBuffers(w, h, frameBuf, arrowBuf)) {
        LogWarn("CDVEH: Failed to build custom display mesh buffers for %dx%d", w, h);
        return {};
    }

    CustomDisplayGpuPair pair{};

    // 1. Setup persistent CPU buffers for frame
    pair.frame.vertexCount = 84;
    pair.frame.indexCount = 150;
    pair.frame.cpuVertices = new uint8_t[2352];
    memcpy(pair.frame.cpuVertices, frameBuf.data() + 14, 2352);
    pair.frame.cpuIndices = new uint16_t[150];
    memcpy(pair.frame.cpuIndices, frameBuf.data() + 2370, 300);

    // 2. Setup persistent CPU buffers for arrow
    pair.arrow.vertexCount = 42;
    pair.arrow.indexCount = 72;
    pair.arrow.cpuVertices = new uint8_t[1176];
    memcpy(pair.arrow.cpuVertices, arrowBuf.data() + 14, 1176);
    pair.arrow.cpuIndices = new uint16_t[72];
    memcpy(pair.arrow.cpuIndices, arrowBuf.data() + 1190, 144);

    // 3. Clone header and geometry objects from baseMon
    uint8_t* baseVar = reinterpret_cast<uint8_t*>(baseMon);
    uint8_t* baseFrameMeshRes = baseVar + 0x3a0;
    uint8_t* baseFrameGpuBuffers = *reinterpret_cast<uint8_t**>(baseFrameMeshRes + 0x30);
    uint8_t* baseFrameGeom = baseFrameGpuBuffers ? *reinterpret_cast<uint8_t**>(baseFrameGpuBuffers + 8) : nullptr;

    uint8_t* baseArrowMeshRes = baseVar + 0x620;
    uint8_t* baseArrowGpuBuffers = *reinterpret_cast<uint8_t**>(baseArrowMeshRes + 0x30);
    uint8_t* baseArrowGeom = baseArrowGpuBuffers ? *reinterpret_cast<uint8_t**>(baseArrowGpuBuffers + 8) : nullptr;

    if (baseFrameGpuBuffers && baseFrameGeom) {
        pair.frame.gpuBuffers = new uint8_t[16];
        memcpy(pair.frame.gpuBuffers, baseFrameGpuBuffers, 16);

        pair.frame.geom = new uint8_t[0x68];
        memcpy(pair.frame.geom, baseFrameGeom, 0x68);
        *reinterpret_cast<uint8_t**>(pair.frame.gpuBuffers + 8) = pair.frame.geom;

        *reinterpret_cast<uint32_t*>(pair.frame.geom + 8) = 1; // refCount
        *reinterpret_cast<void**>(pair.frame.geom + 0x28) = pair.frame.cpuVertices;
        *reinterpret_cast<void**>(pair.frame.geom + 0x30) = pair.frame.cpuIndices;
        *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x48) = 28; // vertexStride
        *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x4c) = 84; // vertexCount
        *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x5c) = 150; // indexCount

        // Temporary fallback to baseMon's VBO/IBO so it's never 0
        pair.frame.vbo = *reinterpret_cast<uint32_t*>(baseFrameGeom + 0x60);
        pair.frame.ibo = *reinterpret_cast<uint32_t*>(baseFrameGeom + 0x64);
        *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x60) = pair.frame.vbo;
        *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x64) = pair.frame.ibo;
    }

    if (baseArrowGpuBuffers && baseArrowGeom) {
        pair.arrow.gpuBuffers = new uint8_t[16];
        memcpy(pair.arrow.gpuBuffers, baseArrowGpuBuffers, 16);

        pair.arrow.geom = new uint8_t[0x68];
        memcpy(pair.arrow.geom, baseArrowGeom, 0x68);
        *reinterpret_cast<uint8_t**>(pair.arrow.gpuBuffers + 8) = pair.arrow.geom;

        *reinterpret_cast<uint32_t*>(pair.arrow.geom + 8) = 1; // refCount
        *reinterpret_cast<void**>(pair.arrow.geom + 0x28) = pair.arrow.cpuVertices;
        *reinterpret_cast<void**>(pair.arrow.geom + 0x30) = pair.arrow.cpuIndices;
        *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x48) = 28; // vertexStride
        *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x4c) = 42; // vertexCount
        *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x5c) = 72; // indexCount

        // Temporary fallback to baseMon's arrow VBO/IBO so it's never 0
        pair.arrow.vbo = *reinterpret_cast<uint32_t*>(baseArrowGeom + 0x60);
        pair.arrow.ibo = *reinterpret_cast<uint32_t*>(baseArrowGeom + 0x64);
        *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x60) = pair.arrow.vbo;
        *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x64) = pair.arrow.ibo;
    }

    // 4. Try immediate OpenGL upload if context is active
    HGLRC ctx = wglGetCurrentContext();
    if (ctx && EnsureOpenGLExtensions()) {
        GLint prevVbo = 0, prevIbo = 0;
        glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &prevVbo);
        glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &prevIbo);

        GLuint frameBufs[2] = {0, 0};
        s_glGenBuffers(2, frameBufs);
        if (frameBufs[0] && frameBufs[1]) {
            s_glBindBuffer(GL_ARRAY_BUFFER, frameBufs[0]);
            s_glBufferData(GL_ARRAY_BUFFER, 2352, pair.frame.cpuVertices, GL_STATIC_DRAW);

            s_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, frameBufs[1]);
            s_glBufferData(GL_ELEMENT_ARRAY_BUFFER, 300, pair.frame.cpuIndices, GL_STATIC_DRAW);

            pair.frame.vbo = frameBufs[0];
            pair.frame.ibo = frameBufs[1];
            if (pair.frame.geom) {
                *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x60) = frameBufs[0];
                *reinterpret_cast<uint32_t*>(pair.frame.geom + 0x64) = frameBufs[1];
            }
            pair.frame.uploaded = true;
        }

        GLuint arrowBufs[2] = {0, 0};
        s_glGenBuffers(2, arrowBufs);
        if (arrowBufs[0] && arrowBufs[1]) {
            s_glBindBuffer(GL_ARRAY_BUFFER, arrowBufs[0]);
            s_glBufferData(GL_ARRAY_BUFFER, 1176, pair.arrow.cpuVertices, GL_STATIC_DRAW);

            s_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, arrowBufs[1]);
            s_glBufferData(GL_ELEMENT_ARRAY_BUFFER, 144, pair.arrow.cpuIndices, GL_STATIC_DRAW);

            pair.arrow.vbo = arrowBufs[0];
            pair.arrow.ibo = arrowBufs[1];
            if (pair.arrow.geom) {
                *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x60) = arrowBufs[0];
                *reinterpret_cast<uint32_t*>(pair.arrow.geom + 0x64) = arrowBufs[1];
            }
            pair.arrow.uploaded = true;
        }

        s_glBindBuffer(GL_ARRAY_BUFFER, prevVbo);
        s_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevIbo);

        LogInfo("CDVEH: Created OpenGL buffers for %dx%d (frame VBO=%u, IBO=%u; arrow VBO=%u, IBO=%u)",
                w, h, pair.frame.vbo, pair.frame.ibo, pair.arrow.vbo, pair.arrow.ibo);
    } else {
        LogWarn("CDVEH: OpenGL context not active for %dx%d mesh creation (ctx=%p); queued for deferred upload",
                w, h, ctx);
    }

    s_gpuMeshCache[key] = pair;
    return pair;
}

static void ApplyCustomDisplayGpuPairToVariants(void* defArray8, const CustomDisplayGpuPair& pair) {
    if (!defArray8) return;
    for (int k = 0; k < 8; ++k) {
        uint8_t* dstVar = reinterpret_cast<uint8_t*>(defArray8) + k * 0x2848;

        // Frame mesh slot 0 (+0x3a0)
        if (pair.frame.gpuBuffers) {
            *reinterpret_cast<uint8_t*>(dstVar + 0x3a0 + 0x00) = 1; // isLoaded
            *reinterpret_cast<void**>(dstVar + 0x3a0 + 0x08) = pair.frame.cpuIndices;
            *reinterpret_cast<uint32_t*>(dstVar + 0x3a0 + 0x10) = pair.frame.indexCount;
            *reinterpret_cast<void**>(dstVar + 0x3a0 + 0x18) = pair.frame.cpuVertices;
            *reinterpret_cast<uint32_t*>(dstVar + 0x3a0 + 0x20) = pair.frame.vertexCount;
            *reinterpret_cast<void**>(dstVar + 0x3a0 + 0x30) = pair.frame.gpuBuffers;
        }

        // Arrow mesh slot 4 (+0x620)
        if (pair.arrow.gpuBuffers) {
            *reinterpret_cast<uint8_t*>(dstVar + 0x620 + 0x00) = 1; // isLoaded
            *reinterpret_cast<void**>(dstVar + 0x620 + 0x08) = pair.arrow.cpuIndices;
            *reinterpret_cast<uint32_t*>(dstVar + 0x620 + 0x10) = pair.arrow.indexCount;
            *reinterpret_cast<void**>(dstVar + 0x620 + 0x18) = pair.arrow.cpuVertices;
            *reinterpret_cast<uint32_t*>(dstVar + 0x620 + 0x20) = pair.arrow.vertexCount;
            *reinterpret_cast<void**>(dstVar + 0x620 + 0x30) = pair.arrow.gpuBuffers;
        }
    }
}

static bool IsVariantInRegistry(void* reg, const char* name) {
    if (!reg || !name) return false;
    uint8_t* uReg = reinterpret_cast<uint8_t*>(reg);
    void** buffer = nullptr;
    uint32_t capacity = 0;
    int32_t bHead = 0;
    uint32_t bCount = 0;
    SIZE_T read = 0;

    if (ReadProcessMemory(GetCurrentProcess(), uReg + 0xc8, &buffer, sizeof(buffer), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xd0, &capacity, sizeof(capacity), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xd4, &bHead, sizeof(bHead), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xd8, &bCount, sizeof(bCount), &read) &&
        buffer && capacity > 0 && bCount > 0 && bCount <= 100000) {

        for (uint32_t i = 0; i < bCount; ++i) {
            uint32_t idx = (bHead + i) % capacity;
            void* def = nullptr;
            if (!ReadProcessMemory(GetCurrentProcess(), &buffer[idx], &def, sizeof(def), &read) || !def)
                continue;
            const char* defIdPtr = nullptr;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(def) + 0x288, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                char buf[64] = {};
                if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                    buf[sizeof(buf) - 1] = '\0';
                    if (_stricmp(buf, name) == 0) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static void EnsureVariantsInRegistry(void* reg, void* defArray8, const char* name) {
    if (!reg || !defArray8 || !name) return;
    if (IsVariantInRegistry(reg, name)) return;

    uint8_t* uReg = reinterpret_cast<uint8_t*>(reg);
    auto growFn = reinterpret_cast<void (*)(void*, uint32_t)>(g_base + 0x8dd150);

    uint32_t vCap = *reinterpret_cast<uint32_t*>(uReg + 0xd0);
    int32_t vHead = *reinterpret_cast<int32_t*>(uReg + 0xd4);
    uint32_t vCount = *reinterpret_cast<uint32_t*>(uReg + 0xd8);
    void** vDefs = *reinterpret_cast<void***>(uReg + 0xc8);

    for (int k = 0; k < 8; ++k) {
        uint8_t* dstVar = reinterpret_cast<uint8_t*>(defArray8) + k * 0x2848;
        if (vCount >= vCap && growFn) {
            growFn(uReg + 0xc8, vCap > 0 ? vCap * 2 : 2048);
            vDefs = *reinterpret_cast<void***>(uReg + 0xc8);
            vCap = *reinterpret_cast<uint32_t*>(uReg + 0xd0);
            vHead = *reinterpret_cast<int32_t*>(uReg + 0xd4);
        }
        if (vCount < vCap && vDefs) {
            uint32_t vIdx = (vHead + vCount) % vCap;
            vDefs[vIdx] = dstVar;
            vCount++;
            *reinterpret_cast<uint32_t*>(uReg + 0xd8) = vCount;
        }
    }
    LogInfo("CDVEH: Registered 8 variants of %s in variant registry %p (count=%u, cap=%u)", name, reg, vCount, vCap);
}

static bool IsDescriptorInRegistry(void* reg, const char* name) {
    if (!reg || !name) return false;
    uint8_t* uReg = reinterpret_cast<uint8_t*>(reg);
    void** buffer = nullptr;
    uint32_t capacity = 0;
    int32_t bHead = 0;
    uint32_t bCount = 0;
    SIZE_T read = 0;

    if (ReadProcessMemory(GetCurrentProcess(), uReg + 0xe0, &buffer, sizeof(buffer), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xe8, &capacity, sizeof(capacity), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xec, &bHead, sizeof(bHead), &read) &&
        ReadProcessMemory(GetCurrentProcess(), uReg + 0xf0, &bCount, sizeof(bCount), &read) &&
        buffer && capacity > 0 && bCount > 0 && bCount <= 100000) {

        for (uint32_t i = 0; i < bCount; ++i) {
            uint32_t idx = (bHead + i) % capacity;
            void* desc = nullptr;
            if (!ReadProcessMemory(GetCurrentProcess(), &buffer[idx], &desc, sizeof(desc), &read) || !desc)
                continue;
            const char* defIdPtr = nullptr;
            if (ReadProcessMemory(GetCurrentProcess(), desc, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                char buf[64] = {};
                if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                    buf[sizeof(buf) - 1] = '\0';
                    if (_stricmp(buf, name) == 0) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}


static std::string GenerateCustomDisplayXml(int w, int h, const char* targetName) {
    int xmin = -(w / 2);
    int xmax = (w - 1) / 2;
    int zmin = -((h - 1) / 2);
    int zmax = h / 2;
    int mass = 4 * w * h;
    int value = 500 * w * h;
    float cx = (xmin + xmax) * 0.125f;
    float cz = (zmin + zmax) * 0.125f;

    std::string s;
    s.reserve(16384);

    s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<definition name=\"Custom Display " + std::to_string(w) + "x" + std::to_string(h) + "\" ";
    s += "category=\"6\" type=\"43\" mass=\"" + std::to_string(mass) + "\" value=\"" + std::to_string(value) + "\" ";
    s += "flags=\"0\" tags=\"\" phys_collision_dampen=\"0\" audio_filename_start=\"\" audio_filename_loop=\"\" audio_filename_end=\"\" ";
    s += "audio_filename_start_b=\"\" audio_filename_loop_b=\"\" audio_filename_end_b=\"\" audio_gain=\"0\" ";
    s += "mesh_data_name=\"meshes/swml_cd_" + std::to_string(w) + "x" + std::to_string(h) + ".mesh\" ";
    s += "mesh_0_name=\"\" mesh_1_name=\"\" ";
    s += "mesh_editor_only_name=\"meshes/swml_cd_" + std::to_string(w) + "x" + std::to_string(h) + "_arrow.mesh\" ";
    s += "block_type=\"0\" child_name=\"\" extender_name=\"\" constraint_type=\"0\" constraint_axis=\"1\" constraint_range_of_motion=\"0\" ";
    s += "max_motor_force=\"100\" max_motor_speed=\"10\" cable_radius=\"16\" cable_length=\"0\" seat_pose=\"0\" seat_health_per_sec=\"0\" ";
    s += "buoy_radius=\"1\" buoy_factor=\"1\" buoy_force=\"100\" force_emitter_max_force=\"1000\" force_emitter_max_vector=\"1\" ";
    s += "force_emitter_default_pitch=\"0\" force_emitter_blade_height=\"0\" force_emitter_rotation_speed=\"1\" ";
    s += "force_emitter_blade_physics_length=\"4\" force_emitter_blade_efficiency=\"0.01\" force_emitter_efficiency=\"1\" ";
    s += "engine_max_force=\"1000\" trans_conn_type=\"0\" wheel_radius=\"0.25\" wheel_wishbone_length=\"1.25\" wheel_suspension_height=\"1\" ";
    s += "wheel_wishbone_margin=\"0.085\" wheel_suspension_offset=\"0.25\" wheel_wishbone_offset=\"0\" wheel_type=\"0\" button_type=\"1\" ";
    s += "light_intensity=\"1\" light_range=\"1\" light_ies_map=\"graphics/ies/ies_default.txtr\" light_fov=\"1\" light_type=\"0\" ";
    s += "door_lower_limit=\"-1\" door_upper_limit=\"1\" door_flipped=\"false\" custom_door_type=\"0\" door_side_dist=\"1\" door_up_dist=\"1\" ";
    s += "dynamic_min_rotation=\"0\" dynamic_max_rotation=\"3.141593\" logic_gate_type=\"0\" indicator_type=\"0\" connector_type=\"0\" ";
    s += "magnet_force=\"1\" gyro_type=\"0\" rudder_surface_area=\"1\" pump_pressure=\"0.01\" water_component_type=\"0\" ";
    s += "torque_component_type=\"0\" jet_engine_component_type=\"0\" particle_speed=\"0.05\" inventory_type=\"0\" inventory_default_outfit=\"0\" ";
    s += "electric_type=\"0\" electric_charge_capacity=\"0\" electric_magnitude=\"0\" composite_type=\"0\" camera_fov_min=\"1.22\" ";
    s += "camera_fov_max=\"0.5\" monitor_border=\"0.022\" monitor_inset=\"0.012\" rx_range=\"10\" rx_length=\"1\">\n";

    // Surfaces
    s += "\t<surfaces>\n";
    for (int x = xmin; x <= xmax; ++x) {
        for (int z = zmin; z <= zmax; ++z) {
            std::string pos = "<position x=\"" + std::to_string(x) + "\" y=\"0\" z=\"" + std::to_string(z) + "\"/>";
            s += "\t\t<surface orientation=\"3\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (x == xmin) s += "\t\t<surface orientation=\"1\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (x == xmax) s += "\t\t<surface orientation=\"0\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (z == zmin) s += "\t\t<surface orientation=\"5\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (z == zmax) s += "\t\t<surface orientation=\"4\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
        }
    }
    s += "\t</surfaces>\n";

    // Buoyancy Surfaces
    s += "\t<buoyancy_surfaces>\n";
    for (int x = xmin; x <= xmax; ++x) {
        for (int z = zmin; z <= zmax; ++z) {
            std::string pos = "<position x=\"" + std::to_string(x) + "\" y=\"0\" z=\"" + std::to_string(z) + "\"/>";
            s += "\t\t<surface orientation=\"3\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (x == xmin) s += "\t\t<surface orientation=\"1\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (x == xmax) s += "\t\t<surface orientation=\"0\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (z == zmin) s += "\t\t<surface orientation=\"5\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
            if (z == zmax) s += "\t\t<surface orientation=\"4\" rotation=\"0\" shape=\"1\" trans_type=\"0\">" + pos + "</surface>\n";
        }
    }
    s += "\t</buoyancy_surfaces>\n";

    // Logic Nodes
    s += "\t<logic_nodes>\n";
    s += "\t\t<logic_node label=\"Video Signal\" mode=\"1\" type=\"6\" description=\"\" flags=\"0\"><position x=\"0\" y=\"0\" z=\"0\"/></logic_node>\n";
    s += "\t\t<logic_node label=\"Touch Output\" mode=\"0\" type=\"5\" description=\"\" flags=\"0\"><position x=\"0\" y=\"0\" z=\"0\"/></logic_node>\n";
    s += "\t\t<logic_node label=\"Electric\" mode=\"1\" type=\"4\" description=\"\" flags=\"0\"><position x=\"0\" y=\"0\" z=\"0\"/></logic_node>\n";
    s += "\t\t<logic_node label=\"Power Switch\" mode=\"1\" type=\"0\" description=\"Controls whether or not the screen is switched on.\" flags=\"0\"><position x=\"0\" y=\"0\" z=\"0\"/></logic_node>\n";
    s += "\t</logic_nodes>\n";

    // Voxels
    s += "\t<voxels>\n";
    for (int x = xmin; x <= xmax; ++x) {
        for (int z = zmin; z <= zmax; ++z) {
            s += "\t\t<voxel flags=\"1\" physics_shape=\"0\" buoy_pipes=\"0\">\n";
            s += "\t\t\t<position x=\"" + std::to_string(x) + "\" y=\"0\" z=\"" + std::to_string(z) + "\"/>\n";
            s += "\t\t\t<physics_shape_rotation 00=\"1\" 01=\"0\" 02=\"0\" 10=\"0\" 11=\"1\" 12=\"0\" 20=\"0\" 21=\"0\" 22=\"1\"/>\n";
            s += "\t\t</voxel>\n";
        }
    }
    s += "\t</voxels>\n";

    // Bounds & properties
    s += "\t<voxel_min x=\"" + std::to_string(xmin) + "\" y=\"0\" z=\"" + std::to_string(zmin) + "\"/>\n";
    s += "\t<voxel_max x=\"" + std::to_string(xmax) + "\" y=\"0\" z=\"" + std::to_string(zmax) + "\"/>\n";
    s += "\t<voxel_physics_min x=\"" + std::to_string(xmin) + "\" y=\"0\" z=\"" + std::to_string(zmin) + "\"/>\n";
    s += "\t<voxel_physics_max x=\"" + std::to_string(xmax) + "\" y=\"0\" z=\"" + std::to_string(zmax) + "\"/>\n";
    s += "\t<bb_physics_min x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<bb_physics_max x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<compartment_sample_pos x=\"0\" y=\"0\" z=\"0\"/>\n";
    char cBuf[128];
    sprintf_s(cBuf, "%.3f", cx);
    std::string cxStr = cBuf;
    sprintf_s(cBuf, "%.3f", cz);
    std::string czStr = cBuf;
    s += "\t<constraint_pos_parent x=\"" + cxStr + "\" y=\"0\" z=\"" + czStr + "\"/>\n";
    s += "\t<constraint_pos_child x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<voxel_location_child x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<seat_offset x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<seat_front x=\"0\" y=\"0\" z=\"1\"/>\n";
    s += "\t<seat_up x=\"0\" y=\"1\" z=\"0\"/>\n";
    s += "\t<seat_camera x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<seat_render x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<force_dir x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<light_position x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<light_color x=\"1\" y=\"1\" z=\"1\"/>\n";
    s += "\t<light_forward x=\"0\" y=\"1\" z=\"0\"/>\n";
    s += "\t<door_size x=\"1\" y=\"1\" z=\"1\"/>\n";
    s += "\t<door_normal x=\"0\" y=\"0\" z=\"1\"/>\n";
    s += "\t<door_side x=\"1\" y=\"0\" z=\"0\"/>\n";
    s += "\t<door_up x=\"0\" y=\"1\" z=\"0\"/>\n";
    s += "\t<door_base_pos x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<dynamic_body_position x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<dynamic_rotation_axes x=\"0\" y=\"1\" z=\"0\"/>\n";
    s += "\t<dynamic_side_axis x=\"1\" y=\"0\" z=\"0\"/>\n";
    s += "\t<magnet_offset x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<connector_axis x=\"0\" y=\"1\" z=\"0\"/>\n";
    s += "\t<connector_up x=\"1\" y=\"0\" z=\"0\"/>\n";
    s += "\t<tooltip_properties description=\"Displays video data.\" short_description=\"A video screen.\"/>\n";
    s += "\t<reward_properties number_rewarded=\"1\"/>\n";
    s += "\t<jet_engine_connections_prev/>\n";
    s += "\t<jet_engine_connections_next/>\n";
    s += "\t<particle_direction x=\"0\" y=\"1\" z=\"0\"/>\n";
    s += "\t<particle_offset x=\"0\" y=\"0\" z=\"0\"/>\n";
    s += "\t<particle_bounds x=\"0.2\" y=\"0.2\" z=\"0.2\"/>\n";
    s += "</definition>\n";

    return s;
}

static void EnsureDescriptorInRegistry(void* reg, const char* name, int w, int h) {
    if (!reg || !name) return;
    if (IsDescriptorInRegistry(reg, name)) return;

    uint8_t* uReg = reinterpret_cast<uint8_t*>(reg);
    auto growFn = reinterpret_cast<void (*)(void*, uint32_t)>(g_base + 0x8dd150);

    std::string xml = GenerateCustomDisplayXml(w, h, name);

    uint8_t* desc = new uint8_t[32];
    memset(desc, 0, 32);
    char* idStr = _strdup(name);
    uint32_t idLen = static_cast<uint32_t>(strlen(name));
    *reinterpret_cast<char**>(desc + 0x00) = idStr;
    *reinterpret_cast<uint32_t*>(desc + 0x08) = idLen;
    *reinterpret_cast<uint32_t*>(desc + 0x0c) = idLen + 1;
    *reinterpret_cast<char**>(desc + 0x10) = _strdup(xml.c_str());
    *reinterpret_cast<uint32_t*>(desc + 0x18) = static_cast<uint32_t>(xml.length());

    uint32_t xCap = *reinterpret_cast<uint32_t*>(uReg + 0xe8);
    int32_t xHead = *reinterpret_cast<int32_t*>(uReg + 0xec);
    uint32_t xCount = *reinterpret_cast<uint32_t*>(uReg + 0xf0);
    void** xDefs = *reinterpret_cast<void***>(uReg + 0xe0);
    if (xCount >= xCap && growFn) {
        growFn(uReg + 0xe0, xCap > 0 ? xCap * 2 : 1024);
        xDefs = *reinterpret_cast<void***>(uReg + 0xe0);
        xCap = *reinterpret_cast<uint32_t*>(uReg + 0xe8);
        xHead = *reinterpret_cast<int32_t*>(uReg + 0xec);
    }
    if (xCount < xCap && xDefs) {
        uint32_t xIdx = (xHead + xCount) % xCap;
        xDefs[xIdx] = desc;
        *reinterpret_cast<uint32_t*>(uReg + 0xf0) = xCount + 1;
    }
    LogInfo("CDVEH: Registered full descriptor for %s in XML registry %p (xmlLen=%zu, count=%u, cap=%u)",
            name, reg, xml.length(), xCount + 1, xCap);
}

static void InitNativeSurface(uint8_t* s, int x, int y, int z, int ori, int rot, bool isBuoyancy, uintptr_t base) {
    memset(s, 0, 0x130);
    *reinterpret_cast<int32_t*>(s + 0x00) = x;
    *reinterpret_cast<int32_t*>(s + 0x04) = y;
    *reinterpret_cast<int32_t*>(s + 0x08) = z;
    *reinterpret_cast<int32_t*>(s + 0x0c) = ori;
    *reinterpret_cast<int32_t*>(s + 0x10) = rot;
    *reinterpret_cast<int32_t*>(s + 0x38) = 1; // shape = 1 (square)
    *reinterpret_cast<int32_t*>(s + 0x3c) = 0; // trans_type = 0

    // Native bounds default 1.0f at +0x40 and +0xb8 (same as constructor 0x1402f7410)
    for (int offset : { 0x40, 0xb8 }) {
        uint8_t* p = s + offset;
        *reinterpret_cast<float*>(p + 0x30) = 1.0f;
        *reinterpret_cast<float*>(p + 0x44) = 1.0f;
        *reinterpret_cast<float*>(p + 0x58) = 1.0f;
        *reinterpret_cast<float*>(p + 0x6c) = 1.0f;
    }

    if (base) {
        using CalcRotMatrixFn = void (*)(int32_t ori, int32_t rot, int32_t* outMatrix);
        using ComputeSurfaceVerticesFn = void (*)(void* surface, int dummy, bool isBuoyancy);

        auto calcRotMatrix = reinterpret_cast<CalcRotMatrixFn>(base + 0x2f89a0);
        auto computeSurfaceVertices = reinterpret_cast<ComputeSurfaceVerticesFn>(base + 0x2f8850);

        calcRotMatrix(ori, rot, reinterpret_cast<int32_t*>(s + 0x14));
        computeSurfaceVertices(s, 0, isBuoyancy);
    }
}

void* GetOrCreateDynamicDisplayDefinitionInRegistry(void* reg, int w, int h, const char* targetName) {
    if (!reg || !targetName) return nullptr;

    void* baseMon = nullptr;
    struct CandInfo {
        const char* name;
        int cw;
        int ch;
    };
    static const CandInfo s_cands[] = {
        { "monitor_1", 1, 1 },
        { "monitor_1x2", 1, 2 },
        { "monitor_1x3", 1, 3 },
        { "monitor_2", 2, 2 },
        { "monitor_2x3", 2, 3 },
        { "monitor_3", 3, 3 },
        { "monitor_5", 5, 5 },
        { "monitor_9", 9, 9 },
    };
    const char* cand = "monitor_1";
    int bestScore = 999999;
    for (const auto& ci : s_cands) {
        int scoreDirect = std::abs(w - ci.cw) * 10 + std::abs(h - ci.ch) * 10;
        int scoreTrans = std::abs(w - ci.ch) * 10 + std::abs(h - ci.cw) * 10 + 1;
        int curScore = (scoreDirect < scoreTrans) ? scoreDirect : scoreTrans;
        if (ci.cw > w || ci.ch > h) {
            curScore += 100 * ((ci.cw > w ? ci.cw - w : 0) + (ci.ch > h ? ci.ch - h : 0));
        }
        if (curScore < bestScore) {
            bestScore = curScore;
            cand = ci.name;
        }
    }

    if (cand) baseMon = FindComponentDefinitionInRegistry(reg, cand);
    if (!baseMon) baseMon = FindComponentDefinitionInRegistry(reg, "monitor_2");
    if (!baseMon) baseMon = FindComponentDefinitionInRegistry(reg, "monitor_1");
    if (!baseMon && g_editorState) baseMon = FindComponentDefinition(g_editorState, "monitor_1");
    if (!baseMon) return nullptr;

    CustomDisplayGpuPair customGpuPair = GetOrCreateCustomDisplayGpuPair(w, h, baseMon);

    void* existing = FindComponentDefinitionInRegistry(reg, targetName);
    if (existing) {
        EnsureVariantsInRegistry(reg, existing, targetName);
        EnsureDescriptorInRegistry(reg, targetName, w, h);
        ApplyCustomDisplayGpuPairToVariants(existing, customGpuPair);
        return existing;
    }

    uint8_t* cloneMem = new uint8_t[8 * 0x2848];
    memset(cloneMem, 0, 8 * 0x2848);

    for (int k = 0; k < 8; ++k) {
        void* srcVar = reinterpret_cast<void**>(baseMon)[k];
        uint8_t* dstVar = cloneMem + k * 0x2848;
        if (srcVar) {
            memcpy(dstVar, srcVar, 0x2848);
        } else {
            memcpy(dstVar, baseMon, 0x2848);
        }
        for (int j = 0; j < 8; ++j) {
            reinterpret_cast<void**>(dstVar)[j] = cloneMem + j * 0x2848;
        }
    }

    int xmin = -(w / 2);
    int xmax = (w - 1) / 2;
    int zmin = -((h - 1) / 2);
    int zmax = h / 2;
    uint32_t voxelCount = static_cast<uint32_t>(w * h);

    std::vector<VoxelEntry> baseVoxels(voxelCount);
    uint32_t vIdx = 0;
    for (int x = xmin; x <= xmax; ++x) {
        for (int z = zmin; z <= zmax; ++z) {
            VoxelEntry& v = baseVoxels[vIdx++];
            v.x = x;
            v.y = 0;
            v.z = z;
            v.flags = 1;
            v.physics_shape = 0;
            v.rot[0] = 1; v.rot[1] = 0; v.rot[2] = 0;
            v.rot[3] = 0; v.rot[4] = 1; v.rot[5] = 0;
            v.rot[6] = 0; v.rot[7] = 0; v.rot[8] = 1;
            v.buoy_pipes = 0;
        }
    }

    uint32_t totalSurfaces = voxelCount + 2 * w + 2 * h;
    struct SurfCoord {
        int ori;
        int x;
        int z;
    };
    std::vector<SurfCoord> surfCoords;
    surfCoords.reserve(totalSurfaces);

    for (int x = xmin; x <= xmax; ++x) {
        for (int z = zmin; z <= zmax; ++z) {
            surfCoords.push_back({3, x, z}); // Back
            if (x == xmin) surfCoords.push_back({1, x, z}); // Left
            if (x == xmax) surfCoords.push_back({0, x, z}); // Right
            if (z == zmin) surfCoords.push_back({5, x, z}); // Bottom
            if (z == zmax) surfCoords.push_back({4, x, z}); // Top
        }
    }

    char* idStr = _strdup(targetName);
    std::string displayName = "Custom Display " + std::to_string(w) + "x" + std::to_string(h);
    char* nameStr = _strdup(displayName.c_str());

    char meshNameBuf[64], arrowNameBuf[64];
    sprintf_s(meshNameBuf, "meshes/swml_cd_%dx%d.mesh", w, h);
    sprintf_s(arrowNameBuf, "meshes/swml_cd_%dx%d_arrow.mesh", w, h);
    char* meshNameStr = _strdup(meshNameBuf);
    char* arrowNameStr = _strdup(arrowNameBuf);

    for (int k = 0; k < 8; ++k) {
        uint8_t* dstVar = cloneMem + k * 0x2848;

        *reinterpret_cast<const char**>(dstVar + 0x288) = idStr;
        *reinterpret_cast<uint32_t*>(dstVar + 0x290) = static_cast<uint32_t>(strlen(targetName));
        *reinterpret_cast<uint32_t*>(dstVar + 0x294) = static_cast<uint32_t>(strlen(targetName) + 1);

        *reinterpret_cast<const char**>(dstVar + 0x298) = nameStr;
        *reinterpret_cast<uint32_t*>(dstVar + 0x2a0) = static_cast<uint32_t>(displayName.length());
        *reinterpret_cast<uint32_t*>(dstVar + 0x2a4) = static_cast<uint32_t>(displayName.length() + 1);

        int sx = (k & 1) ? -1 : 1;
        int sy = (k & 2) ? -1 : 1;
        int sz = (k & 4) ? -1 : 1;

        VoxelEntry* vArr = new VoxelEntry[voxelCount];
        for (uint32_t i = 0; i < voxelCount; ++i) {
            VoxelEntry v = baseVoxels[i];
            v.x *= sx;
            v.z *= sz;
            v.rot[0] *= sx;
            v.rot[4] *= sy;
            v.rot[8] *= sz;
            vArr[i] = v;
        }
        DefRingBuffer* vRb = reinterpret_cast<DefRingBuffer*>(dstVar + 0xa8);
        vRb->data = vArr;
        vRb->cap = voxelCount;
        vRb->head = 0;
        vRb->count = voxelCount;
        vRb->pad = 0;

        void** sPtrArray = new void*[totalSurfaces];
        for (uint32_t i = 0; i < totalSurfaces; ++i) {
            uint8_t* s = new uint8_t[0x130];
            const auto& c = surfCoords[i];
            int ori = c.ori;
            if (c.ori == 0) ori = (sx == -1) ? 1 : 0;
            else if (c.ori == 1) ori = (sx == -1) ? 0 : 1;
            else if (c.ori == 3) ori = (sy == -1) ? 2 : 3;
            else if (c.ori == 4) ori = (sz == -1) ? 5 : 4;
            else if (c.ori == 5) ori = (sz == -1) ? 4 : 5;

            int x = c.x * sx;
            int z = c.z * sz;

            InitNativeSurface(s, x, 0, z, ori, 0, false, g_base);
            sPtrArray[i] = s;
        }
        DefRingBuffer* sRb = reinterpret_cast<DefRingBuffer*>(dstVar + 0x48);
        sRb->data = sPtrArray;
        sRb->cap = totalSurfaces;
        sRb->head = 0;
        sRb->count = totalSurfaces;
        sRb->pad = 0;

        void** bPtrArray = new void*[totalSurfaces];
        for (uint32_t i = 0; i < totalSurfaces; ++i) {
            uint8_t* b = new uint8_t[0x130];
            const auto& c = surfCoords[i];
            int ori = c.ori;
            if (c.ori == 0) ori = (sx == -1) ? 1 : 0;
            else if (c.ori == 1) ori = (sx == -1) ? 0 : 1;
            else if (c.ori == 3) ori = (sy == -1) ? 2 : 3;
            else if (c.ori == 4) ori = (sz == -1) ? 5 : 4;
            else if (c.ori == 5) ori = (sz == -1) ? 4 : 5;

            int x = c.x * sx;
            int z = c.z * sz;

            InitNativeSurface(b, x, 0, z, ori, 0, true, g_base);
            bPtrArray[i] = b;
        }
        DefRingBuffer* bRb = reinterpret_cast<DefRingBuffer*>(dstVar + 0x60);
        bRb->data = bPtrArray;
        bRb->cap = totalSurfaces;
        bRb->head = 0;
        bRb->count = totalSurfaces;
        bRb->pad = 0;

        *reinterpret_cast<uint8_t*>(dstVar + 0x40) = static_cast<uint8_t>(k);
        *reinterpret_cast<int32_t*>(dstVar + 0x2a8) = 6;
        *reinterpret_cast<float*>(dstVar + 0x2b0) = 4.0f * w * h;
        *reinterpret_cast<int32_t*>(dstVar + 0x2b4) = 500 * w * h;
        *reinterpret_cast<uint64_t*>(dstVar + 0x2b8) = 0; // Flags: ALWAYS 0. Bit 0x80 (128) means IS_MULTIBODY_CHILD and causes crash on component deletion!

        int vkXmin = (sx == 1) ? xmin : -xmax;
        int vkXmax = (sx == 1) ? xmax : -xmin;
        int vkZmin = (sz == 1) ? zmin : -zmax;
        int vkZmax = (sz == 1) ? zmax : -zmin;

        int* vmin = reinterpret_cast<int*>(dstVar + 0xc0);
        int* vmax = reinterpret_cast<int*>(dstVar + 0xcc);
        int* vpmin = reinterpret_cast<int*>(dstVar + 0xd8);
        int* vpmax = reinterpret_cast<int*>(dstVar + 0xe4);
        vmin[0] = vkXmin; vmin[1] = 0; vmin[2] = vkZmin;
        vmax[0] = vkXmax; vmax[1] = 0; vmax[2] = vkZmax;
        vpmin[0] = vkXmin; vpmin[1] = 0; vpmin[2] = vkZmin;
        vpmax[0] = vkXmax; vpmax[1] = 0; vpmax[2] = vkZmax;

        float cx = (vkXmin + vkXmax) * 0.125f;
        float cz = (vkZmin + vkZmax) * 0.125f;
        float* com = reinterpret_cast<float*>(dstVar + 0x110);
        com[0] = cx; com[1] = 0.0f; com[2] = cz;
        float* constraint = reinterpret_cast<float*>(dstVar + 0x340);
        constraint[0] = cx; constraint[1] = 0.0f; constraint[2] = cz;

        // Ensure MeshResource (0x3a0..0x760, 6 * 0xa0 = 0x3c0 bytes) has valid GPU VBO/IBO from base monitor
        void* srcVar = reinterpret_cast<void**>(baseMon)[k];
        if (!srcVar) srcVar = baseMon;
        memcpy(dstVar + 0x3a0, reinterpret_cast<uint8_t*>(srcVar) + 0x3a0, 0x3c0);

        *reinterpret_cast<const char**>(dstVar + 0x760) = meshNameStr;
        *reinterpret_cast<uint32_t*>(dstVar + 0x768) = static_cast<uint32_t>(strlen(meshNameStr));
        *reinterpret_cast<uint32_t*>(dstVar + 0x76c) = static_cast<uint32_t>(strlen(meshNameStr) + 1);

        *reinterpret_cast<const char**>(dstVar + 0x790) = arrowNameStr;
        *reinterpret_cast<uint32_t*>(dstVar + 0x798) = static_cast<uint32_t>(strlen(arrowNameStr));
        *reinterpret_cast<uint32_t*>(dstVar + 0x79c) = static_cast<uint32_t>(strlen(arrowNameStr) + 1);
    }

    // Apply custom GPU mesh (VBO/IBO and CPU buffers) to all 8 variants
    ApplyCustomDisplayGpuPairToVariants(cloneMem, customGpuPair);

    // 1. Insert all 8 variants into variant ring buffer (reg + 0xc8)
    EnsureVariantsInRegistry(reg, cloneMem, targetName);

    // 2. Insert descriptor into XML ring buffer (reg + 0xe0)
    EnsureDescriptorInRegistry(reg, targetName, w, h);

    // 3. Insert into global ring buffer (reg + 0xf8) ONLY for swml_custom_display
    // Dynamic displays (swml_cd_WxH) must NOT be inserted into reg + 0xf8 so they do not appear in the workbench catalogue!
    if (strcmp(targetName, "swml_custom_display") == 0) {
        uint8_t* uReg = reinterpret_cast<uint8_t*>(reg);
        uint32_t cap = *reinterpret_cast<uint32_t*>(uReg + 0x100);
        int32_t head = *reinterpret_cast<int32_t*>(uReg + 0x104);
        uint32_t count = *reinterpret_cast<uint32_t*>(uReg + 0x108);
        void** allDefs = *reinterpret_cast<void***>(uReg + 0xf8);
        if (count >= cap) {
            auto growFn = reinterpret_cast<void (*)(void*, uint32_t)>(g_base + 0x8dd150);
            if (growFn) {
                growFn(uReg + 0xf8, cap > 0 ? cap * 2 : 1024);
                allDefs = *reinterpret_cast<void***>(uReg + 0xf8);
                cap = *reinterpret_cast<uint32_t*>(uReg + 0x100);
                head = *reinterpret_cast<int32_t*>(uReg + 0x104);
            }
        }
        if (count < cap && allDefs) {
            uint32_t insertIdx = (head + count) % cap;
            allDefs[insertIdx] = cloneMem;
            *reinterpret_cast<uint32_t*>(uReg + 0x108) = count + 1;
            LogInfo("CDVEH: Registered swml_custom_display in global registry %p at index %u (count=%u, cap=%u)",
                    reg, insertIdx, count + 1, cap);
        }
    } else {
        LogInfo("CDVEH: Registered dynamic display %s (%dx%d, %u voxels, %u surfaces) in registry %p (variant & XML buffers only; excluded from catalogue)",
                targetName, w, h, voxelCount, totalSurfaces, reg);
    }

    return cloneMem;
}

void* GetOrCreateDynamicDisplayDefinition(void* self, int w, int h, const char* targetName) {
    if (!self) return nullptr;
    void* world = nullptr;
    if (!Pointer(self, 0x70, world) || !world) return nullptr;
    void* reg = reinterpret_cast<uint8_t*>(world) + 0xbb670;
    return GetOrCreateDynamicDisplayDefinitionInRegistry(reg, w, h, targetName);
}

#pragma pack(push, 1)
struct SwEngineString {
    const char* data;
    uint32_t length;
    uint32_t capacity;
};
#pragma pack(pop)

using FindComponentDefFn = void* (*)(void* registry, const SwEngineString* name);
FindComponentDefFn g_originalFindComponentDef = nullptr;
uint8_t* g_findDefTrampoline = nullptr;

void* HookedFindComponentDefinitionByName(void* registry, const SwEngineString* name) {
    if (!g_originalFindComponentDef) return nullptr;
    void* result = g_originalFindComponentDef(registry, name);
    if (result) return result;

    static thread_local bool s_reentrant = false;
    if (s_reentrant) return nullptr;
    s_reentrant = true;

    if (registry && name && name->data && name->length > 0 && name->length < 128) {
        char cleanName[128] = {};
        memcpy(cleanName, name->data, name->length);
        cleanName[name->length] = '\0';

        void* existing = FindComponentDefinitionInRegistry(registry, cleanName);
        if (existing) {
            s_reentrant = false;
            return existing;
        }

        if (strcmp(cleanName, "swml_custom_display") == 0) {
            void* def = GetOrCreateDynamicDisplayDefinitionInRegistry(registry, 1, 1, cleanName);
            if (def) {
                LogInfo("CDVEH: HookedFindDef dynamically populated %s for registry %p -> %p", cleanName, registry, def);
                s_reentrant = false;
                return def;
            }
        } else if (strncmp(cleanName, "swml_cd_", 8) == 0) {
            int w = 1, h = 1;
            if (sscanf_s(cleanName, "swml_cd_%dx%d", &w, &h) >= 2 && w >= 1 && h >= 1) {
                GetOrAllocateDisplayDefinitionId(w, h, w * 32, h * 32);
                void* def = GetOrCreateDynamicDisplayDefinitionInRegistry(registry, w, h, cleanName);
                if (def) {
                    LogInfo("CDVEH: HookedFindDef dynamically populated %s for registry %p -> %p", cleanName, registry, def);
                    s_reentrant = false;
                    return def;
                }
            }
        }
    }

    s_reentrant = false;
    return nullptr;
}

bool InstallFindDefHook(uint8_t* base) {
    if (g_findDefTrampoline) return true;
    uint8_t* target = base + 0x46f380;

    g_findDefTrampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_findDefTrampoline) {
        LogError("CDVEH: Failed to allocate trampoline for FindComponentDefinitionByName");
        return false;
    }

    // Copy original 16-byte prologue:
    // 48 89 54 24 10  mov [rsp+10], rdx
    // 48 89 4c 24 08  mov [rsp+8], rcx
    // 53              push rbx
    // 55              push rbp
    // 56              push rsi
    // 57              push rdi
    // 41 54           push r12
    memcpy(g_findDefTrampoline, target, 16);

    // Jump from trampoline back to target + 16:
    // FF 25 00 00 00 00 [target + 16]
    g_findDefTrampoline[16] = 0xff;
    g_findDefTrampoline[17] = 0x25;
    *reinterpret_cast<int32_t*>(g_findDefTrampoline + 18) = 0;
    *reinterpret_cast<uint64_t*>(g_findDefTrampoline + 22) = reinterpret_cast<uint64_t>(target + 16);

    g_originalFindComponentDef = reinterpret_cast<FindComponentDefFn>(g_findDefTrampoline);

    // Patch target with 14-byte jump to HookedFindComponentDefinitionByName + 2 NOPs:
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogError("CDVEH: VirtualProtect failed on 0x46f380");
        VirtualFree(g_findDefTrampoline, 0, MEM_RELEASE);
        g_findDefTrampoline = nullptr;
        return false;
    }

    target[0] = 0xff;
    target[1] = 0x25;
    *reinterpret_cast<int32_t*>(target + 2) = 0;
    *reinterpret_cast<uint64_t*>(target + 6) = reinterpret_cast<uint64_t>(&HookedFindComponentDefinitionByName);
    target[14] = 0x90;
    target[15] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, 16, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    FlushInstructionCache(GetCurrentProcess(), g_findDefTrampoline, 32);

    LogInfo("CDVEH: Hooked FindComponentDefinitionByName at %p (trampoline at %p)", target, g_findDefTrampoline);
    return true;
}

void UninstallFindDefHook(uint8_t* base) {
    if (!g_findDefTrampoline) return;
    uint8_t* target = base + 0x46f380;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(target, g_findDefTrampoline, 16);
        DWORD ignored = 0;
        VirtualProtect(target, 16, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 16);
    }
    VirtualFree(g_findDefTrampoline, 0, MEM_RELEASE);
    g_findDefTrampoline = nullptr;
    g_originalFindComponentDef = nullptr;
    LogInfo("CDVEH: Unhooked FindComponentDefinitionByName");
}

using FindDefDescriptorFn = void* (*)(void* registry, const SwEngineString* name);
FindDefDescriptorFn g_originalFindDefDescriptor = nullptr;
uint8_t* g_findDescriptorTrampoline = nullptr;

void* HookedFindDefinitionDescriptorByName(void* registry, const SwEngineString* name) {
    if (!g_originalFindDefDescriptor) return nullptr;
    void* result = g_originalFindDefDescriptor(registry, name);
    if (result) return result;

    static thread_local bool s_reentrantDesc = false;
    if (s_reentrantDesc) return nullptr;
    s_reentrantDesc = true;

    if (registry && name && name->data && name->length > 0 && name->length < 128) {
        char cleanName[128] = {};
        memcpy(cleanName, name->data, name->length);
        cleanName[name->length] = '\0';

        if (strcmp(cleanName, "swml_custom_display") == 0) {
            GetOrCreateDynamicDisplayDefinitionInRegistry(registry, 1, 1, cleanName);
            result = g_originalFindDefDescriptor(registry, name);
            if (result) {
                LogInfo("CDVEH: HookedFindDefDescriptor dynamically populated descriptor for %s in registry %p -> %p",
                        cleanName, registry, result);
                s_reentrantDesc = false;
                return result;
            }
        } else if (strncmp(cleanName, "swml_cd_", 8) == 0) {
            int w = 1, h = 1;
            if (sscanf_s(cleanName, "swml_cd_%dx%d", &w, &h) >= 2 && w >= 1 && h >= 1) {
                GetOrAllocateDisplayDefinitionId(w, h, w * 32, h * 32);
                GetOrCreateDynamicDisplayDefinitionInRegistry(registry, w, h, cleanName);
                result = g_originalFindDefDescriptor(registry, name);
                if (result) {
                    LogInfo("CDVEH: HookedFindDefDescriptor dynamically populated descriptor for %s in registry %p -> %p",
                            cleanName, registry, result);
                    s_reentrantDesc = false;
                    return result;
                }
            }
        }
    }

    s_reentrantDesc = false;
    return nullptr;
}

bool InstallFindDescriptorHook(uint8_t* base) {
    if (g_findDescriptorTrampoline) return true;
    uint8_t* target = base + 0x46f2f0;

    g_findDescriptorTrampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_findDescriptorTrampoline) {
        LogError("CDVEH: Failed to allocate trampoline for FindDefinitionDescriptorByName");
        return false;
    }

    memcpy(g_findDescriptorTrampoline, target, 17);

    g_findDescriptorTrampoline[17] = 0xff;
    g_findDescriptorTrampoline[18] = 0x25;
    *reinterpret_cast<int32_t*>(g_findDescriptorTrampoline + 19) = 0;
    *reinterpret_cast<uint64_t*>(g_findDescriptorTrampoline + 23) = reinterpret_cast<uint64_t>(target + 17);

    g_originalFindDefDescriptor = reinterpret_cast<FindDefDescriptorFn>(g_findDescriptorTrampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 17, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogError("CDVEH: VirtualProtect failed on 0x46f2f0");
        VirtualFree(g_findDescriptorTrampoline, 0, MEM_RELEASE);
        g_findDescriptorTrampoline = nullptr;
        return false;
    }

    target[0] = 0xff;
    target[1] = 0x25;
    *reinterpret_cast<int32_t*>(target + 2) = 0;
    *reinterpret_cast<uint64_t*>(target + 6) = reinterpret_cast<uint64_t>(&HookedFindDefinitionDescriptorByName);
    target[14] = 0x90;
    target[15] = 0x90;
    target[16] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, 17, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 17);
    FlushInstructionCache(GetCurrentProcess(), g_findDescriptorTrampoline, 32);

    LogInfo("CDVEH: Hooked FindDefinitionDescriptorByName at %p (trampoline at %p)", target, g_findDescriptorTrampoline);
    return true;
}

void UninstallFindDescriptorHook(uint8_t* base) {
    if (!g_findDescriptorTrampoline) return;
    uint8_t* target = base + 0x46f2f0;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 17, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(target, g_findDescriptorTrampoline, 17);
        DWORD ignored = 0;
        VirtualProtect(target, 17, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 17);
    }
    VirtualFree(g_findDescriptorTrampoline, 0, MEM_RELEASE);
    g_findDescriptorTrampoline = nullptr;
    g_originalFindDefDescriptor = nullptr;
    LogInfo("CDVEH: Unhooked FindDefinitionDescriptorByName");
}

using DeserializeVehicleComponentsFn = void (*)(void* body, void** registryPtr, void* xmlReader, void* outComponents, int bodyIndex);
DeserializeVehicleComponentsFn g_originalDeserializeVehicleComponents = nullptr;
uint8_t* g_deserializeVehicleComponentsTrampoline = nullptr;

static bool IsComponentRegistry(void* ptr) {
    if (!ptr) return false;
    uint8_t* u = reinterpret_cast<uint8_t*>(ptr);
    void** vBuf = nullptr;
    uint32_t vCap = 0, vCount = 0;
    void** aBuf = nullptr;
    uint32_t aCap = 0, aCount = 0;
    SIZE_T read = 0;

    if (!ReadProcessMemory(GetCurrentProcess(), u + 0xc8, &vBuf, sizeof(vBuf), &read) || !vBuf)
        return false;
    if (!ReadProcessMemory(GetCurrentProcess(), u + 0xd0, &vCap, sizeof(vCap), &read) || vCap < 100 || vCap > 500000)
        return false;
    if (!ReadProcessMemory(GetCurrentProcess(), u + 0xd8, &vCount, sizeof(vCount), &read) || vCount == 0 || vCount > vCap)
        return false;

    if (!ReadProcessMemory(GetCurrentProcess(), u + 0xf8, &aBuf, sizeof(aBuf), &read) || !aBuf)
        return false;
    if (!ReadProcessMemory(GetCurrentProcess(), u + 0x100, &aCap, sizeof(aCap), &read) || aCap < 100 || aCap > 500000)
        return false;
    if (!ReadProcessMemory(GetCurrentProcess(), u + 0x108, &aCount, sizeof(aCount), &read) || aCount == 0 || aCount > aCap)
        return false;

    return true;
}

static void PopulateRegistryWithCustomDisplays(void* reg) {
    if (!IsComponentRegistry(reg)) return;
    auto displays = GetAllRegisteredDisplays();
    LogInfo("CDVEH: PopulateRegistryWithCustomDisplays injecting %zu displays into registry %p", displays.size(), reg);
    for (const auto& entry : displays) {
        const std::string& defId = entry.first;
        int w = 1, h = 1;
        if (sscanf_s(defId.c_str(), "swml_cd_%dx%d", &w, &h) >= 2 && w >= 1 && h >= 1) {
            GetOrCreateDynamicDisplayDefinitionInRegistry(reg, w, h, defId.c_str());
        }
    }
    GetOrCreateDynamicDisplayDefinitionInRegistry(reg, 1, 1, "swml_custom_display");
}

void HookedDeserializeVehicleComponents(void* body, void** registryPtr, void* xmlReader, void* outComponents, int bodyIndex) {
    if (!g_originalDeserializeVehicleComponents) return;

    if (registryPtr) {
        if (IsComponentRegistry(registryPtr)) {
            PopulateRegistryWithCustomDisplays(registryPtr);
        } else {
            void* r0 = nullptr;
            if (Pointer(registryPtr, 0x00, r0) && IsComponentRegistry(r0)) {
                PopulateRegistryWithCustomDisplays(r0);
            }
            void* r10 = nullptr;
            if (Pointer(registryPtr, 0x10, r10) && IsComponentRegistry(r10)) {
                PopulateRegistryWithCustomDisplays(r10);
            }
        }
    }

    if (body) {
        void* world = nullptr;
        if (Pointer(body, 0x70, world) && world) {
            void* worldReg = reinterpret_cast<uint8_t*>(world) + 0xbb670;
            if (IsComponentRegistry(worldReg)) {
                PopulateRegistryWithCustomDisplays(worldReg);
            }
        }
    }

    if (g_editorState) {
        void* edWorld = nullptr;
        if (Pointer(g_editorState, 0x70, edWorld) && edWorld) {
            void* edReg = reinterpret_cast<uint8_t*>(edWorld) + 0xbb670;
            if (IsComponentRegistry(edReg)) {
                PopulateRegistryWithCustomDisplays(edReg);
            }
        }
    }

    g_originalDeserializeVehicleComponents(body, registryPtr, xmlReader, outComponents, bodyIndex);
}

bool InstallDeserializeVehicleComponentsHook(uint8_t* base) {
    if (g_deserializeVehicleComponentsTrampoline) return true;
    uint8_t* target = base + 0x4b8a50;

    g_deserializeVehicleComponentsTrampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_deserializeVehicleComponentsTrampoline) {
        LogError("CDVEH: Failed to allocate trampoline for DeserializeVehicleComponents");
        return false;
    }

    // Copy original 20 bytes:
    // 48 89 5c 24 20  mov [rsp+20], rbx
    // 4c 89 44 24 18  mov [rsp+18], r8
    // 48 89 54 24 10  mov [rsp+10], rdx
    // 48 89 4c 24 08  mov [rsp+8], rcx
    memcpy(g_deserializeVehicleComponentsTrampoline, target, 20);

    // Jump from trampoline back to target + 20:
    g_deserializeVehicleComponentsTrampoline[20] = 0xff;
    g_deserializeVehicleComponentsTrampoline[21] = 0x25;
    *reinterpret_cast<int32_t*>(g_deserializeVehicleComponentsTrampoline + 22) = 0;
    *reinterpret_cast<uint64_t*>(g_deserializeVehicleComponentsTrampoline + 26) = reinterpret_cast<uint64_t>(target + 20);

    g_originalDeserializeVehicleComponents = reinterpret_cast<DeserializeVehicleComponentsFn>(g_deserializeVehicleComponentsTrampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 20, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogError("CDVEH: VirtualProtect failed on 0x4b8a50");
        VirtualFree(g_deserializeVehicleComponentsTrampoline, 0, MEM_RELEASE);
        g_deserializeVehicleComponentsTrampoline = nullptr;
        return false;
    }

    target[0] = 0xff;
    target[1] = 0x25;
    *reinterpret_cast<int32_t*>(target + 2) = 0;
    *reinterpret_cast<uint64_t*>(target + 6) = reinterpret_cast<uint64_t>(&HookedDeserializeVehicleComponents);
    for (int i = 14; i < 20; ++i) {
        target[i] = 0x90;
    }

    DWORD ignored = 0;
    VirtualProtect(target, 20, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 20);
    FlushInstructionCache(GetCurrentProcess(), g_deserializeVehicleComponentsTrampoline, 36);

    LogInfo("CDVEH: Hooked DeserializeVehicleComponents at %p (trampoline at %p)", target, g_deserializeVehicleComponentsTrampoline);
    return true;
}

void UninstallDeserializeVehicleComponentsHook(uint8_t* base) {
    if (!g_deserializeVehicleComponentsTrampoline) return;
    uint8_t* target = base + 0x4b8a50;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 20, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(target, g_deserializeVehicleComponentsTrampoline, 20);
        DWORD ignored = 0;
        VirtualProtect(target, 20, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 20);
    }
    VirtualFree(g_deserializeVehicleComponentsTrampoline, 0, MEM_RELEASE);
    g_deserializeVehicleComponentsTrampoline = nullptr;
    g_originalDeserializeVehicleComponents = nullptr;
    LogInfo("CDVEH: Unhooked DeserializeVehicleComponents");
}

using RegistryClearFn = void (*)(void* registry);
RegistryClearFn g_originalRegistryClear = nullptr;
uint8_t* g_registryClearTrampoline = nullptr;

static bool SafeCheckModId(const void* ptr, size_t offset) {
    __try {
        if (!ptr) return false;
        const char* str = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(ptr) + offset);
        if (str && strncmp(str, "swml_", 5) == 0) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static char* SafeGetXmlData(void* desc) {
    __try {
        return *reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(desc) + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static char* SafeGetIdStr(void* desc) {
    __try {
        return *reinterpret_cast<char**>(desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

void HookedComponentRegistryClear(void* registry) {
    if (!registry || !g_originalRegistryClear) return;

    static thread_local bool s_inClear = false;
    if (s_inClear) {
        g_originalRegistryClear(registry);
        return;
    }
    s_inClear = true;

    uint8_t* uReg = reinterpret_cast<uint8_t*>(registry);

    // 1. Clean XML descriptors (reg + 0xe0)
    void** xBuffer = *reinterpret_cast<void***>(uReg + 0xe0);
    uint32_t xCap = *reinterpret_cast<uint32_t*>(uReg + 0xe8);
    int32_t xHead = *reinterpret_cast<int32_t*>(uReg + 0xec);
    uint32_t xCount = *reinterpret_cast<uint32_t*>(uReg + 0xf0);

    if (xBuffer && xCap > 0 && xCount > 0 && xCount <= 100000) {
        std::vector<void*> kept;
        for (uint32_t i = 0; i < xCount; ++i) {
            uint32_t idx = (xHead + i) % xCap;
            void* desc = xBuffer[idx];
            if (!desc) continue;
            if (SafeCheckModId(desc, 0)) {
                char* xml = SafeGetXmlData(desc);
                char* id = SafeGetIdStr(desc);
                if (xml) free(xml);
                if (id) free(id);
                delete[] reinterpret_cast<uint8_t*>(desc);
            } else {
                kept.push_back(desc);
            }
        }
        for (size_t i = 0; i < kept.size(); ++i) {
            xBuffer[i] = kept[i];
        }
        *reinterpret_cast<int32_t*>(uReg + 0xec) = 0;
        *reinterpret_cast<uint32_t*>(uReg + 0xf0) = static_cast<uint32_t>(kept.size());
        LogInfo("CDVEH: HookedRegistryClear cleaned XML descriptors (kept=%zu, prev=%u)", kept.size(), xCount);
    }

    // 2. Clean variant definitions (reg + 0xc8)
    void** vBuffer = *reinterpret_cast<void***>(uReg + 0xc8);
    uint32_t vCap = *reinterpret_cast<uint32_t*>(uReg + 0xd0);
    int32_t vHead = *reinterpret_cast<int32_t*>(uReg + 0xd4);
    uint32_t vCount = *reinterpret_cast<uint32_t*>(uReg + 0xd8);

    if (vBuffer && vCap > 0 && vCount > 0 && vCount <= 100000) {
        std::vector<void*> kept;
        for (uint32_t i = 0; i < vCount; ++i) {
            uint32_t idx = (vHead + i) % vCap;
            void* var = vBuffer[idx];
            if (!var) continue;
            if (SafeCheckModId(var, 0x288)) {
                // Mod variant - do NOT let engine destruct/free with _aligned_free
            } else {
                kept.push_back(var);
            }
        }
        for (size_t i = 0; i < kept.size(); ++i) {
            vBuffer[i] = kept[i];
        }
        *reinterpret_cast<int32_t*>(uReg + 0xd4) = 0;
        *reinterpret_cast<uint32_t*>(uReg + 0xd8) = static_cast<uint32_t>(kept.size());
        LogInfo("CDVEH: HookedRegistryClear cleaned variants (kept=%zu, prev=%u)", kept.size(), vCount);
    }

    // 3. Clean global definitions (reg + 0xf8)
    void** gBuffer = *reinterpret_cast<void***>(uReg + 0xf8);
    uint32_t gCap = *reinterpret_cast<uint32_t*>(uReg + 0x100);
    int32_t gHead = *reinterpret_cast<int32_t*>(uReg + 0x104);
    uint32_t gCount = *reinterpret_cast<uint32_t*>(uReg + 0x108);

    if (gBuffer && gCap > 0 && gCount > 0 && gCount <= 100000) {
        std::vector<void*> kept;
        for (uint32_t i = 0; i < gCount; ++i) {
            uint32_t idx = (gHead + i) % gCap;
            void* def = gBuffer[idx];
            if (!def) continue;
            if (SafeCheckModId(def, 0x288)) {
                // Mod global def
            } else {
                kept.push_back(def);
            }
        }
        for (size_t i = 0; i < kept.size(); ++i) {
            gBuffer[i] = kept[i];
        }
        *reinterpret_cast<int32_t*>(uReg + 0x104) = 0;
        *reinterpret_cast<uint32_t*>(uReg + 0x108) = static_cast<uint32_t>(kept.size());
        LogInfo("CDVEH: HookedRegistryClear cleaned global definitions (kept=%zu, prev=%u)", kept.size(), gCount);
    }

    g_originalRegistryClear(registry);
    s_inClear = false;
}

bool InstallRegistryClearHook(uint8_t* base) {
    if (g_registryClearTrampoline) return true;
    uint8_t* target = base + 0x5f1fd0;

    g_registryClearTrampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_registryClearTrampoline) {
        LogError("CDVEH: Failed to allocate trampoline for ComponentRegistry::Clear");
        return false;
    }

    memcpy(g_registryClearTrampoline, target, 15);

    g_registryClearTrampoline[15] = 0xff;
    g_registryClearTrampoline[16] = 0x25;
    *reinterpret_cast<int32_t*>(g_registryClearTrampoline + 17) = 0;
    *reinterpret_cast<uint64_t*>(g_registryClearTrampoline + 21) = reinterpret_cast<uint64_t>(target + 15);

    g_originalRegistryClear = reinterpret_cast<RegistryClearFn>(g_registryClearTrampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogError("CDVEH: VirtualProtect failed on 0x5f1fd0");
        VirtualFree(g_registryClearTrampoline, 0, MEM_RELEASE);
        g_registryClearTrampoline = nullptr;
        return false;
    }

    target[0] = 0xff;
    target[1] = 0x25;
    *reinterpret_cast<int32_t*>(target + 2) = 0;
    *reinterpret_cast<uint64_t*>(target + 6) = reinterpret_cast<uint64_t>(&HookedComponentRegistryClear);
    target[14] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, 15, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 15);
    FlushInstructionCache(GetCurrentProcess(), g_registryClearTrampoline, 32);

    LogInfo("CDVEH: Hooked ComponentRegistry::Clear at %p (trampoline at %p)", target, g_registryClearTrampoline);
    return true;
}

void UninstallRegistryClearHook(uint8_t* base) {
    if (!g_registryClearTrampoline) return;
    uint8_t* target = base + 0x5f1fd0;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(target, g_registryClearTrampoline, 15);
        DWORD ignored = 0;
        VirtualProtect(target, 15, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 15);
    }
    VirtualFree(g_registryClearTrampoline, 0, MEM_RELEASE);
    g_registryClearTrampoline = nullptr;
    g_originalRegistryClear = nullptr;
    LogInfo("CDVEH: Unhooked ComponentRegistry::Clear");
}

using TranslateStringFn = sw::EStr* (*)(sw::EStr* out, const sw::EStr* key);
static TranslateStringFn g_originalTranslateString = nullptr;
static uint8_t* g_translateStringTrampoline = nullptr;

bool TryTranslateCustomDisplayKey(const char* keyData, size_t keyLen, std::string& outText) {
    if (!keyData || keyLen == 0 || keyLen > 256) return false;

    // Fast check: must contain "swml" or "custom_display"
    std::string k;
    k.reserve(keyLen);
    for (size_t i = 0; i < keyLen; ++i) {
        k.push_back(static_cast<char>(tolower(static_cast<unsigned char>(keyData[i]))));
    }

    if (k.find("swml") == std::string::npos && k.find("custom_display") == std::string::npos) {
        return false;
    }

    // Strip "def_" prefix if present
    const char* p = k.c_str();
    if (strncmp(p, "def_", 4) == 0) {
        p += 4;
    }

    // 1. Base custom display catalog item
    if (strcmp(p, "swml_custom_display_name") == 0 || strcmp(p, "swml_custom_display") == 0) {
        outText = "Custom Display";
        return true;
    }
    if (strcmp(p, "swml_custom_display_s_desc") == 0) {
        outText = "Custom Screen";
        return true;
    }
    if (strcmp(p, "swml_custom_display_desc") == 0) {
        outText = "Custom video screen with configurable physical size and pixel resolution.";
        return true;
    }
    int nodeIdx = -1;
    if (sscanf_s(p, "swml_custom_display_node_%d_label", &nodeIdx) == 1) {
        switch (nodeIdx) {
            case 0: outText = "Video Signal"; return true;
            case 1: outText = "Touch Output"; return true;
            case 2: outText = "Electric"; return true;
            case 3: outText = "Power Switch"; return true;
            default: {
                char buf[32];
                snprintf(buf, sizeof(buf), "Logic Node %d", nodeIdx);
                outText = buf;
                return true;
            }
        }
    }
    if (sscanf_s(p, "swml_custom_display_node_%d_desc", &nodeIdx) == 1) {
        switch (nodeIdx) {
            case 0: outText = "Composite video signal input."; return true;
            case 1: outText = "Composite touch data output."; return true;
            case 2: outText = "Electric power connection."; return true;
            case 3: outText = "Screen on/off toggle input."; return true;
            default: outText = "Logic signal."; return true;
        }
    }

    // 2. Dynamic display sizes: swml_cd_%dx%d[_s%d]...
    int w = 0, h = 0;
    int charsRead = 0;
    if (sscanf_s(p, "swml_cd_%dx%d%n", &w, &h, &charsRead) >= 2 && charsRead > 0) {
        const char* rest = p + charsRead;
        // Skip optional slot suffix like "_s2" or "_s3"
        int slot = 0;
        int slotChars = 0;
        if (sscanf_s(rest, "_s%d%n", &slot, &slotChars) >= 1 && slotChars > 0) {
            rest += slotChars;
        }

        if (*rest == '\0' || strcmp(rest, "_name") == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Custom Display %dx%d", w, h);
            outText = buf;
            return true;
        }
        if (strcmp(rest, "_s_desc") == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Custom Screen %dx%d", w, h);
            outText = buf;
            return true;
        }
        if (strcmp(rest, "_desc") == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Custom video screen %dx%d.", w, h);
            outText = buf;
            return true;
        }
        int cdNodeIdx = -1;
        if (sscanf_s(rest, "_node_%d_label", &cdNodeIdx) == 1) {
            switch (cdNodeIdx) {
                case 0: outText = "Video Signal"; return true;
                case 1: outText = "Touch Output"; return true;
                case 2: outText = "Electric"; return true;
                case 3: outText = "Power Switch"; return true;
                default: {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Logic Node %d", cdNodeIdx);
                    outText = buf;
                    return true;
                }
            }
        }
        if (sscanf_s(rest, "_node_%d_desc", &cdNodeIdx) == 1) {
            switch (cdNodeIdx) {
                case 0: outText = "Composite video signal input."; return true;
                case 1: outText = "Composite touch data output."; return true;
                case 2: outText = "Electric power connection."; return true;
                case 3: outText = "Screen on/off toggle input."; return true;
                default: outText = "Logic signal."; return true;
            }
        }
        // Fallback for any other suffix on swml_cd
        char buf[64];
        snprintf(buf, sizeof(buf), "Custom Display %dx%d", w, h);
        outText = buf;
        return true;
    }

    return false;
}

sw::EStr* HookedTranslateString(sw::EStr* out, const sw::EStr* key) {
    if (!out) return nullptr;
    if (key && key->data && key->len > 0) {
        std::string translated;
        if (TryTranslateCustomDisplayKey(key->data, key->len, translated)) {
            out->data = nullptr;
            out->len = static_cast<uint32_t>(translated.size());
            out->cap = 0;
            if (g_dialogApi.engine.str_reserve) {
                g_dialogApi.engine.str_reserve(out, out->len + 1);
            } else if (g_base) {
                auto reserveFn = reinterpret_cast<sw::fn_str_reserve_t>(reinterpret_cast<uint8_t*>(g_base) + 0x17cb00);
                reserveFn(out, out->len + 1);
            }
            if (out->data) {
                memcpy(out->data, translated.data(), translated.size());
                out->data[translated.size()] = '\0';
            }
            return out;
        }
    }
    return g_originalTranslateString(out, key);
}

bool InstallLocalizationHook(uint8_t* base) {
    if (g_translateStringTrampoline) return true;
    uint8_t* target = base + 0x20c720;

    g_translateStringTrampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_translateStringTrampoline) {
        LogError("CDVEH: Failed to allocate trampoline for TranslateString (0x20c720)");
        return false;
    }

    memcpy(g_translateStringTrampoline, target, 15);

    g_translateStringTrampoline[15] = 0xff;
    g_translateStringTrampoline[16] = 0x25;
    *reinterpret_cast<int32_t*>(g_translateStringTrampoline + 17) = 0;
    *reinterpret_cast<uint64_t*>(g_translateStringTrampoline + 21) = reinterpret_cast<uint64_t>(target + 15);

    g_originalTranslateString = reinterpret_cast<TranslateStringFn>(g_translateStringTrampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogError("CDVEH: VirtualProtect failed on 0x20c720");
        VirtualFree(g_translateStringTrampoline, 0, MEM_RELEASE);
        g_translateStringTrampoline = nullptr;
        return false;
    }

    target[0] = 0xff;
    target[1] = 0x25;
    *reinterpret_cast<int32_t*>(target + 2) = 0;
    *reinterpret_cast<uint64_t*>(target + 6) = reinterpret_cast<uint64_t>(&HookedTranslateString);
    target[14] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, 15, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 15);
    FlushInstructionCache(GetCurrentProcess(), g_translateStringTrampoline, 32);

    LogInfo("CDVEH: Hooked TranslateString at %p (trampoline at %p)", target, g_translateStringTrampoline);
    return true;
}

void UninstallLocalizationHook(uint8_t* base) {
    if (!g_translateStringTrampoline) return;
    uint8_t* target = base + 0x20c720;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(target, g_translateStringTrampoline, 15);
        DWORD ignored = 0;
        VirtualProtect(target, 15, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 15);
    }
    VirtualFree(g_translateStringTrampoline, 0, MEM_RELEASE);
    g_translateStringTrampoline = nullptr;
    g_originalTranslateString = nullptr;
    LogInfo("CDVEH: Unhooked TranslateString");
}

void PreloadSavedCustomDisplays(void* self) {
    if (!self) return;
    auto displays = GetAllRegisteredDisplays();
    LogInfo("CDVEH: PreloadSavedCustomDisplays checking %zu saved display(s)...", displays.size());
    for (const auto& entry : displays) {
        const std::string& defId = entry.first;
        int w = 1, h = 1;
        if (sscanf_s(defId.c_str(), "swml_cd_%dx%d", &w, &h) >= 2 && w >= 1 && h >= 1) {
            void* def = GetOrCreateDynamicDisplayDefinition(self, w, h, defId.c_str());
            LogInfo("CDVEH: Preloaded display %s (%dx%d) -> %p", defId.c_str(), w, h, def);
        }
    }
}

void InjectCustomDisplayCatalogueEntry(void* editor, void* ui) {
    if (!editor) return;

    void* world = nullptr;
    if (!Pointer(editor, 0x70, world) || !world) return;
    uint8_t* reg = reinterpret_cast<uint8_t*>(world) + 0xbb670;

    void* customDef = FindComponentDefinition(editor, "swml_custom_display");
    if (!customDef) {
        void* mon1 = FindComponentDefinition(editor, "monitor_1");
        if (!mon1) {
            return;
        }

        uint8_t* cloneMem = new uint8_t[8 * 0x2848];
        memset(cloneMem, 0, 8 * 0x2848);

        for (int k = 0; k < 8; ++k) {
            void* srcVar = reinterpret_cast<void**>(mon1)[k];
            uint8_t* dstVar = cloneMem + k * 0x2848;
            if (srcVar) {
                memcpy(dstVar, srcVar, 0x2848);
            } else {
                memcpy(dstVar, mon1, 0x2848);
            }
        }

        static const char s_id[] = "swml_custom_display";
        static const char s_name[] = "Custom Display";

        for (int k = 0; k < 8; ++k) {
            uint8_t* dstVar = cloneMem + k * 0x2848;
            for (int j = 0; j < 8; ++j) {
                reinterpret_cast<void**>(dstVar)[j] = cloneMem + j * 0x2848;
            }

            *reinterpret_cast<const char**>(dstVar + 0x288) = s_id;
            *reinterpret_cast<uint32_t*>(dstVar + 0x290) = sizeof(s_id) - 1;
            *reinterpret_cast<uint32_t*>(dstVar + 0x294) = sizeof(s_id);

            *reinterpret_cast<const char**>(dstVar + 0x298) = s_name;
            *reinterpret_cast<uint32_t*>(dstVar + 0x2a0) = sizeof(s_name) - 1;
            *reinterpret_cast<uint32_t*>(dstVar + 0x2a4) = sizeof(s_name);

            *reinterpret_cast<uint8_t*>(dstVar + 0x40) = static_cast<uint8_t>(k);
            *reinterpret_cast<int32_t*>(dstVar + 0x2a8) = 6; // Displays category
            *reinterpret_cast<uint64_t*>(dstVar + 0x2b8) = 0; // Flags: 0 = visible in catalogue
        }

        customDef = cloneMem;
        EnsureVariantsInRegistry(reg, customDef, s_id);
        EnsureDescriptorInRegistry(reg, s_id, 1, 1);

        // 1. Insert into global ring buffer (reg + 0xf8)
        uint32_t cap = *reinterpret_cast<uint32_t*>(reg + 0x100);
        int32_t head = *reinterpret_cast<int32_t*>(reg + 0x104);
        uint32_t count = *reinterpret_cast<uint32_t*>(reg + 0x108);
        void** allDefs = *reinterpret_cast<void***>(reg + 0xf8);
        if (count >= cap) {
            auto growFn = reinterpret_cast<void (*)(void*, uint32_t)>(g_base + 0x8dd150);
            if (growFn) {
                growFn(reg + 0xf8, cap > 0 ? cap * 2 : 1024);
                allDefs = *reinterpret_cast<void***>(reg + 0xf8);
                cap = *reinterpret_cast<uint32_t*>(reg + 0x100);
                head = *reinterpret_cast<int32_t*>(reg + 0x104);
            }
        }
        if (count < cap && allDefs) {
            uint32_t insertIdx = (head + count) % cap;
            allDefs[insertIdx] = customDef;
            *reinterpret_cast<uint32_t*>(reg + 0x108) = count + 1;
            LogInfo("CDVEH: Injected swml_custom_display into global registry at index %u (count=%u, cap=%u)", insertIdx, count + 1, cap);
        }

        // 2. Insert into Category 6 bucket (reg + 0x110 + 6 * 24)
        uint8_t* bucket = reg + 0x110 + 6 * 24;
        void** bBuf = *reinterpret_cast<void***>(bucket + 0);
        uint32_t bCap = *reinterpret_cast<uint32_t*>(bucket + 8);
        int32_t bHead = *reinterpret_cast<int32_t*>(bucket + 12);
        uint32_t bCount = *reinterpret_cast<uint32_t*>(bucket + 16);
        if (bCount >= bCap) {
            auto growFn = reinterpret_cast<void (*)(void*, uint32_t)>(g_base + 0x8dd150);
            if (growFn) {
                growFn(bucket, bCap > 0 ? bCap * 2 : 64);
                bBuf = *reinterpret_cast<void***>(bucket + 0);
                bCap = *reinterpret_cast<uint32_t*>(bucket + 8);
                bHead = *reinterpret_cast<int32_t*>(bucket + 12);
            }
        }
        if (bCount < bCap && bBuf) {
            uint32_t insertIdx = (bHead + bCount) % bCap;
            bBuf[insertIdx] = customDef;
            *reinterpret_cast<uint32_t*>(bucket + 16) = bCount + 1;
            LogInfo("CDVEH: Injected swml_custom_display into category 6 bucket at index %u (count=%u, cap=%u)", insertIdx, bCount + 1, bCap);
        }
    }

    // 3. Inject into active catalogue UI widget if open
    if (!ui) {
        Pointer(editor, 0x8a8, ui);
    }
    if (ui && customDef) {
        void* catalogue = nullptr;
        if (Pointer(ui, 0x150, catalogue) && catalogue) {
            int32_t cCount = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(catalogue) + 0x70);
            uint32_t cCap = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(catalogue) + 0x68);
            int32_t cHead = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(catalogue) + 0x6c);
            void** cBuf = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(catalogue) + 0x60);

            bool alreadyInCatalogue = false;
            if (cBuf && cCap > 0) {
                for (int32_t i = 0; i < cCount; ++i) {
                    void* item = cBuf[(cHead + i) % cCap];
                    if (item && *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(item) + 8) == customDef) {
                        alreadyInCatalogue = true;
                        break;
                    }
                }
            }

            if (!alreadyInCatalogue) {
                void* context = StateContext(editor);
                auto addCatalogueItem = reinterpret_cast<void (*)(void*, void*, void*)>(g_base + 0x611070);
                if (addCatalogueItem) {
                    addCatalogueItem(catalogue, context, customDef);

                    void* scrollOwner = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(catalogue) + 0x78);
                    if (scrollOwner) {
                        void* layoutGrid = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(scrollOwner) + 0xb0);
                        void* scrollPane = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(scrollOwner) + 0xa8);
                        auto layoutFn = reinterpret_cast<void (*)(void*, void*, uint8_t, uint8_t)>(g_base + 0x60bd10);
                        auto refreshScrollFn = reinterpret_cast<void (*)(void*, void*)>(g_base + 0x24b650);
                        if (layoutGrid && layoutFn) layoutFn(layoutGrid, context, 1, 1);
                        if (scrollPane && refreshScrollFn) refreshScrollFn(scrollPane, context);
                    }
                    LogInfo("CDVEH: Dynamically inserted Custom Display into active Catalogue UI widget!");
                }
            }
        }
    }
}

void CloseCustomDisplayDialog(void* self) {
    if (g_dialogTree.dialog) {
        if (g_dialogApi.setOpen) {
            g_dialogApi.setOpen(g_dialogTree.dialog, 0, 1);
        }
        auto setChildrenOpen = reinterpret_cast<void (*)(void*, uint8_t, uint8_t)>(g_base + 0x25aa60);
        if (setChildrenOpen) {
            setChildrenOpen(g_dialogTree.dialog, 0, 1);
        }
        void* editor = self ? self : g_dialogEditor;
        void* context = editor ? StateContext(editor) : nullptr;
        auto setEditboxAlpha = reinterpret_cast<void (*)(void*, void*, uint8_t)>(g_base + 0x24f7d0);
        for (const void* eb : g_dialogTree.editboxes) {
            if (eb) {
                if (setEditboxAlpha) {
                    setEditboxAlpha(const_cast<void*>(eb), context, 0);
                } else {
                    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(const_cast<void*>(eb)) + 0x1c) = false;
                }
            }
        }
    }

    void* editor = self ? self : g_dialogEditor;
    if (editor) {
        void* context = StateContext(editor);
        if (context) {
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(context) + 0x1a0) = nullptr;
        }
    }
}

void OnDialogCancel(void* ctx, void* userdata) {
    void* self = userdata ? userdata : g_dialogEditor;
    LogInfo("CDVEH: Dialog Cancel clicked! self=%p", self);
    CloseCustomDisplayDialog(self);
}

void OnDialogConfirm(void* ctx, void* userdata) {
    void* self = userdata ? userdata : g_dialogEditor;
    LogInfo("CDVEH: Dialog Confirm clicked! self=%p", self);

    if (!g_dialogTree.complete || !g_dialogTree.dialog) return;

    auto readEditboxInt = [](const void* editbox, int defaultValue) -> int {
        if (!editbox) return defaultValue;
        void* content = nullptr;
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const uint8_t*>(editbox) + 0x78, &content, sizeof(content), &read) || !content)
            return defaultValue;
        const char* strData = nullptr;
        uint32_t strLen = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(content) + 0xb8, &strData, sizeof(strData), &read) &&
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(content) + 0xc0, &strLen, sizeof(strLen), &read) &&
            strData && strLen > 0 && strLen < 32) {
            char buf[32] = {};
            if (ReadProcessMemory(GetCurrentProcess(), strData, buf, strLen, &read)) {
                buf[strLen] = '\0';
                try {
                    int val = std::stoi(buf);
                    if (val > 0) return val;
                } catch (...) {}
            }
        }
        return defaultValue;
    };

    int lastW = 1, lastH = 1, lastPx = 32, lastPy = 32;
    GetLastDialogValues(lastW, lastH, lastPx, lastPy);

    int widthBlocks = readEditboxInt(g_dialogTree.editboxes[0], lastW);
    int heightBlocks = readEditboxInt(g_dialogTree.editboxes[1], lastH);
    int resY = readEditboxInt(g_dialogTree.editboxes[2], heightBlocks * 32);

    if (widthBlocks < 1) widthBlocks = 1;
    if (widthBlocks > 100) widthBlocks = 100;
    if (heightBlocks < 1) heightBlocks = 1;
    if (heightBlocks > 100) heightBlocks = 100;
    if (resY < 1) resY = 1;
    if (resY > 8192) resY = 8192;

    // Automatically calculate width resolution to maintain square pixels based on physical block aspect ratio
    int resX = (resY * widthBlocks + heightBlocks / 2) / heightBlocks;
    if (resX < 1) resX = 1;
    if (resX > 8192) resX = 8192;

    SetLastDialogValues(widthBlocks, heightBlocks, resX, resY);

    // Close the dialog and clear modal focus immediately
    CloseCustomDisplayDialog(self);

    if (!self) {
        LogError("CDVEH: self editor state is null, cannot place component");
        return;
    }

    std::string allocatedName = GetOrAllocateDisplayDefinitionId(widthBlocks, heightBlocks, resX, resY);
    char targetName[64];
    strcpy_s(targetName, allocatedName.c_str());

    void* targetDef = FindComponentDefinition(self, targetName);
    if (!targetDef) {
        targetDef = GetOrCreateDynamicDisplayDefinition(self, widthBlocks, heightBlocks, targetName);
    }

    if (targetDef) {
        SetCustomDisplayResolution(targetName, resX, resY);
        using SelectDefFn = void (*)(void*, void*, uint64_t, void*);
        auto selectDef = reinterpret_cast<SelectDefFn>(g_base + 0x801340);
        selectDef(self, targetDef, 0, nullptr);
        LogInfo("CDVEH: Selected %s in player hand with resolution %dx%d!", targetName, resX, resY);
    } else {
        LogError("CDVEH: Could not find or create dynamic display definition %s in registry!", targetName);
    }
}

void OpenCustomDisplayDialog(void* self, void* ui) {
    if (!self || !ui || !g_dialogApi.Ready()) {
        LogError("CDVEH: Cannot open dialog (api ready=%d)", g_dialogApi.Ready());
        return;
    }
    void* context = StateContext(self);
    if (!context) {
        LogError("CDVEH: Context is null");
        return;
    }

    if (!g_dialogTree.complete || !g_dialogTree.dialog) {
        CustomDisplayDialogNativeTree tree{};
        auto result = BuildCustomDisplayCallbackDialog(g_dialogApi, ui, context, tree);
        if (result != CustomDisplayDialogBuildResult::Ok || !tree.complete || !tree.dialog) {
            LogError("CDVEH: BuildCustomDisplayCallbackDialog failed: %d", static_cast<int>(result));
            return;
        }
        g_dialogTree = tree;
        g_dialogEditor = self;

        void* confirmBtn = nullptr;
        void* cancelBtn = nullptr;
        Pointer(tree.dialog, 0xd0, confirmBtn);
        Pointer(tree.dialog, 0xd8, cancelBtn);

        if (confirmBtn) {
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(confirmBtn) + 0xd8) = reinterpret_cast<void*>(&OnDialogConfirm);
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(confirmBtn) + 0xe0) = self;
        }
        if (cancelBtn) {
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cancelBtn) + 0xd8) = reinterpret_cast<void*>(&OnDialogCancel);
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cancelBtn) + 0xe0) = self;
        }
        LogInfo("CDVEH: Custom Display dialog tree built and callbacks attached");
    } else {
        UpdateCustomDisplayDialogEditboxes(g_dialogTree);
        g_dialogEditor = self;
        void* confirmBtn = nullptr;
        void* cancelBtn = nullptr;
        Pointer(g_dialogTree.dialog, 0xd0, confirmBtn);
        Pointer(g_dialogTree.dialog, 0xd8, cancelBtn);
        if (confirmBtn) {
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(confirmBtn) + 0xe0) = self;
        }
        if (cancelBtn) {
            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cancelBtn) + 0xe0) = self;
        }
    }

    if (g_dialogApi.setOpen) {
        g_dialogApi.setOpen(g_dialogTree.dialog, 1, 1);
    }
    auto setChildrenOpen = reinterpret_cast<void (*)(void*, uint8_t, uint8_t)>(g_base + 0x25aa60);
    if (setChildrenOpen) {
        setChildrenOpen(g_dialogTree.dialog, 1, 1);
    }

    // Re-assert solid opaque background
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(g_dialogTree.dialog) + 0x74) = 1;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(g_dialogTree.dialog) + 0x75) = 0xff;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_dialogTree.dialog) + 0x78) = 0.80f;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_dialogTree.dialog) + 0x7c) = -0.88f;
    void* bgRect = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_dialogTree.dialog) + 0x58);
    if (bgRect) {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x08) = 0.0f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x0c) = 0.0f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x10) = 0.80f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(bgRect) + 0x14) = -0.88f;
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(bgRect) + 0x58) = 0xff141618;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(bgRect) + 0x5c) = 0xff;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(bgRect) + 0x1c) = 1;
    }

    auto setEditboxAlpha = reinterpret_cast<void (*)(void*, void*, uint8_t)>(g_base + 0x24f7d0);
    for (const void* eb : g_dialogTree.editboxes) {
        if (eb) {
            if (setEditboxAlpha) {
                setEditboxAlpha(const_cast<void*>(eb), context, 255);
            } else {
                *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(const_cast<void*>(eb)) + 0x1c) = true;
            }
        }
    }

    // Set modal focus in context so clicks are routed to the dialog
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(context) + 0x1a0) = g_dialogTree.dialog;

    LogInfo("CDVEH: Custom Display dialog opened!");
}

void Init(void* self, void* context) {
    const auto event = Begin();
    Observe(event, "ui.init.before", self, context, true);
    originalInit(self, context);
    Observe(event, "ui.init.after", self, context, true);
}

void Update(void* self, void* a, void* context, void* c) {
    const uint32_t frame = frames.fetch_add(1, std::memory_order_relaxed);
    const auto event = frame < 16 || frame % 60 == 0 ? Begin() : UINT32_MAX;
    Observe(event, "ui.update.before", self, context, true);

    if (!g_editorState && context) {
        size_t rootOffset = size_t(layout.stateRoot) + layout.widgets.rootContext;
        void* candidate = reinterpret_cast<uint8_t*>(context) - rootOffset;
        void* uiPtr = nullptr;
        if (Pointer(candidate, 0x8a8, uiPtr) && uiPtr == self) {
            g_editorState = candidate;
            LogInfo("CDVEH: Discovered g_editorState from UI context at %p", g_editorState);
        }
    }
    if (g_editorState && (frame % 30 == 0)) {
        InjectCustomDisplayCatalogueEntry(g_editorState, self);
        FlushPendingCustomDisplayGpuBuffers();
    }

    originalUpdate(self, a, context, c);
    Observe(event, "ui.update.after", self, context, true);
}

void UiTeardown(void* self, void* context) {
    const auto event = Begin();
    Observe(event, "ui.teardown.before", self, context, true);
    CloseCustomDisplayDialog(g_dialogEditor);
    g_dialogTree = {};
    g_dialogEditor = nullptr;
    originalUiTeardown(self, context);
    Observe(event, "ui.teardown.after", self, context, true);
}

void* Destroy(void* self, uint32_t flags) {
    const auto event = Begin();
    Observe(event, "ui.destroy.before", self, nullptr, false);
    CloseCustomDisplayDialog(g_dialogEditor);
    g_dialogTree = {};
    g_dialogEditor = nullptr;
    void* result = originalDestroy(self, flags);
    Observe(event, "ui.destroy.after", self, nullptr, false);
    return result;
}

void StateTeardown(void* self) {
    const auto event = Begin();
    Observe(event, "state.teardown.before", self, StateContext(self), false);
    CloseCustomDisplayDialog(self);
    g_dialogTree = {};
    g_dialogEditor = nullptr;
    originalStateTeardown(self);
    Observe(event, "state.teardown.after", self, nullptr, false);
}

void Actions(void* self, void* a, void* b, void* c, uintptr_t d, int32_t action) {
    FlushPendingCustomDisplayGpuBuffers();
    const auto event = Begin();
    Observe(event, "state.action.before", self, StateContext(self), false, action);

    if ((action == 0x61 || action == 0x64) && self) {
        void* ui = nullptr;
        if (Pointer(self, 0x8a8, ui) && ui) {
            void* def = nullptr;
            if (action == 0x61) {
                void* catalogue = nullptr;
                if (Pointer(ui, 0x150, catalogue) && catalogue) {
                    auto get_selected = reinterpret_cast<void* (*)(void*)>(g_base + 0x611050);
                    void* item_entry = get_selected(catalogue);
                    int itemType = -1;
                    SIZE_T read = 0;
                    if (item_entry && ReadProcessMemory(GetCurrentProcess(), item_entry, &itemType, sizeof(itemType), &read) && itemType == 0) {
                        Pointer(item_entry, 8, def);
                    }
                }
            } else if (action == 0x64) {
                void* toolbar = nullptr;
                if (Pointer(ui, 0x160, toolbar) && toolbar) {
                    auto get_active = reinterpret_cast<void* (*)(void*)>(g_base + 0x60a470);
                    void* active_slot = get_active(toolbar);
                    if (active_slot) {
                        void* item_entry = nullptr;
                        int itemType = -1;
                        SIZE_T read = 0;
                        if (Pointer(active_slot, 0x88, item_entry) && item_entry &&
                            ReadProcessMemory(GetCurrentProcess(), item_entry, &itemType, sizeof(itemType), &read) && itemType == 0) {
                            Pointer(item_entry, 8, def);
                        }
                    }
                }
            }

            if (def) {
                const char* defIdPtr = nullptr;
                SIZE_T read = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t*>(def) + 0x288, &defIdPtr, sizeof(defIdPtr), &read) && defIdPtr) {
                    char buf[64] = {};
                    if (ReadProcessMemory(GetCurrentProcess(), defIdPtr, buf, sizeof(buf) - 1, &read)) {
                        buf[sizeof(buf) - 1] = '\0';
                        if (strcmp(buf, "swml_custom_display") == 0) {
                            LogInfo("CDVEH: swml_custom_display selected via action 0x%x (%d)! Opening native dialog.", action, action);
                            OpenCustomDisplayDialog(self, ui);
                            return; // Intercepted: do not select base placeholder item
                        }
                    }
                }
            }
        }
    }

    originalActions(self, a, b, c, d, action);
    Observe(event, "state.action.after", self, nullptr, false, action);
}

void InspectMonitorDefinition(void* self) {
    void* mon1 = FindComponentDefinition(self, "monitor_1");
    if (!mon1) {
        LogWarn("CDVEH: monitor_1 not found in registry");
        return;
    }
    LogInfo("CDVEH: Found monitor_1 at %p", mon1);

    auto inspectVec = [](const char* name, void* def, size_t offset) {
        void* first = nullptr;
        void* last = nullptr;
        void* end = nullptr;
        if (Pointer(def, offset, first) && Pointer(def, offset + 8, last) && Pointer(def, offset + 16, end)) {
            uintptr_t bFirst = reinterpret_cast<uintptr_t>(first);
            uintptr_t bLast = reinterpret_cast<uintptr_t>(last);
            size_t bytes = bLast >= bFirst ? bLast - bFirst : 0;
            LogInfo("CDVEH: %s vector at offset 0x%zx: first=%p, last=%p, end=%p, totalBytes=%zu", name, offset, first, last, end, bytes);
        }
    };
    inspectVec("surfaces (0x48)", mon1, 0x48);
    inspectVec("buoyancy (0x60)", mon1, 0x60);
    inspectVec("logic (0x78)", mon1, 0x78);
    inspectVec("voxels (0x90)", mon1, 0x90);
    inspectVec("rotations (0xa8)", mon1, 0xa8);

    void* mon2 = FindComponentDefinition(self, "monitor_2");
    if (mon2) {
        LogInfo("CDVEH: Found monitor_2 at %p", mon2);
        inspectVec("voxels_m2 (0x90)", mon2, 0x90);
        inspectVec("surfaces_m2 (0x48)", mon2, 0x48);
    }

    uint8_t* m1Bytes = reinterpret_cast<uint8_t*>(mon1);
    const char* m1Path = *reinterpret_cast<const char**>(m1Bytes + 0x760);
    LogInfo("CDVEH: mon1 mesh_data_name: %s", m1Path ? m1Path : "<null>");
    for (int i = 0; i < 10; ++i) {
        uint64_t w1 = *reinterpret_cast<uint64_t*>(m1Bytes + 0x3a0 + i * 8);
        uint64_t w2 = mon2 ? *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(mon2) + 0x3a0 + i * 8) : 0;
        LogInfo("CDVEH: mesh[0x%x]: mon1=0x%llx, mon2=0x%llx", 0x3a0 + i * 8, w1, w2);
    }

    void* world = nullptr;
    if (Pointer(self, 0x70, world) && world) {
        uint8_t* reg = reinterpret_cast<uint8_t*>(world) + 0xbb670;
        void** allDefs = nullptr;
        uint32_t cap = 0, head = 0, count = 0;
        SIZE_T read = 0;
        if (Pointer(reg, 0xf8, (void*&)allDefs) &&
            ReadProcessMemory(GetCurrentProcess(), reg + 0x100, &cap, 4, &read) &&
            ReadProcessMemory(GetCurrentProcess(), reg + 0x104, &head, 4, &read) &&
            ReadProcessMemory(GetCurrentProcess(), reg + 0x108, &count, 4, &read)) {
            LogInfo("CDVEH: Global registry at %p: buffer=%p, cap=%u, head=%u, count=%u", reg + 0xf8, allDefs, cap, head, count);
        }
    }
}

void Enter(void* self, void* a, void* b, void* c) {
    const auto event = Begin();
    Observe(event, "state.enter.before", self, StateContext(self), false);
    g_editorState = self;
    originalEnter(self, a, b, c);
    Observe(event, "state.enter.after", self, nullptr, false);
    InjectCustomDisplayCatalogueEntry(self, nullptr);
    PreloadSavedCustomDisplays(self);
    FlushPendingCustomDisplayGpuBuffers();
    InspectMonitorDefinition(self);
}

void Leave(void* self, void* a, void* b, void* c) {
    const auto event = Begin();
    Observe(event, "state.leave.before", self, StateContext(self), false);
    CloseCustomDisplayDialog(self);
    g_dialogTree = {};
    g_dialogEditor = nullptr;
    g_editorState = nullptr;
    originalLeave(self, a, b, c);
    Observe(event, "state.leave.after", self, nullptr, false);
}
}

bool InstallCustomDisplayVehicleObserver(uint8_t* base) {
    if (installed) return true;
    if (!IsSupportedEngineBuild(base)) return false;
    const auto result = discovery::DiscoverLoadedEngine(base);
    discovery::Image image;
    CustomDisplayVehicleActivityLayout activity;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!discovery::ParseImage(base, nt->OptionalHeader.SizeOfImage, image) ||
        !DiscoverCustomDisplayVehicleActivityLayout(image, result, activity)) {
        LogError("CDVEH: incomplete reviewed vehicle UI contracts; no observer installed");
        return false;
    }

    BindCustomDisplayDialogNativeApi(image, result, g_dialogApi);
    g_base = reinterpret_cast<uintptr_t>(base);
    layout = activity.lifetime;
    InitializeCustomDisplayRegistry();

    auto** ui = reinterpret_cast<void**>(base + result.Address(0xb99940));
    auto** state = reinterpret_cast<void**>(base + result.Address(0xb44838));
    SlotPatch patches[] = {
        {&ui[layout.uiInitSlot], base + result.Address(0x62ac80), reinterpret_cast<void*>(&Init), reinterpret_cast<void**>(&originalInit)},
        {&ui[layout.uiUpdateSlot], base + result.Address(0x62c530), reinterpret_cast<void*>(&Update), reinterpret_cast<void**>(&originalUpdate)},
        {&ui[layout.uiTeardownSlot], base + result.Address(0x241bc0), reinterpret_cast<void*>(&UiTeardown), reinterpret_cast<void**>(&originalUiTeardown)},
        {&ui[layout.uiDestroySlot], base + result.Address(0x62ed20), reinterpret_cast<void*>(&Destroy), reinterpret_cast<void**>(&originalDestroy)},
        {&state[layout.stateTeardownSlot], base + result.Address(0x7cf750), reinterpret_cast<void*>(&StateTeardown), reinterpret_cast<void**>(&originalStateTeardown)},
        {&state[layout.stateActionsSlot], base + result.Address(0x7fa7a0), reinterpret_cast<void*>(&Actions), reinterpret_cast<void**>(&originalActions)},
        {&state[activity.stateEnterSlot], base + result.Address(0x7cfce0), reinterpret_cast<void*>(&Enter), reinterpret_cast<void**>(&originalEnter)},
        {&state[activity.stateLeaveSlot], base + result.Address(0x7d0770), reinterpret_cast<void*>(&Leave), reinterpret_cast<void**>(&originalLeave)}
    };
    g_vehicleUiTable = ui;
    g_vehicleStateTable = state;
    g_stateEnterSlot = activity.stateEnterSlot;
    g_stateLeaveSlot = activity.stateLeaveSlot;

    if (!PatchSlots(patches, _countof(patches))) {
        LogError("CDVEH: slot conflict/protection failure; no vehicle observer installed");
        return false;
    }
    InstallFindDefHook(base);
    InstallFindDescriptorHook(base);
    InstallDeserializeVehicleComponentsHook(base);
    InstallRegistryClearHook(base);
    InstallLocalizationHook(base);
    installed = true;
    LogInfo("CDVEH: Vehicle observer, FindDef hook, FindDescriptor hook, Deserialize hook, RegistryClear hook, Localization hook, and Custom Display dialog hooks active");
    return true;
}

void UninstallCustomDisplayVehicleObserver() {
    if (!installed || !g_vehicleUiTable || !g_vehicleStateTable) return;
    UninstallLocalizationHook(reinterpret_cast<uint8_t*>(g_base));
    UninstallRegistryClearHook(reinterpret_cast<uint8_t*>(g_base));
    UninstallDeserializeVehicleComponentsHook(reinterpret_cast<uint8_t*>(g_base));
    UninstallFindDescriptorHook(reinterpret_cast<uint8_t*>(g_base));
    UninstallFindDefHook(reinterpret_cast<uint8_t*>(g_base));
    SlotPatch patches[] = {
        {&g_vehicleUiTable[layout.uiInitSlot], reinterpret_cast<void*>(&Init), reinterpret_cast<void*>(originalInit), reinterpret_cast<void**>(&originalInit)},
        {&g_vehicleUiTable[layout.uiUpdateSlot], reinterpret_cast<void*>(&Update), reinterpret_cast<void*>(originalUpdate), reinterpret_cast<void**>(&originalUpdate)},
        {&g_vehicleUiTable[layout.uiTeardownSlot], reinterpret_cast<void*>(&UiTeardown), reinterpret_cast<void*>(originalUiTeardown), reinterpret_cast<void**>(&originalUiTeardown)},
        {&g_vehicleUiTable[layout.uiDestroySlot], reinterpret_cast<void*>(&Destroy), reinterpret_cast<void*>(originalDestroy), reinterpret_cast<void**>(&originalDestroy)},
        {&g_vehicleStateTable[layout.stateTeardownSlot], reinterpret_cast<void*>(&StateTeardown), reinterpret_cast<void*>(originalStateTeardown), reinterpret_cast<void**>(&originalStateTeardown)},
        {&g_vehicleStateTable[layout.stateActionsSlot], reinterpret_cast<void*>(&Actions), reinterpret_cast<void*>(originalActions), reinterpret_cast<void**>(&originalActions)},
        {&g_vehicleStateTable[g_stateEnterSlot], reinterpret_cast<void*>(&Enter), reinterpret_cast<void*>(originalEnter), reinterpret_cast<void**>(&originalEnter)},
        {&g_vehicleStateTable[g_stateLeaveSlot], reinterpret_cast<void*>(&Leave), reinterpret_cast<void*>(originalLeave), reinterpret_cast<void**>(&originalLeave)}
    };
    PatchSlots(patches, _countof(patches));
    installed = false;
    LogInfo("CDVEH: Vehicle observer and Custom Display dialog hooks uninstalled");
}

} // namespace swml
