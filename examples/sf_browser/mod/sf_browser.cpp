// sf_browser.dll: publishes the "browser" service (sf_browser_api.h). Chromium runs in
// cef\sf_browser_host.exe, started here inside a job object (it dies with the game); frames
// arrive through shared memory and are drawn with the loader's image overlay; the image's
// input callback forwards mouse and keyboard to the page. No game address.
#include "../../../include/sw_mod_sdk.h"
#include "../sf_browser_api.h"
#include "../ipc.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace {
const SwModHost* g_host = nullptr;
const SwImageApi* g_image = nullptr;

void Log(const std::string& text) { if (g_host) g_host->log(g_host->modId, text.c_str()); }

// ---- Host process and pipe ------------------------------------------------------------------
// Commands go out on g_cmdPipe (writer thread), events come in on g_evtPipe (reader thread).
HANDLE g_cmdPipe = INVALID_HANDLE_VALUE, g_evtPipe = INVALID_HANDLE_VALUE, g_job = nullptr, g_process = nullptr;
std::wstring g_base;
std::thread g_reader, g_writer;
std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<std::vector<uint8_t>> g_queue;
bool g_stopping = false;
std::atomic<bool> g_hostAlive{false};

void Post(uint16_t type, uint64_t browser, std::initializer_list<int32_t> a, const std::string& text = std::string()) {
    auto bytes = sfb::Encode(type, browser, a, text);
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_stopping || g_queue.size() > 20000) return; // never block the caller (window thread included)
    g_queue.push_back(std::move(bytes));
    g_queueCv.notify_one();
}

void WriterThread() {
    // Waits for the host to connect, then sends queued commands in order.
    if (!ConnectNamedPipe(g_cmdPipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
    for (;;) {
        std::vector<uint8_t> next;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return g_stopping || !g_queue.empty(); });
            if (g_queue.empty()) return; // stopping and drained
            next = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (!sfb::WriteAll(g_cmdPipe, next.data(), next.size())) return;
    }
}

// ---- Browsers -----------------------------------------------------------------------------------
struct Browser {
    uint64_t id = 0;
    const SwModHost* owner = nullptr;
    SfBrowserEventCallback callback = nullptr;
    void* userData = nullptr;
    // Image state (kept across resizes, which recreate the image).
    SwHandle image = 0;
    SwUiSurface surface = SW_SURFACE_NONE;
    SwImageRect rect = {};
    int32_t order = 0;
    float opacity = 1;
    bool visible = true, interactive = false, focused = false;
    // Frame pump.
    std::thread pump;
    HANDLE wake = nullptr;              // stops the pump
    std::atomic<bool> pumpStop{false};
    HANDLE mapping = nullptr, mutex = nullptr, event = nullptr;
    sfb::FrameHeader* frame = nullptr;
    uint32_t width = 0, height = 0;
};
std::recursive_mutex g_callbackMutex; // held while calling into a consumer; release waits on it
std::mutex g_frameMutex;              // serializes AttachFrame and Drop (image + pump lifetime)
std::mutex g_mutex;                   // guards g_browsers and Browser fields
std::map<uint64_t, std::shared_ptr<Browser>> g_browsers;
uint64_t g_nextBrowser = 1;

void StopPump(Browser& b) { // g_mutex NOT held (joins)
    if (b.pump.joinable()) { b.pumpStop = true; SetEvent(b.wake); b.pump.join(); }
    b.pumpStop = false;
    if (b.frame) UnmapViewOfFile(b.frame);
    for (HANDLE h : {b.mapping, b.mutex, b.event}) if (h) CloseHandle(h);
    b.frame = nullptr; b.mapping = b.mutex = b.event = nullptr;
}

void PumpThread(std::shared_ptr<Browser> b) {
    HANDLE waits[2] = {b->event, b->wake};
    while (!b->pumpStop) {
        const DWORD r = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (b->pumpStop) break;
        if (r != WAIT_OBJECT_0) continue;
        if (WaitForSingleObject(b->mutex, 200) != WAIT_OBJECT_0) continue;
        sfb::FrameHeader* f = b->frame;
        if (f->x0 < f->x1 && f->y0 < f->y1 && f->x1 <= b->width && f->y1 <= b->height) {
            SwImageRegion region = {f->x0, f->y0, f->x1 - f->x0, f->y1 - f->y0};
            const auto* pixels = reinterpret_cast<const uint8_t*>(f + 1) + (size_t(f->y0) * b->width + f->x0) * 4;
            g_image->update(g_host, b->image, &region, pixels, b->width * 4);
            f->x0 = f->y0 = f->x1 = f->y1 = 0;
        }
        ReleaseMutex(b->mutex);
    }
}

void OnImageInput(const SwImageInput* e, void* data); // forward

// (Re)creates the image for a new frame buffer. Called on the reader thread.
void AttachFrame(const std::shared_ptr<Browser>& b, uint32_t generation, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> frameLock(g_frameMutex);
    { std::lock_guard<std::mutex> lock(g_mutex); if (!g_browsers.count(b->id)) return; } // dropped meanwhile
    StopPump(*b);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sfb::FrameName(g_base, b->id, generation, L"").c_str());
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, sfb::FrameName(g_base, b->id, generation, L"_m").c_str());
    HANDLE event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, sfb::FrameName(g_base, b->id, generation, L"_e").c_str());
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0) : nullptr;
    if (!view || !mutex || !event) {
        Log("cannot open a frame buffer");
        if (view) UnmapViewOfFile(view);
        for (HANDLE x : {mapping, mutex, event}) if (x) CloseHandle(x);
        return;
    }
    SwHandle old = b->image, image = 0;
    SwUiSurface surface; SwImageRect rect; int32_t order; float opacity; bool visible, interactive, focused;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        surface = b->surface; rect = b->rect; order = b->order; opacity = b->opacity;
        visible = b->visible; interactive = b->interactive; focused = b->focused;
    }
    // Image calls outside g_mutex: the loader may be delivering input to us (lock order).
    if (g_image->create(g_host, surface, w, h, SW_IMAGE_BGRA8_PREMULTIPLIED, 0, &image) == SW_OK) {
        g_image->setRect(g_host, image, &rect);
        g_image->setOrder(g_host, image, order);
        g_image->setOpacity(g_host, image, opacity);
        g_image->setVisible(g_host, image, visible);
        if (interactive) g_image->setInputCallback(g_host, image, &OnImageInput, reinterpret_cast<void*>(uintptr_t(b->id)));
        if (focused) g_image->setKeyboardFocus(g_host, image, true);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        b->image = image;
        b->mapping = mapping; b->mutex = mutex; b->event = event;
        b->frame = static_cast<sfb::FrameHeader*>(view);
        b->width = w; b->height = h;
        b->frame->x0 = 0; b->frame->y0 = 0; b->frame->x1 = w; b->frame->y1 = h; // first upload: everything
    }
    if (old) g_image->destroy(g_host, old);
    if (!image) { Log("cannot create the browser image"); return; }
    SetEvent(event);
    b->pump = std::thread(&PumpThread, b);
}

void Emit(const std::shared_ptr<Browser>& b, SfBrowserEventType type, const std::string& text, int32_t code = 0) {
    std::lock_guard<std::recursive_mutex> calling(g_callbackMutex);
    SfBrowserEventCallback fn;
    void* data;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_browsers.count(b->id)) return; // destroyed meanwhile
        fn = b->callback; data = b->userData;
    }
    if (!fn) return;
    SfBrowserEvent e = {sizeof(SfBrowserEvent), type, b->id, text.c_str(), code};
    fn(&e, data);
}

std::shared_ptr<Browser> Get(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_browsers.find(id);
    return it == g_browsers.end() ? nullptr : it->second;
}

void ReaderThread() {
    if (!ConnectNamedPipe(g_evtPipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
    sfb::Message m;
    while (sfb::Receive(g_evtPipe, m)) {
        if (m.h.type == sfb::kReady) { g_hostAlive = true; Log("Chromium host ready"); continue; }
        if (m.h.type == sfb::kLog) { Log("host: " + m.text); continue; }
        auto b = Get(m.h.browser);
        if (!b) continue;
        switch (m.h.type) {
        case sfb::kFrameBuffer:
            if (m.h.a[1] > 0 && m.h.a[2] > 0 && uint32_t(m.h.a[1]) <= sfb::kMaxDimension && uint32_t(m.h.a[2]) <= sfb::kMaxDimension)
                AttachFrame(b, uint32_t(m.h.a[0]), uint32_t(m.h.a[1]), uint32_t(m.h.a[2]));
            break;
        case sfb::kMessage: Emit(b, SF_BROWSER_EVENT_MESSAGE, m.text); break;
        case sfb::kLoadStart: Emit(b, SF_BROWSER_EVENT_LOAD_START, m.text); break;
        case sfb::kLoadEnd: Emit(b, SF_BROWSER_EVENT_LOAD_END, m.text, m.h.a[0]); break;
        case sfb::kLoadError: Emit(b, SF_BROWSER_EVENT_LOAD_ERROR, m.text, m.h.a[0]); break;
        case sfb::kTitle: Emit(b, SF_BROWSER_EVENT_TITLE, m.text); break;
        case sfb::kUrl: Emit(b, SF_BROWSER_EVENT_URL, m.text); break;
        case sfb::kCrashed: Emit(b, SF_BROWSER_EVENT_CRASHED, std::string()); break;
        default: break;
        }
    }
    // Host gone: tell every consumer once.
    g_hostAlive = false;
    std::vector<std::shared_ptr<Browser>> all;
    { std::lock_guard<std::mutex> lock(g_mutex); for (auto& b : g_browsers) all.push_back(b.second); }
    bool stopping;
    { std::lock_guard<std::mutex> lock(g_queueMutex); stopping = g_stopping; }
    if (!stopping) {
        Log("Chromium host process ended");
        for (auto& b : all) Emit(b, SF_BROWSER_EVENT_CLOSED, std::string());
    }
}

// ---- Input from the image (window thread): forward to the page --------------------------------
void OnImageInput(const SwImageInput* e, void* data) {
    const uint64_t id = uint64_t(uintptr_t(data));
    const int32_t x = int32_t(e->x), y = int32_t(e->y), mods = int32_t(e->modifiers), buttons = int32_t(e->buttons);
    switch (e->type) {
    case SW_IMAGE_INPUT_MOUSE_MOVE: Post(sfb::kMouseMove, id, {x, y, mods, buttons}); break;
    case SW_IMAGE_INPUT_MOUSE_LEAVE: Post(sfb::kMouseLeave, id, {x, y, mods, buttons}); break;
    case SW_IMAGE_INPUT_MOUSE_DOWN:
    case SW_IMAGE_INPUT_MOUSE_UP:
        Post(sfb::kMouseClick, id, {x, y, mods, buttons, int32_t(e->button), e->type == SW_IMAGE_INPUT_MOUSE_UP ? 1 : 0,
                                    int32_t(e->clickCount)});
        break;
    case SW_IMAGE_INPUT_MOUSE_WHEEL:
        Post(sfb::kMouseWheel, id, {x, y, mods, buttons, int32_t(e->wheelX * 120), int32_t(e->wheelY * 120)});
        break;
    case SW_IMAGE_INPUT_KEY_DOWN:
    case SW_IMAGE_INPUT_KEY_UP:
        Post(sfb::kKey, id, {e->type == SW_IMAGE_INPUT_KEY_DOWN ? 0 : 1, int32_t(e->virtualKey), int32_t(e->nativeKeyData), mods,
                             e->systemKey ? 1 : 0});
        break;
    case SW_IMAGE_INPUT_CHAR: {
        const uint32_t cp = e->codepoint;
        if (cp > 0xFFFF) { // CEF takes UTF-16 units
            Post(sfb::kKey, id, {2, int32_t(0xD800 + ((cp - 0x10000) >> 10)), int32_t(e->nativeKeyData), mods, 0});
            Post(sfb::kKey, id, {2, int32_t(0xDC00 + ((cp - 0x10000) & 0x3FF)), int32_t(e->nativeKeyData), mods, 0});
        } else {
            Post(sfb::kKey, id, {2, int32_t(cp), int32_t(e->nativeKeyData), mods, 0});
        }
        break;
    }
    case SW_IMAGE_INPUT_FOCUS:
    case SW_IMAGE_INPUT_BLUR: {
        const bool focus = e->type == SW_IMAGE_INPUT_FOCUS;
        { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_browsers.find(id); if (it != g_browsers.end()) it->second->focused = focus; }
        Post(sfb::kFocus, id, {focus ? 1 : 0});
        break;
    }
    default: break;
    }
}

// ---- Service ----------------------------------------------------------------------------------
bool ValidUrl(const char* s) { return s && *s && strnlen_s(s, 1 << 20) < (1 << 20); }

std::shared_ptr<Browser> Owned(const SwModHost* caller, SfBrowserHandle id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_browsers.find(id);
    return it != g_browsers.end() && it->second->owner == caller ? it->second : nullptr;
}

SwResult Create(const SwModHost* caller, const SfBrowserOptions* o, SfBrowserEventCallback cb, void* data, SfBrowserHandle* out) {
    if (!caller) return SW_INVALID_OWNER;
    if (!o || !out || o->structSize < sizeof(SfBrowserOptions) || !ValidUrl(o->url) || o->width < 1 || o->height < 1 ||
        o->width > sfb::kMaxDimension || o->height > sfb::kMaxDimension || o->surface < SW_SURFACE_MENU || o->surface > SW_SURFACE_NONE ||
        o->rect.anchor < 0 || o->rect.anchor > 8) return SW_INVALID_ARGUMENT;
    auto b = std::make_shared<Browser>();
    b->owner = caller; b->callback = cb; b->userData = data;
    b->surface = o->surface; b->rect = o->rect; b->order = o->order; b->interactive = o->interactive;
    b->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!b->wake) return SW_UNAVAILABLE;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        size_t mine = 0;
        for (auto& x : g_browsers) if (x.second->owner == caller) ++mine;
        if (mine >= 16 || g_browsers.size() >= 32) { CloseHandle(b->wake); return SW_LIMIT_REACHED; }
        b->id = g_nextBrowser++;
        g_browsers[b->id] = b;
    }
    Post(sfb::kCreate, b->id, {int32_t(o->width), int32_t(o->height), o->transparent ? 1 : 0}, o->url);
    *out = b->id;
    return SW_OK;
}
void Drop(const std::shared_ptr<Browser>& b) { // removes it everywhere; never while g_mutex held
    { std::lock_guard<std::mutex> lock(g_mutex); g_browsers.erase(b->id); }
    Post(sfb::kDestroy, b->id, {});
    std::lock_guard<std::mutex> frameLock(g_frameMutex);
    StopPump(*b);
    if (b->image) g_image->destroy(g_host, b->image);
    b->image = 0;
    if (b->wake) CloseHandle(b->wake);
    b->wake = nullptr;
}
SwResult Destroy(const SwModHost* caller, SfBrowserHandle id) {
    auto b = Owned(caller, id);
    if (!b) return SW_NOT_FOUND;
    Drop(b);
    return SW_OK;
}
SwResult Text(const SwModHost* caller, SfBrowserHandle id, const char* text, uint16_t type) {
    if (!ValidUrl(text) && type != sfb::kPost) return SW_INVALID_ARGUMENT;
    if (!text) return SW_INVALID_ARGUMENT;
    if (!Owned(caller, id)) return SW_NOT_FOUND;
    Post(type, id, {}, text);
    return SW_OK;
}
SwResult LoadUrl(const SwModHost* c, SfBrowserHandle id, const char* url) { return Text(c, id, url, sfb::kLoadUrl); }
SwResult ExecJs(const SwModHost* c, SfBrowserHandle id, const char* code) { return Text(c, id, code, sfb::kExecJs); }
SwResult PostMessageToPage(const SwModHost* c, SfBrowserHandle id, const char* text) { return Text(c, id, text, sfb::kPost); }
SwResult Navigate(const SwModHost* c, SfBrowserHandle id, int32_t action) {
    if (action < SF_BROWSER_BACK || action > SF_BROWSER_STOP) return SW_INVALID_ARGUMENT;
    if (!Owned(c, id)) return SW_NOT_FOUND;
    Post(sfb::kNavigate, id, {action});
    return SW_OK;
}
SwResult Resize(const SwModHost* c, SfBrowserHandle id, uint32_t w, uint32_t h) {
    if (w < 1 || h < 1 || w > sfb::kMaxDimension || h > sfb::kMaxDimension) return SW_INVALID_ARGUMENT;
    if (!Owned(c, id)) return SW_NOT_FOUND;
    Post(sfb::kResize, id, {int32_t(w), int32_t(h)});
    return SW_OK;
}
// Updates the remembered state under g_mutex, then applies it to the image outside the lock
// (image calls may wait for the loader's input delivery, which can call back into us).
template<class Fn> SwResult WithImage(const SwModHost* c, SfBrowserHandle id, Fn fn) {
    auto b = Owned(c, id);
    if (!b) return SW_NOT_FOUND;
    std::lock_guard<std::mutex> frameLock(g_frameMutex); // the image cannot be replaced meanwhile
    SwHandle image;
    { std::lock_guard<std::mutex> lock(g_mutex); image = b->image; }
    return fn(*b, image);
}
SwResult SetRect(const SwModHost* c, SfBrowserHandle id, const SwImageRect* r) {
    if (!r || r->anchor < 0 || r->anchor > 8) return SW_INVALID_ARGUMENT;
    return WithImage(c, id, [&](Browser& b, SwHandle img) {
        { std::lock_guard<std::mutex> lock(g_mutex); b.rect = *r; }
        return img ? g_image->setRect(g_host, img, r) : SW_OK;
    });
}
SwResult SetVisible(const SwModHost* c, SfBrowserHandle id, bool v) {
    return WithImage(c, id, [&](Browser& b, SwHandle img) {
        { std::lock_guard<std::mutex> lock(g_mutex); b.visible = v; }
        return img ? g_image->setVisible(g_host, img, v) : SW_OK;
    });
}
SwResult SetOrder(const SwModHost* c, SfBrowserHandle id, int32_t order) {
    return WithImage(c, id, [&](Browser& b, SwHandle img) {
        { std::lock_guard<std::mutex> lock(g_mutex); b.order = order; }
        return img ? g_image->setOrder(g_host, img, order) : SW_OK;
    });
}
SwResult SetOpacity(const SwModHost* c, SfBrowserHandle id, float opacity) {
    if (!(opacity >= 0 && opacity <= 1)) return SW_INVALID_ARGUMENT;
    return WithImage(c, id, [&](Browser& b, SwHandle img) {
        { std::lock_guard<std::mutex> lock(g_mutex); b.opacity = opacity; }
        return img ? g_image->setOpacity(g_host, img, opacity) : SW_OK;
    });
}
SwResult SetFocus(const SwModHost* c, SfBrowserHandle id, bool focus) {
    return WithImage(c, id, [&](Browser& b, SwHandle img) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!b.interactive) return SW_INVALID_ARGUMENT;
            b.focused = focus;
        }
        return img ? g_image->setKeyboardFocus(g_host, img, focus) : SW_OK;
    });
}
SwResult FileUrl(const SwModHost* caller, const char* relative, char* out, size_t capacity) {
    if (!caller || !caller->modDir || !relative || !*relative || !out || !capacity) return SW_INVALID_ARGUMENT;
    std::string rel(relative);
    if (rel.find("..") != std::string::npos || rel.find(':') != std::string::npos || rel[0] == '/' || rel[0] == '\\')
        return SW_INVALID_ARGUMENT;
    std::string path = std::string(caller->modDir) + "/" + rel;
    std::string url = "file:///";
    for (char ch : path) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\\') url += '/';
        else if (c == ' ' || c == '%' || c == '#' || c == '?') { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); url += b; }
        else url += ch;
    }
    if (url.size() + 1 > capacity) return SW_BUFFER_TOO_SMALL;
    memcpy(out, url.c_str(), url.size() + 1);
    return SW_OK;
}

const SfBrowserService kService = {sizeof(SfBrowserService), &Create, &Destroy, &LoadUrl, &ExecJs, &PostMessageToPage,
    &Navigate, &Resize, &SetRect, &SetVisible, &SetOrder, &SetOpacity, &SetFocus, &FileUrl};

// A consumer is unloading: close its browsers; no callback of it runs after this returns.
void OnOwnerReleased(const SwModHost* released, void*) {
    std::lock_guard<std::recursive_mutex> calling(g_callbackMutex);
    std::vector<std::shared_ptr<Browser>> mine;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& b : g_browsers) if (b.second->owner == released) mine.push_back(b.second);
    }
    for (auto& b : mine) Drop(b);
}

std::wstring Utf8ToWide(const char* s) { return sfb::Widen(s ? s : ""); }

bool StartHost() {
    const std::wstring dir = Utf8ToWide(g_host->modDir), cef = dir + L"\\cef", exe = cef + L"\\sf_browser_host.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("missing cef\\sf_browser_host.exe"); return false; }
    g_base = L"sfb_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    g_cmdPipe = CreateNamedPipeW(sfb::PipeName(g_base, L"_cmd").c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 mode, 1, 1 << 20, 0, 0, nullptr);
    g_evtPipe = CreateNamedPipeW(sfb::PipeName(g_base, L"_evt").c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 mode, 1, 0, 1 << 20, 0, nullptr);
    if (g_cmdPipe == INVALID_HANDLE_VALUE || g_evtPipe == INVALID_HANDLE_VALUE) { Log("cannot create the pipes"); return false; }
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!g_job || !SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        Log("cannot create the job object"); return false;
    }
    std::wstring cmd = L"\"" + exe + L"\" --sf-base=" + g_base + L" \"--sf-cache=" + dir +
                       L"\\cache\" \"--sf-log=" + dir + L"\\cef.log\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        cef.c_str(), &si, &pi)) {
        Log("cannot start sf_browser_host.exe (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    if (!AssignProcessToJobObject(g_job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        Log("cannot attach the host to the job object");
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    g_writer = std::thread(&WriterThread);
    g_reader = std::thread(&ReaderThread);
    return true;
}

void StopHost() {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_cmdPipe != INVALID_HANDLE_VALUE && !g_stopping) {
            g_queue.push_back(sfb::Encode(sfb::kShutdown, 0, {}));
        }
        g_stopping = true;
        g_queueCv.notify_all();
    }
    // The host closes its browsers and exits on kShutdown; if it does not, the job ends it.
    if (g_process && WaitForSingleObject(g_process, 3000) != WAIT_OBJECT_0 && g_job) TerminateJobObject(g_job, 0);
    // Unblock a thread still waiting for a connection or in a read/write.
    for (std::thread* t : {&g_writer, &g_reader}) {
        if (!t->joinable()) continue;
        while (WaitForSingleObject(t->native_handle(), 50) == WAIT_TIMEOUT) CancelSynchronousIo(t->native_handle());
        t->join();
    }
    for (HANDLE* h : {&g_cmdPipe, &g_evtPipe}) if (*h != INVALID_HANDLE_VALUE) { CloseHandle(*h); *h = INVALID_HANDLE_VALUE; }
    if (g_process) CloseHandle(g_process);
    if (g_job) CloseHandle(g_job);
    g_process = nullptr; g_job = nullptr;
}
} // namespace

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION || !host->api->runtime) return false;
    const SwEngineApi* api = host->api;
    if (api->structSize < offsetof(SwEngineApi, services) + sizeof(api->services) || !api->image || !api->services ||
        api->image->structSize < offsetof(SwImageApi, setKeyboardFocus) + sizeof(api->image->setKeyboardFocus) ||
        api->services->structSize < offsetof(SwServiceApi, setOwnerReleaseCallback) + sizeof(api->services->setOwnerReleaseCallback) ||
        !(api->runtime->capabilities() & SW_CAP_NATIVE_IMAGE)) {
        host->log(host->modId, "this loader lacks the image/services API needed by sf_browser");
        return false;
    }
    g_host = host;
    g_image = api->image;
    if (!StartHost()) { StopHost(); g_host = nullptr; return false; }
    api->services->setOwnerReleaseCallback(host, &OnOwnerReleased, nullptr);
    if (api->services->publish(host, SF_BROWSER_SERVICE, SF_BROWSER_VERSION, &kService, sizeof(kService)) != SW_OK) {
        Log("cannot publish the browser service");
        StopHost();
        g_host = nullptr;
        return false;
    }
    Log("browser service published; Chromium host starting");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {
    // The loader already withdrew the service, our images and their input callbacks.
    std::vector<std::shared_ptr<Browser>> all;
    { std::lock_guard<std::mutex> lock(g_mutex); for (auto& b : g_browsers) all.push_back(b.second); }
    for (auto& b : all) { StopPump(*b); if (b->wake) CloseHandle(b->wake); b->wake = nullptr; }
    { std::lock_guard<std::mutex> lock(g_mutex); g_browsers.clear(); }
    StopHost();
    g_host = nullptr;
}
