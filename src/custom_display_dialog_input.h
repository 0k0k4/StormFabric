#pragma once
#include "custom_display_dialog_callbacks.h"

namespace swml {

using CustomDisplayNativeCharacter = uintptr_t (*)(void*, void*, void*, uint8_t);
using CustomDisplayNativeKey = uintptr_t (*)(void*, void*, void*, void*, uint32_t, uint32_t);
using CustomDisplayNativeMouse = bool (*)(void*, void*, void*, const float*, int32_t, int32_t);
using CustomDisplayNativePointer = uintptr_t (*)(void*, void*, uintptr_t, const float*, float, float, int32_t);
using CustomDisplayGetControls = void* (*)();
struct CustomDisplayDialogInputApi {
    CustomDisplayDialogInputLayout layout;
    const void* dialogTable = nullptr;
    CustomDisplayNativeCharacter character = nullptr;
    CustomDisplayNativeKey key = nullptr;
    CustomDisplayNativeMouse mouse = nullptr;
    CustomDisplayNativePointer pointer = nullptr;
    CustomDisplayGetControls controls = nullptr;
};
bool BindCustomDisplayDialogInputApi(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayDialogInputApi& out);

// A COMPLETE native input snapshot, not an OS keyboard poll or a claim that
// hooks/capture are certified. The native reader below still needs its owner hook.
// Must be obtained at the same owner-thread safe point as Begin/Release.
struct CustomDisplayHeldInput {
    // Covers the native window key domain (including codes above 255), NOT the
    // separately translated 256-code vehicle text-routing domain.
    std::array<uint64_t, 8> keys{};
    uint8_t mouseButtons = 0;
    bool complete = false;
    bool Neutral() const;
};

using CustomDisplayRetireNativeRecipients = void (*)(void*);
struct CustomDisplayNativeInputStateApi {
    CustomDisplayNativeInputStateLayout layout;
    // Native hover-leave/capture-loss notifications, NOT a pointer-latch reset.
    // Bound read-only; not automatically invoked by the reader or Begin.
    CustomDisplayRetireNativeRecipients retireRecipients = nullptr;
};
bool BindCustomDisplayNativeInputStateApi(const discovery::Image& image,
    const discovery::Result& result, CustomDisplayNativeInputStateApi& out);

// Reads every native window keyboard/mouse state, without polling the OS or
// consuming sticky states. The future authenticated owner hook supplies the
// exact window host and retires this reader BEFORE either native object dies.
// Same-thread/double reads detect drift, not cross-thread synchronization.
class CustomDisplayNativeHeldInputReader {
public:
    CustomDisplayNativeHeldInputReader() = default;
    CustomDisplayNativeHeldInputReader(const CustomDisplayNativeHeldInputReader&) = delete;
    CustomDisplayNativeHeldInputReader& operator=(const CustomDisplayNativeHeldInputReader&) = delete;
    bool Bind(const void* owner, uint32_t thread, const void* windowHost,
        const CustomDisplayNativeInputStateApi& api, CustomDisplayDialogCopy copy, void* copyContext);
    bool Read(const void* owner, uint32_t thread, CustomDisplayHeldInput& out);
    bool BeforeOwnerTeardown(const void* owner, uint32_t thread);
private:
    bool Copy(const void* base, size_t offset, void* out, size_t bytes) const;
    bool Snapshot(CustomDisplayHeldInput& out) const;
    const void* owner_ = nullptr;
    const void* host_ = nullptr;
    const void* window_ = nullptr;
    uint32_t thread_ = 0;
    CustomDisplayNativeInputStateLayout layout_;
    CustomDisplayDialogCopy copy_ = nullptr;
    void* copyContext_ = nullptr;
    bool busy_ = false, retired_ = false;
};
enum class CustomDisplayInputResult { PassThrough, Consumed, NativeClosed, Fault };

// Per-opening, single-owner native-input adapter, NOT an installed hook. It
// never invokes the vehicle shortcut/placement handlers: only native editbox or
// root UI helpers. All matching input is consumed, regardless of native return.
class CustomDisplayDialogInputSession {
public:
    CustomDisplayDialogInputSession() = default;
    CustomDisplayDialogInputSession(const CustomDisplayDialogInputSession&) = delete;
    CustomDisplayDialogInputSession& operator=(const CustomDisplayDialogInputSession&) = delete;
    bool Begin(const void* owner, uint32_t thread, uint64_t token,
        CustomDisplayDialogCallbackLease lease, const CustomDisplayDialogNativeTree& tree,
        const CustomDisplayDialogInputApi& api, const CustomDisplayDialogTextAccess& access,
        const CustomDisplayHeldInput& held);
    // After Show and native basic update have published this modal. Until then
    // capture consumes without routing, and callback tickets remain disarmed.
    bool Activate(const void* owner, uint32_t thread, uint64_t token, const CustomDisplayHeldInput& held);
    CustomDisplayInputResult Character(const void* owner, uint32_t thread, uint64_t token, uint8_t value);
    CustomDisplayInputResult Key(const void* owner, uint32_t thread, uint64_t token,
        void* application, uint32_t key, uint32_t phase);
    CustomDisplayInputResult Command(const void* owner, uint32_t thread, uint64_t token,
        uint32_t kind, uint32_t code, uint32_t phase);
    CustomDisplayInputResult Mouse(const void* owner, uint32_t thread, uint64_t token,
        void* application, void* nativeInput, const float* point, int32_t button, int32_t phase);
    CustomDisplayInputResult Pointer(const void* owner, uint32_t thread, uint64_t token,
        void* application, const float* point, float dx, float dy, int32_t flags);
    // Consume any additional input boundary (wheel, drag, etc.) while captured;
    // the future hook transaction must include ALL such boundaries.
    CustomDisplayInputResult Consume(const void* owner, uint32_t thread, uint64_t token) const;
    // Owner-thread safe point only, outside native input/update traversal.
    // Observes native close BEFORE draining. Keeps input captured throughout
    // validation/native close; only failed validation resumes local editing.
    // Native closure maps to Cancelled, a routing fault to Stale (no request).
    CustomDisplayDialogDispatchResult Drain(const void* owner, uint32_t thread, uint64_t token,
        CustomDisplayDialogController& controller, const CustomDisplayLimits& limits,
        CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& request);
    // Owner safe-point coordinator using the bound NATIVE reader, not a caller's
    // complete flag: wait/activate, deferred drain, native retirement, release.
    // Source failure keeps capture and pending commands; no validated request.
    // Still not a production hook or a placement/registration authorization.
    CustomDisplayDialogDispatchResult Advance(const void* owner, uint32_t thread, uint64_t token,
        CustomDisplayNativeHeldInputReader& source, const CustomDisplayNativeInputStateApi& native,
        CustomDisplayDialogController& controller, const CustomDisplayLimits& limits,
        CustomDisplayDialogValidation& validation, CustomDisplayDialogRequest& request);
    bool BeforeClose(const void* owner, uint32_t thread, uint64_t token);
    // Closed and unpublished tree only, neutral snapshot and zero native latch.
    // Sends the engine's capture-loss/hover-leave notifications to OUR retained
    // recipients. Never clears fields directly or notifies a stock recipient.
    bool RetireRecipients(const void* owner, uint32_t thread, uint64_t token,
        const CustomDisplayNativeInputStateApi& native, const CustomDisplayHeldInput& held);
    // Also waits for native modal unpublication and recipient/hover retirement;
    // a neutral device snapshot alone must never release a still-published tree.
    bool Release(const void* owner, uint32_t thread, uint64_t token, const CustomDisplayHeldInput& held);
    bool BeforeOwnerTeardown(const void* owner, uint32_t thread);
private:
    enum class Phase { Unbound, Opening, Active, Draining, Faulted, Retired };
    bool Matches(const void* owner, uint32_t thread, uint64_t token) const;
    CustomDisplayInputResult Guard(const void* owner, uint32_t thread, uint64_t token);
    bool Copy(const void* base, size_t offset, void* out, size_t bytes) const;
    bool Content(const void* candidate) const;
    bool Focus(void*& out) const;
    bool Descendant(const void* candidate, const void* parent) const;
    CustomDisplayInputResult Fault();
    const void* owner_ = nullptr;
    uint32_t thread_ = 0;
    uint64_t token_ = 0;
    CustomDisplayDialogCallbackLease lease_ = 0;
    void* dialog_ = nullptr;
    void* root_ = nullptr;
    void* context_ = nullptr;
    void* queue_ = nullptr;
    std::array<const void*, 4> boxes_{};
    std::array<void*, 256> keyRecipients_{};
    std::array<bool, 256> keysDown_{};
    CustomDisplayDialogInputApi api_;
    CustomDisplayDialogTextAccess access_;
    Phase phase_ = Phase::Unbound;
    bool busy_ = false;
};

} // namespace swml
