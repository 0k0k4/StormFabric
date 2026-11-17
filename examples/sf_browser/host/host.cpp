// sf_browser_host.exe: runs Chromium (CEF) OUTSIDE the game. Launched by sf_browser.dll with
//   --sf-base=<pipe and shared memory prefix> --sf-cache=<dir> --sf-log=<file>
// It renders every browser off-screen and copies the pixels into shared memory; commands and
// events go through the pipe (see ../ipc.h). The same executable is CEF's subprocess
// (renderer, GPU, utility): CefExecuteProcess handles that role first.
#include "../ipc.h"
#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace {

// ---- Bridge injected in every page --------------------------------------------------------
const char kBridge[] = R"JS((function () {
  if (window.sfBrowser) return;
  window.sfBrowser = {
    onmessage: null,
    send: function (text) {
      window.cefQuery({ request: String(text), persistent: false,
                        onSuccess: function () {}, onFailure: function () {} });
    },
    _deliver: function (text) {
      if (typeof window.sfBrowser.onmessage === 'function') window.sfBrowser.onmessage(text);
    }
  };
})();)JS";

std::string JsString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += char(c);
        }
    }
    // U+2028/U+2029 end lines in older JS parsers: escape them too.
    size_t p;
    while ((p = out.find("\xE2\x80\xA8")) != std::string::npos) out.replace(p, 3, "\\u2028");
    while ((p = out.find("\xE2\x80\xA9")) != std::string::npos) out.replace(p, 3, "\\u2029");
    return out + "\"";
}

// ---- Pipe to the game ---------------------------------------------------------------------
HANDLE g_cmdPipe = INVALID_HANDLE_VALUE, g_evtPipe = INVALID_HANDLE_VALUE; // read / write
std::mutex g_writeMutex;
std::wstring g_base;
void Send(uint16_t type, uint64_t browser, std::initializer_list<int32_t> a, const std::string& text = std::string()) {
    auto bytes = sfb::Encode(type, browser, a, text.size() > sfb::kMaxMessage / 2 ? text.substr(0, sfb::kMaxMessage / 2) : text);
    std::lock_guard<std::mutex> lock(g_writeMutex);
    if (g_evtPipe != INVALID_HANDLE_VALUE) sfb::WriteAll(g_evtPipe, bytes.data(), bytes.size());
}
void Log(const std::string& text) { Send(sfb::kLog, 0, {}, text); }

// ---- Shared frame -------------------------------------------------------------------------
struct Frame {
    HANDLE mapping = nullptr, mutex = nullptr, event = nullptr;
    sfb::FrameHeader* header = nullptr;
    uint8_t* pixels = nullptr;
    uint32_t width = 0, height = 0, generation = 0;
    void Close() {
        if (header) UnmapViewOfFile(header);
        for (HANDLE h : {mapping, mutex, event}) if (h) CloseHandle(h);
        *this = Frame();
    }
    bool Open(uint64_t browser, uint32_t gen, uint32_t w, uint32_t h) {
        const uint64_t bytes = sizeof(sfb::FrameHeader) + uint64_t(w) * h * 4;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(bytes >> 32), DWORD(bytes),
                                     sfb::FrameName(g_base, browser, gen, L"").c_str());
        mutex = CreateMutexW(nullptr, FALSE, sfb::FrameName(g_base, browser, gen, L"_m").c_str());
        event = CreateEventW(nullptr, FALSE, FALSE, sfb::FrameName(g_base, browser, gen, L"_e").c_str());
        if (!mapping || !mutex || !event) { Close(); return false; }
        header = static_cast<sfb::FrameHeader*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(bytes)));
        if (!header) { Close(); return false; }
        pixels = reinterpret_cast<uint8_t*>(header + 1);
        header->width = w; header->height = h;
        header->x0 = header->y0 = header->x1 = header->y1 = 0;
        width = w; height = h; generation = gen;
        return true;
    }
};

// ---- One browser ----------------------------------------------------------------------------
class Client;
std::map<uint64_t, CefRefPtr<Client>> g_clients; // UI thread
int g_open = 0;
bool g_quitting = false;
CefRefPtr<CefMessageRouterBrowserSide> g_router;

void MaybeQuit() { if (g_quitting && g_open == 0) CefQuitMessageLoop(); }

class Client : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefLoadHandler,
               public CefDisplayHandler, public CefRequestHandler {
public:
    Client(uint64_t id, uint32_t w, uint32_t h) : id_(id), width_(w), height_(h) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    CefRefPtr<CefBrowser> browser() const { return browser_; }
    uint64_t id() const { return id_; }

    // Shared memory for the current size; the game reopens it on kFrameBuffer.
    bool NewFrame() {
        Frame next;
        if (!next.Open(id_, frame_.generation + 1, width_, height_)) { Log("cannot create frame buffer"); return false; }
        frame_.Close();
        frame_ = next;
        Send(sfb::kFrameBuffer, id_, {int32_t(frame_.generation), int32_t(width_), int32_t(height_)});
        return true;
    }
    void Resize(uint32_t w, uint32_t h) {
        width_ = w; height_ = h;
        NewFrame();
        if (browser_) browser_->GetHost()->WasResized();
    }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, int(width_), int(height_)); }
    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override {
        info.device_scale_factor = 1;
        info.rect = info.available_rect = CefRect(0, 0, int(width_), int(height_));
        return true;
    }
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override {
        popupVisible_ = show;
        if (!show) { popup_.clear(); browser->GetHost()->Invalidate(PET_VIEW); }
    }
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popupRect_ = rect; }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty, const void* buffer,
                 int w, int h) override {
        if (!frame_.header) return;
        if (type == PET_POPUP) {
            popup_.assign(static_cast<const uint8_t*>(buffer), static_cast<const uint8_t*>(buffer) + size_t(w) * h * 4);
            popupW_ = w; popupH_ = h;
            Lock();
            BlitPopup();
            Unlock();
            return;
        }
        if (uint32_t(w) != frame_.width || uint32_t(h) != frame_.height) return; // stale size during a resize
        Lock();
        const auto* src = static_cast<const uint8_t*>(buffer);
        for (const CefRect& r : dirty) {
            const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
            const int x1 = std::min(w, r.x + r.width), y1 = std::min(h, r.y + r.height);
            if (x0 >= x1 || y0 >= y1) continue;
            for (int y = y0; y < y1; ++y)
                memcpy(frame_.pixels + (size_t(y) * w + x0) * 4, src + (size_t(y) * w + x0) * 4, size_t(x1 - x0) * 4);
            Mark(x0, y0, x1, y1);
        }
        if (popupVisible_ && !popup_.empty()) BlitPopup();
        Unlock();
    }

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override { browser_ = browser; ++g_open; }
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int, const CefString& url, const CefString&,
                       cef_window_open_disposition_t, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                       CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
        frame->LoadURL(url); // no separate windows: open in place
        return true;
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        g_router->OnBeforeClose(browser);
        browser_ = nullptr;
        frame_.Close();
        --g_open;
        MaybeQuit();
    }

    // CefLoadHandler
    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override {
        if (frame->IsMain()) Send(sfb::kLoadStart, id_, {}, frame->GetURL().ToString());
    }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int status) override {
        if (frame->IsMain()) Send(sfb::kLoadEnd, id_, {status}, frame->GetURL().ToString());
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code, const CefString&,
                     const CefString& url) override {
        if (frame->IsMain() && code != ERR_ABORTED) Send(sfb::kLoadError, id_, {int32_t(code)}, url.ToString());
    }

    // CefDisplayHandler
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override { Send(sfb::kTitle, id_, {}, title.ToString()); }
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (frame->IsMain()) Send(sfb::kUrl, id_, {}, url.ToString());
    }

    // CefRequestHandler
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest>, bool, bool) override {
        g_router->OnBeforeBrowse(browser, frame);
        return false;
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus, int, const CefString&) override {
        g_router->OnRenderProcessTerminated(browser);
        Send(sfb::kCrashed, id_, {});
    }

    // CefClient
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                                  CefRefPtr<CefProcessMessage> message) override {
        return g_router->OnProcessMessageReceived(browser, frame, source, message);
    }

private:
    void Lock() { WaitForSingleObject(frame_.mutex, 1000); }
    void Unlock() {
        frame_.header->sequence++;
        ReleaseMutex(frame_.mutex);
        SetEvent(frame_.event);
    }
    void Mark(int x0, int y0, int x1, int y1) {
        auto* f = frame_.header;
        if (f->x0 >= f->x1 || f->y0 >= f->y1) { f->x0 = x0; f->y0 = y0; f->x1 = x1; f->y1 = y1; return; }
        f->x0 = std::min<uint32_t>(f->x0, x0); f->y0 = std::min<uint32_t>(f->y0, y0);
        f->x1 = std::max<uint32_t>(f->x1, x1); f->y1 = std::max<uint32_t>(f->y1, y1);
    }
    void BlitPopup() { // popup buffer over the view, clipped to the frame
        const int x0 = std::max(0, popupRect_.x), y0 = std::max(0, popupRect_.y);
        const int x1 = std::min(int(frame_.width), popupRect_.x + popupW_);
        const int y1 = std::min(int(frame_.height), popupRect_.y + popupH_);
        if (x0 >= x1 || y0 >= y1) return;
        for (int y = y0; y < y1; ++y)
            memcpy(frame_.pixels + (size_t(y) * frame_.width + x0) * 4,
                   popup_.data() + (size_t(y - popupRect_.y) * popupW_ + (x0 - popupRect_.x)) * 4, size_t(x1 - x0) * 4);
        Mark(x0, y0, x1, y1);
    }

    uint64_t id_;
    uint32_t width_, height_;
    CefRefPtr<CefBrowser> browser_;
    Frame frame_;
    bool popupVisible_ = false;
    CefRect popupRect_;
    std::vector<uint8_t> popup_;
    int popupW_ = 0, popupH_ = 0;
    IMPLEMENT_REFCOUNTING(Client);
};

// window.sfBrowser.send(text) arrives here through cefQuery.
class QueryHandler : public CefMessageRouterBrowserSide::Handler {
public:
    using CefMessageRouterBrowserSide::Handler::OnQuery;
    bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int64_t, const CefString& request, bool,
                 CefRefPtr<Callback> callback) override {
        for (auto& c : g_clients)
            if (c.second->browser() && c.second->browser()->IsSame(browser)) Send(sfb::kMessage, c.first, {}, request.ToString());
        callback->Success("");
        return true;
    }
};
QueryHandler g_queryHandler;

// ---- Commands from the game (UI thread) -------------------------------------------------------
CefMouseEvent Mouse(const sfb::Message& m) {
    CefMouseEvent e;
    e.x = m.h.a[0]; e.y = m.h.a[1];
    uint32_t f = 0;
    if (m.h.a[2] & sfb::kModCtrl) f |= EVENTFLAG_CONTROL_DOWN;
    if (m.h.a[2] & sfb::kModShift) f |= EVENTFLAG_SHIFT_DOWN;
    if (m.h.a[2] & sfb::kModAlt) f |= EVENTFLAG_ALT_DOWN;
    if (m.h.a[3] & sfb::kButtonLeft) f |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (m.h.a[3] & sfb::kButtonRight) f |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (m.h.a[3] & sfb::kButtonMiddle) f |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    e.modifiers = f;
    return e;
}

void CloseAll() {
    g_quitting = true;
    for (auto& c : g_clients) if (c.second->browser()) c.second->browser()->GetHost()->CloseBrowser(true);
    g_clients.clear();
    MaybeQuit();
}

void Handle(sfb::Message m) {
    CEF_REQUIRE_UI_THREAD();
    if (m.h.type == sfb::kShutdown) { CloseAll(); return; }
    if (m.h.type == sfb::kCreate) {
        if (g_quitting || g_clients.count(m.h.browser)) return;
        const uint32_t w = std::clamp<int32_t>(m.h.a[0], 1, sfb::kMaxDimension), h = std::clamp<int32_t>(m.h.a[1], 1, sfb::kMaxDimension);
        CefRefPtr<Client> client = new Client(m.h.browser, w, h);
        if (!client->NewFrame()) return;
        g_clients[m.h.browser] = client;
        CefWindowInfo window;
        window.SetAsWindowless(nullptr);
        CefBrowserSettings settings;
        settings.windowless_frame_rate = 60;
        settings.background_color = m.h.a[2] ? CefColorSetARGB(0, 0, 0, 0) : CefColorSetARGB(255, 255, 255, 255);
        if (!CefBrowserHost::CreateBrowser(window, client, m.text, settings, nullptr, nullptr)) Log("CreateBrowser failed");
        return;
    }
    auto it = g_clients.find(m.h.browser);
    if (it == g_clients.end()) return;
    Client& c = *it->second;
    if (m.h.type == sfb::kResize) {
        c.Resize(std::clamp<int32_t>(m.h.a[0], 1, sfb::kMaxDimension), std::clamp<int32_t>(m.h.a[1], 1, sfb::kMaxDimension));
        return;
    }
    CefRefPtr<CefBrowser> b = c.browser();
    if (m.h.type == sfb::kDestroy) {
        if (b) b->GetHost()->CloseBrowser(true);
        g_clients.erase(it);
        return;
    }
    if (!b) return; // not created yet: CreateBrowser is asynchronous, early input is dropped
    CefRefPtr<CefBrowserHost> host = b->GetHost();
    switch (m.h.type) {
    case sfb::kLoadUrl: b->GetMainFrame()->LoadURL(m.text); break;
    case sfb::kExecJs: b->GetMainFrame()->ExecuteJavaScript(m.text, b->GetMainFrame()->GetURL(), 0); break;
    case sfb::kPost:
        b->GetMainFrame()->ExecuteJavaScript("window.sfBrowser&&window.sfBrowser._deliver(" + JsString(m.text) + ");",
                                             b->GetMainFrame()->GetURL(), 0);
        break;
    case sfb::kNavigate:
        if (m.h.a[0] == 0) b->GoBack(); else if (m.h.a[0] == 1) b->GoForward();
        else if (m.h.a[0] == 2) b->Reload(); else if (m.h.a[0] == 3) b->StopLoad();
        break;
    case sfb::kMouseMove: host->SendMouseMoveEvent(Mouse(m), false); break;
    case sfb::kMouseLeave: host->SendMouseMoveEvent(Mouse(m), true); break;
    case sfb::kMouseClick: {
        const cef_mouse_button_type_t button = m.h.a[4] == 1 ? MBT_RIGHT : m.h.a[4] == 2 ? MBT_MIDDLE : MBT_LEFT;
        host->SendMouseClickEvent(Mouse(m), button, m.h.a[5] != 0, std::max(1, m.h.a[6]));
        break;
    }
    case sfb::kMouseWheel: host->SendMouseWheelEvent(Mouse(m), m.h.a[4], m.h.a[5]); break;
    case sfb::kKey: {
        CefKeyEvent e;
        e.type = m.h.a[0] == 0 ? KEYEVENT_RAWKEYDOWN : m.h.a[0] == 1 ? KEYEVENT_KEYUP : KEYEVENT_CHAR;
        e.windows_key_code = m.h.a[1];
        e.native_key_code = m.h.a[2];
        uint32_t f = 0;
        if (m.h.a[3] & sfb::kModCtrl) f |= EVENTFLAG_CONTROL_DOWN;
        if (m.h.a[3] & sfb::kModShift) f |= EVENTFLAG_SHIFT_DOWN;
        if (m.h.a[3] & sfb::kModAlt) f |= EVENTFLAG_ALT_DOWN;
        e.modifiers = f;
        e.is_system_key = m.h.a[4] != 0;
        if (e.type == KEYEVENT_CHAR) e.character = e.unmodified_character = char16_t(m.h.a[1]);
        host->SendKeyEvent(e);
        break;
    }
    case sfb::kFocus: host->SetFocus(m.h.a[0] != 0); break;
    default: break;
    }
}

DWORD WINAPI ReaderThread(void*) {
    sfb::Message m;
    while (sfb::Receive(g_cmdPipe, m)) CefPostTask(TID_UI, base::BindOnce(&Handle, m));
    // The game closed the pipe (unload or exit): close everything and leave.
    CefPostTask(TID_UI, base::BindOnce(&CloseAll));
    return 0;
}

// ---- CefApp: renderer side of the bridge -------------------------------------------------------
class App : public CefApp, public CefRenderProcessHandler, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString& type, CefRefPtr<CefCommandLine> command) override {
        if (type.empty()) {
            command->AppendSwitch("disable-extensions");
            command->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        }
    }
    void OnWebKitInitialized() override { renderer_ = CefMessageRouterRendererSide::Create(CefMessageRouterConfig()); }
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
        renderer_->OnContextCreated(browser, frame, context);
        if (frame->IsMain()) frame->ExecuteJavaScript(kBridge, frame->GetURL(), 0);
    }
    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
        renderer_->OnContextReleased(browser, frame, context);
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                                  CefRefPtr<CefProcessMessage> message) override {
        return renderer_->OnProcessMessageReceived(browser, frame, source, message);
    }
    void OnContextInitialized() override {
        g_router = CefMessageRouterBrowserSide::Create(CefMessageRouterConfig());
        g_router->AddHandler(&g_queryHandler, false);
        Send(sfb::kReady, 0, {});
        CreateThread(nullptr, 0, &ReaderThread, nullptr, 0, nullptr);
    }
private:
    CefRefPtr<CefMessageRouterRendererSide> renderer_;
    IMPLEMENT_REFCOUNTING(App);
};

std::wstring Switch(CefRefPtr<CefCommandLine> cl, const char* name) { return cl->GetSwitchValue(name).ToWString(); }

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    CefMainArgs args(instance);
    CefRefPtr<App> app = new App;
    const int code = CefExecuteProcess(args, app, nullptr);
    if (code >= 0) return code; // a CEF subprocess finished

    CefRefPtr<CefCommandLine> cl = CefCommandLine::CreateCommandLine();
    cl->InitFromString(GetCommandLineW());
    g_base = Switch(cl, "sf-base");
    if (g_base.empty()) {
        MessageBoxW(nullptr, L"sf_browser_host.exe is started by the sf_browser Stormworks mod.", L"sf_browser", MB_OK);
        return 1;
    }
    g_cmdPipe = CreateFileW(sfb::PipeName(g_base, L"_cmd").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    g_evtPipe = CreateFileW(sfb::PipeName(g_base, L"_evt").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_cmdPipe == INVALID_HANDLE_VALUE || g_evtPipe == INVALID_HANDLE_VALUE) return 2;

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop = false;
    settings.log_severity = LOGSEVERITY_WARNING;
    const std::wstring cache = Switch(cl, "sf-cache"), log = Switch(cl, "sf-log");
    if (!cache.empty()) { CefString(&settings.root_cache_path) = cache; CefString(&settings.cache_path) = cache; }
    if (!log.empty()) CefString(&settings.log_file) = log;
    if (!CefInitialize(args, settings, app, nullptr)) { Log("CefInitialize failed"); return 3; }
    CefRunMessageLoop();
    g_clients.clear();
    g_router = nullptr;
    CefShutdown();
    CloseHandle(g_cmdPipe);
    CloseHandle(g_evtPipe);
    return 0;
}
