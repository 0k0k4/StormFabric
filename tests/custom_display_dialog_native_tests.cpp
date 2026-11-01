// Exercise real construction/controller code against fake engine functions.
// No game process, native hooks, archive registration or placement.
#include "../src/custom_display_dialog_controller.h"
#include "../src/custom_display_engine_string.h"
#include "../src/custom_display_catalogue.h"
#include "../src/custom_display_dialog_callbacks.h"
#include "../src/custom_display_dialog_input.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <thread>

using namespace swml;
namespace {
struct Object {
    size_t size = 0;
    unsigned init = 0;
    bool dialog = false, open = false;
    std::string initial, allowed;
    void* content = nullptr;
};
std::map<void*, Object> objects;
std::map<void*, size_t> strings;
unsigned allocations = 0, reserves = 0, failAllocation = 0, failReserve = 0, openCalls = 0, reads = 0;
int dialogTable, boxTable, contentTable;
int buttonTable;
bool callbackOnly = false;
unsigned recipientRetireCalls = 0;
void* heldDuringRetirement = nullptr;
CustomDisplayDialogCallbackLayout buttons{0xe0, 0xe8, 0xb0, 0x90, 0x98, 0x58, 0x5c};
CustomDisplayDialogWidgetLayout layout;
std::string titleSeen, bodySeen;
CustomDisplayDialogController* reentrant = nullptr;
const void* reentrantOwner = nullptr;
uint64_t reentrantToken = 0;
using NativeCallback = void (*)(void*, void*);
NativeCallback closingCallback = nullptr;
void* closingContext = nullptr, *closingData = nullptr;
void (*duringClose)() = nullptr;

template<class T> void Put(void* memory, size_t offset, T value) {
    auto it = objects.find(memory);
    assert(it != objects.end() && offset + sizeof(T) <= it->second.size);
    std::memcpy(static_cast<uint8_t*>(memory) + offset, &value, sizeof(T));
}
void Reserve(sw::EStr* value, uint32_t bytes) {
    assert(!value->data && bytes);
    if (++reserves == failReserve) return;
    value->cap = bytes < 32 ? 32 : bytes;
    value->data = static_cast<char*>(std::calloc(value->cap, 1));
    assert(value->data);
    strings.emplace(value->data, value->cap);
}
void Free(void* memory) {
    assert(strings.erase(memory) == 1); // Detect wrong allocator/double frees.
    std::free(memory);
}
std::string ReadString(sw::EStr* text) {
    assert(text && strings.count(text->data) && text->cap > text->len && !text->data[text->len]);
    return {text->data, text->len};
}
std::string Consume(sw::EStr* text) {
    auto copy = ReadString(text);
    Free(text->data);
    text->len = text->cap = 0;
    // Deliberately leave a dangling data pointer, just like the native consumer.
    return copy;
}
void* Raw(size_t bytes) {
    void* memory = std::calloc(bytes, 1);
    assert(memory);
    objects.emplace(memory, Object{bytes});
    return memory;
}
void* New(size_t bytes) {
    if (++allocations == failAllocation) return nullptr;
    return Raw(bytes);
}
void* Basic(void* memory, sw::EStr* title, sw::EStr* body, uint8_t cancel, uint8_t confirm,
            int32_t confirmAction, int32_t cancelAction) {
    assert(cancel == 1 && confirm == 1 && confirmAction == (callbackOnly ? 0 : 1001) && cancelAction == (callbackOnly ? 0 : 1002));
    assert(objects.at(memory).size == 0x180); // Deliberately shifted allocation.
    titleSeen = ReadString(title); bodySeen = ReadString(body); // Borrow, do NOT consume.
    objects.at(memory).dialog = true;
    Put(memory, 0, &dialogTable);
    return memory;
}
void* Edit(void* memory, float x, float y, float w, float h, int32_t anchor,
           sw::EStr* text, int32_t action) {
    assert(x == .40f && y < 0 && w == .32f && h == .12f && anchor == 0 && action == (callbackOnly ? 0 : 1003));
    assert(objects.at(memory).size == 0xa0);
    objects.at(memory).initial = Consume(text);
    Put(memory, 0, &boxTable);
    return memory;
}
void* Add(void* parent, void* context, void* child) {
    assert(parent && context && child);
    auto& object = objects.at(child);
    assert(++object.init == 1); // Must not initialize a detached tree then re-init.
    Put(child, layout.parent, parent);
    if (object.dialog) {
        object.open = false;
        if (callbackOnly) {
            for (uint32_t offset : {buttons.confirmButton, buttons.cancelButton}) {
                void* button = Raw(buttons.buttonBytes);
                Put(button, 0, &buttonTable); Put(button, layout.parent, child);
                Put(child, offset, button);
            }
        }
        return child;
    }
    object.content = Raw(0x140);
    Put(child, layout.editboxContent, object.content);
    Put(object.content, 0, &contentTable);
    Put(object.content, layout.parent, child);
    // Allocate the fake live buffer independently of failure-injected reserve.
    auto* data = static_cast<char*>(std::calloc(32, 1));
    strings.emplace(data, 32);
    std::memcpy(data, object.initial.c_str(), object.initial.size() + 1);
    Put(object.content, layout.liveText, sw::EStr{data, uint32_t(object.initial.size()), 32});
    return child;
}
void Allowed(void* child, sw::EStr* text) {
    auto& object = objects.at(child);
    assert(object.init == 1 && object.content); // Filter only after content exists.
    object.allowed = Consume(text);
}
void SetOpen(void* dialog, uint8_t open, uint8_t immediate) {
    assert(objects.at(dialog).dialog && immediate == 1);
    objects.at(dialog).open = open != 0;
    Put(dialog, layout.dialogOpen, open);
    ++openCalls;
    if (!open && closingCallback) closingCallback(closingContext, closingData);
    if (!open && duringClose) duringClose();
    if (!open && reentrant) {
        assert(!reentrant->Current(reentrantOwner, 7, reentrantToken));
        assert(!reentrant->Cancel(reentrantOwner, 7, reentrantToken));
    }
}
bool Copy(void*, const void* base, size_t offset, void* out, size_t bytes) {
    ++reads;
    auto object = objects.find(const_cast<void*>(base));
    const auto text = strings.find(const_cast<void*>(base));
    size_t size = object != objects.end() ? object->second.size : text != strings.end() ? text->second : 0;
    if (!size || offset > size || bytes > size - offset) return false;
    std::memcpy(out, static_cast<const uint8_t*>(base) + offset, bytes);
    return true;
}
void Reset() {
    for (const auto& pair : objects) std::free(pair.first);
    for (const auto& pair : strings) std::free(pair.first);
    objects.clear(); strings.clear();
    allocations = reserves = failAllocation = failReserve = openCalls = reads = 0;
    titleSeen.clear(); bodySeen.clear(); reentrant = nullptr;
    callbackOnly = false;
    closingCallback = nullptr; closingContext = closingData = nullptr;
    duringClose = nullptr;
}
CustomDisplayDialogNativeApi Api() {
    CustomDisplayDialogNativeApi api;
    api.engine.op_new = &New; api.engine.str_reserve = &Reserve;
    api.engine.str_free = &Free; api.engine.add_child = &Add;
    api.makeDialog = &Basic; api.makeEditbox = &Edit;
    api.setAllowed = &Allowed; api.setOpen = &SetOpen;
    api.dialogBytes = 0x180; api.editboxBytes = 0xa0;
    return api;
}
CustomDisplayDialogBuildResult Build(CustomDisplayDialogNativeTree& tree) {
    return BuildCustomDisplayNativeDialog(Api(), &dialogTable, &contentTable, 1001, 1002, 1003, tree);
}
void NoTemporaryLeaks(const CustomDisplayDialogNativeTree& tree) {
    size_t children = 0;
    for (auto* box : tree.editboxes) if (box) ++children;
    assert(strings.size() == children); // Only the native live buffers remain.
    if (tree.dialog) assert(!objects.at(tree.dialog).open);
}
void SetText(const void* box, const char* value) {
    void* content = objects.at(const_cast<void*>(box)).content;
    sw::EStr text{};
    assert(Copy(nullptr, content, layout.liveText, &text, sizeof(text)));
    assert(std::strlen(value) < text.cap);
    std::memcpy(text.data, value, std::strlen(value) + 1);
    text.len = uint32_t(std::strlen(value));
    Put(content, layout.liveText, text);
}
}

namespace {
unsigned characters = 0, keys = 0, mouseCalls = 0, pointerCalls = 0;
void* lastRecipient = nullptr;
void* controlsObject = nullptr;
CustomDisplayDialogInputSession* inputReentrant = nullptr;
const void* inputOwner = nullptr;
uint32_t inputThread = 0;
uint64_t inputToken = 0;
bool InputCopy(void* context, const void* base, size_t offset, void* out, size_t bytes) {
    if (Copy(context, base, offset, out, bytes)) return true;
    const auto address = reinterpret_cast<uintptr_t>(base);
    for (const auto& object : objects) {
        const auto start = reinterpret_cast<uintptr_t>(object.first);
        if (address < start || address - start >= object.second.size) continue;
        auto remaining = object.second.size - (address - start);
        if (offset > remaining || bytes > remaining - offset) return false;
        std::memcpy(out, static_cast<const uint8_t*>(base) + offset, bytes);
        return true;
    }
    return false;
}
uintptr_t InputCharacter(void* recipient, void*, void*, uint8_t) {
    ++characters; lastRecipient = recipient;
    if (inputReentrant) {
        assert(inputReentrant->Character(inputOwner, inputThread, inputToken, '2') == CustomDisplayInputResult::Consumed);
        assert(!inputReentrant->BeforeOwnerTeardown(inputOwner, inputThread));
    }
    return 0;
}
uintptr_t InputKey(void* recipient, void*, void*, void*, uint32_t key, uint32_t) {
    assert(key != 61 && key != 94); // Shifted fixture submit keys must NEVER reach native text.
    ++keys; lastRecipient = recipient; return 0;
}
bool InputMouse(void*, void*, void*, const float*, int32_t, int32_t) { ++mouseCalls; return false; }
uintptr_t InputPointer(void*, void*, uintptr_t scratch, const float*, float, float, int32_t) {
    assert(!scratch); ++pointerCalls; return 0;
}
void* InputControls() { return controlsObject; }
void CheckClosingCapture() {
    assert(inputReentrant && inputReentrant->Character(inputOwner, inputThread, inputToken, '3') == CustomDisplayInputResult::Consumed);
    assert(!inputReentrant->BeforeOwnerTeardown(inputOwner, inputThread));
}
void InputRetireRecipients(void* root) {
    assert(inputOwner && root == static_cast<const uint8_t*>(inputOwner) + 0x80);
    ++recipientRetireCalls;
    CheckClosingCapture();
    Put(const_cast<void*>(inputOwner), 0xd0 + 0x28, static_cast<void*>(nullptr));
    Put(const_cast<void*>(inputOwner), 0x80 + 0x180, static_cast<void*>(nullptr));
    if (heldDuringRetirement) Put(heldDuringRetirement, 0x100 + 348, uint8_t(1));
}
struct HeldCopyContext {
    CustomDisplayNativeHeldInputReader* reader = nullptr;
    const void* owner = nullptr;
    uint32_t thread = 0;
    void* window = nullptr;
    uint32_t keyStates = 0;
    bool drift = false, reenter = false;
};
bool HeldCopy(void* context, const void* base, size_t offset, void* out, size_t bytes) {
    auto& state = *static_cast<HeldCopyContext*>(context);
    const bool copied = Copy(nullptr, base, offset, out, bytes);
    if (state.reenter) {
        CustomDisplayHeldInput nested;
        assert(!state.reader->Read(state.owner, state.thread, nested) && !nested.complete);
        assert(!state.reader->BeforeOwnerTeardown(state.owner, state.thread));
    }
    if (state.drift && base == state.window && offset == state.keyStates) {
        state.drift = false;
        Put(state.window, state.keyStates + 348, uint8_t(1));
    }
    return copied;
}
void TestHeldReader() {
    Reset();
    auto* owner = Raw(0x20);
    auto* host = Raw(0x40);
    auto* window = Raw(0x300);
    auto* replacement = Raw(0x300);
    const auto thread = GetCurrentThreadId();
    CustomDisplayNativeInputStateApi api;
    api.layout.hostWindow = 0x20; api.layout.keyStates = 0x100; api.layout.keyCount = 349;
    api.layout.mouseStates = 0x60; api.layout.mouseCount = 8; // Deliberately shifted, no runtime constants.
    Put(host, api.layout.hostWindow, window);
    CustomDisplayNativeHeldInputReader reader;
    HeldCopyContext copy{&reader, owner, thread, window, api.layout.keyStates};
    CustomDisplayHeldInput sample; sample.complete = true;
    assert(!reader.Read(owner, thread, sample) && !sample.complete);
    for (auto field : {0u, 513u}) {
        auto invalid = api; invalid.layout.keyCount = field;
        assert(!reader.Bind(owner, thread, host, invalid, &HeldCopy, &copy));
    }
    auto invalid = api; invalid.layout.mouseStates = invalid.layout.keyStates - 4;
    assert(!reader.Bind(owner, thread, host, invalid, &HeldCopy, &copy));
    assert(reader.Bind(owner, thread, host, api, &HeldCopy, &copy));
    assert(!reader.Bind(owner, thread, host, api, &HeldCopy, &copy));
    assert(reader.Read(owner, thread, sample) && sample.Neutral());
    for (uint32_t key = 0; key < api.layout.keyCount; ++key) {
        Put(window, api.layout.keyStates + key, uint8_t(1));
        assert(reader.Read(owner, thread, sample) && !sample.Neutral());
        assert(sample.keys[key / 64] == (uint64_t(1) << (key % 64)));
        Put(window, api.layout.keyStates + key, uint8_t(0));
    }
    for (uint32_t button = 0; button < api.layout.mouseCount; ++button) {
        Put(window, api.layout.mouseStates + button, uint8_t(1));
        assert(reader.Read(owner, thread, sample) && sample.mouseButtons == (1u << button) && !sample.Neutral());
        Put(window, api.layout.mouseStates + button, uint8_t(0));
    }
    Put(window, api.layout.keyStates + 348, uint8_t(3));
    assert(reader.Read(owner, thread, sample) && !sample.Neutral()); // Sticky is not consumed.
    Put(window, api.layout.keyStates + 348, uint8_t(2));
    assert(!reader.Read(owner, thread, sample) && !sample.complete);
    Put(window, api.layout.keyStates + 348, uint8_t(0));
    Put(window, api.layout.mouseStates + 7, uint8_t(3));
    assert(reader.Read(owner, thread, sample) && sample.mouseButtons == 128);
    Put(window, api.layout.mouseStates + 7, uint8_t(255));
    assert(!reader.Read(owner, thread, sample) && !sample.complete);
    Put(window, api.layout.mouseStates + 7, uint8_t(0));
    auto before = reads;
    assert(!reader.Read(host, thread, sample) && !sample.complete && reads == before);
    std::thread wrong([&] {
        CustomDisplayHeldInput other;
        assert(!reader.Read(owner, thread, other) && !other.complete);
        assert(!reader.BeforeOwnerTeardown(owner, thread));
    }); wrong.join();
    assert(reads == before);
    copy.reenter = true;
    assert(reader.Read(owner, thread, sample) && sample.Neutral());
    copy.reenter = false; copy.drift = true;
    assert(!reader.Read(owner, thread, sample) && !sample.complete);
    Put(window, api.layout.keyStates + 348, uint8_t(0));
    Put(host, api.layout.hostWindow, replacement);
    assert(!reader.Read(owner, thread, sample) && !sample.complete);
    Put(host, api.layout.hostWindow, window);
    objects.at(window).size = api.layout.keyStates + api.layout.keyCount - 1;
    assert(!reader.Read(owner, thread, sample) && !sample.complete); // No truncated success.
    before = reads;
    assert(reader.BeforeOwnerTeardown(owner, thread) && reads == before);
    assert(!reader.Read(owner, thread, sample) && !sample.complete && reads == before);
    assert(!reader.Bind(owner, thread, host, api, &HeldCopy, &copy));
    Reset();
    puts("PASS: complete native-window keyboard/mouse reader, all 349 keys/eight buttons, sticky/invalid states, drift, exact host pin, thread/reentrancy/teardown gates (fake memory, no installed source)");
}
void TestInput() {
    using R = CustomDisplayInputResult;
    Reset(); callbackOnly = true;
    layout.contextModal = 0x38; layout.rootContext = 0x50;
    auto* state = Raw(0x600);
    auto* root = static_cast<uint8_t*>(state) + 0x80;
    auto* context = static_cast<uint8_t*>(state) + 0xd0;
    const auto thread = GetCurrentThreadId();
    CustomDisplayDialogNativeTree tree;
    assert(BuildCustomDisplayCallbackDialog(Api(), root, context, tree) == CustomDisplayDialogBuildResult::Ok);
    CustomDisplayDialogTextAccess access{layout, &boxTable, &contentTable, &InputCopy, nullptr};
    CustomDisplayDialogCallbackAccess callbacks{layout, buttons, 0x180, &dialogTable, &buttonTable, &InputCopy, nullptr};
    CustomDisplayDialogInputApi api;
    api.layout = {0x80, 0xd0, 0x300, 0x28, 0x20, 0x28, 61, 94};
    api.layout.rootHover = 0x180; api.layout.rootPointerHeld = 0x488;
    api.dialogTable = &dialogTable;
    api.character = &InputCharacter; api.key = &InputKey; api.mouse = &InputMouse;
    api.pointer = &InputPointer; api.controls = &InputControls;
    CustomDisplayNativeInputStateApi retirement;
    retirement.layout.rootContext = layout.rootContext;
    retirement.layout.rootRecipient = layout.rootContext + api.layout.contextFocus;
    retirement.layout.rootHover = api.layout.rootHover;
    retirement.layout.rootPointerHeld = api.layout.rootPointerHeld;
    retirement.retireRecipients = &InputRetireRecipients;
    controlsObject = Raw(0x40); Put(controlsObject, 0x20, uint32_t(3)); Put(controlsObject, 0x28, uint32_t(17));
    auto lease = ReserveCustomDisplayDialogCallbacks(state, thread, context, 19);
    assert(lease && AttachCustomDisplayDialogCallbacks(lease, state, thread, tree, callbacks));
    CustomDisplayHeldInput neutral; neutral.complete = true;
    CustomDisplayHeldInput held = neutral; held.keys[3] = 1;
    for (size_t word = 4; word < held.keys.size(); ++word) {
        auto highKey = neutral; highKey.keys[word] = uint64_t(1) << 63;
        assert(!highKey.Neutral());
    }
    CustomDisplayDialogInputSession input;
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, {}));
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, held));
    held = neutral; held.mouseButtons = 1;
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, held));
    assert(!input.Begin(state, thread, 20, lease, tree, api, access, neutral));
    assert(!input.Begin(state, thread, 19, lease + 1, tree, api, access, neutral));
    Put(state, 0x80 + api.layout.rootPointerHeld, uint8_t(1));
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, neutral));
    Put(state, 0x80 + api.layout.rootPointerHeld, uint8_t(0));
    Put(state, 0xd0 + api.layout.contextFocus, controlsObject);
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, neutral));
    Put(state, 0xd0 + api.layout.contextFocus, static_cast<void*>(nullptr));
    Put(state, 0x80 + api.layout.rootHover, controlsObject);
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, neutral));
    Put(state, 0x80 + api.layout.rootHover, static_cast<void*>(nullptr));
    assert(input.Begin(state, thread, 19, lease, tree, api, access, neutral));
    assert(!input.Begin(state, thread, 19, lease, tree, api, access, neutral));
    auto openingReads = reads;
    assert(input.Character(state, thread, 19, '1') == R::Consumed && reads == openingReads);
    assert(!input.Activate(state, thread, 19, neutral));
    SetOpen(tree.dialog, 1, 1);
    assert(!input.Activate(state, thread, 19, neutral)); // Wait for actual modal publication, not just open flag.
    Put(state, 0xd0 + layout.contextModal, tree.dialog);
    auto* first = objects.at(const_cast<void*>(tree.editboxes[0])).content;
    auto* second = objects.at(const_cast<void*>(tree.editboxes[1])).content;
    Put(state, 0xd0 + api.layout.contextFocus, first);
    assert(!QueueCustomDisplayDialogCommand(lease, state, thread, 19, CustomDisplayDialogCommand::Confirm));
    assert(!input.Activate(state, thread, 19, held));
    assert(!input.Activate(state, thread, 19, {}));
    Put(state, 0x80 + api.layout.rootPointerHeld, uint8_t(1));
    assert(!input.Activate(state, thread, 19, neutral));
    Put(state, 0x80 + api.layout.rootPointerHeld, uint8_t(0));
    Put(state, 0xd0 + api.layout.contextFocus, controlsObject);
    assert(!input.Activate(state, thread, 19, neutral));
    Put(state, 0xd0 + api.layout.contextFocus, first);
    Put(state, 0x80 + api.layout.rootHover, controlsObject);
    assert(!input.Activate(state, thread, 19, neutral));
    Put(state, 0x80 + api.layout.rootHover, static_cast<void*>(nullptr));
    assert(input.Activate(state, thread, 19, neutral));
    assert(!input.Activate(state, thread, 19, neutral));
    const unsigned beforeReads = reads;
    assert(input.Character(state, thread, 20, '1') == R::PassThrough && reads == beforeReads);
    assert(input.Character(root, thread, 19, '1') == R::PassThrough && reads == beforeReads);
    std::thread wrong([&] { assert(input.Character(state, thread, 19, '1') == R::PassThrough); }); wrong.join();
    assert(reads == beforeReads);
    inputReentrant = &input; inputOwner = state; inputThread = thread; inputToken = 19;
    assert(input.Character(state, thread, 19, '1') == R::Consumed && characters == 1 && lastRecipient == first);
    inputReentrant = nullptr;
    assert(input.Key(state, thread, 19, state, 10, 1) == R::Consumed && keys == 1);
    Put(state, 0xd0 + api.layout.contextFocus, second);
    assert(input.Key(state, thread, 19, state, 10, 1) == R::Consumed && keys == 1);
    assert(input.Key(state, thread, 19, state, 10, 0) == R::Consumed && keys == 3 && lastRecipient == second);
    CustomDisplayDialogCallbackEvent event;
    assert(!QueueCustomDisplayDialogCommand(lease, state, thread, 20, CustomDisplayDialogCommand::Cancel));
    assert(!QueueCustomDisplayDialogCommand(lease, state, thread, 19, CustomDisplayDialogCommand::None));
    assert(input.Key(state, thread, 19, nullptr, 61, 1) == R::Consumed && keys == 3);
    assert(input.Command(state, thread, 19, 3, 17, 0) == R::Consumed);
    assert(TakeCustomDisplayDialogCallback(lease, state, thread, event) && event.command == CustomDisplayDialogCommand::Confirm);
    assert(ArmCustomDisplayDialogCallbacks(lease, state, thread));
    assert(input.Key(state, thread, 19, nullptr, 61, 1) == R::Consumed); // Repeat cannot resubmit.
    assert(!TakeCustomDisplayDialogCallback(lease, state, thread, event));
    assert(input.Key(state, thread, 19, nullptr, 61, 0) == R::Consumed);
    assert(input.Key(state, thread, 19, nullptr, 94, 1) == R::Consumed);
    assert(input.Command(state, thread, 19, 3, 17, 1) == R::Consumed);
    assert(input.Key(state, thread, 19, nullptr, 61, 1) == R::Consumed);
    assert(TakeCustomDisplayDialogCallback(lease, state, thread, event) && event.command == CustomDisplayDialogCommand::Cancel);
    assert(ArmCustomDisplayDialogCallbacks(lease, state, thread));
    auto* point = Raw(2 * sizeof(float)); Put(point, 0, 0.2f); Put(point, sizeof(float), 0.3f);
    assert(input.Mouse(state, thread, 19, state, state, static_cast<float*>(point), 0, 1) == R::Consumed && mouseCalls == 1);
    assert(input.Pointer(state, thread, 19, state, static_cast<float*>(point), 0.f, 0.f, 0) == R::Consumed && pointerCalls == 1);
    auto* foreign = Raw(0x80); // Foreign root must not receive a held release.
    Put(state, 0xd0 + api.layout.contextFocus, foreign);
    assert(input.Mouse(state, thread, 19, state, state, static_cast<float*>(point), 0, 0) == R::Consumed && mouseCalls == 1);
    assert(input.Mouse(state, thread, 19, state, state, static_cast<float*>(point), 0, 1) == R::Consumed && mouseCalls == 1);
    assert(input.BeforeClose(state, thread, 19));
    assert(!input.RetireRecipients(state, thread, 19, retirement, neutral)); // Still open.
    SetOpen(tree.dialog, 0, 1);
    auto calls = characters + keys + mouseCalls + pointerCalls;
    assert(input.Character(state, thread, 19, '1') == R::Consumed);
    assert(!input.Release(state, thread, 19, {}));
    assert(!input.Release(state, thread, 19, held));
    assert(input.Consume(state, thread, 19) == R::Consumed);
    assert(!input.Release(state, thread, 19, neutral)); // Closed flag is not modal unpublication.
    assert(!input.RetireRecipients(state, thread, 19, retirement, neutral));
    Put(state, 0xd0 + layout.contextModal, static_cast<void*>(nullptr));
    assert(!input.Release(state, thread, 19, neutral)); // Old recipient must be retired natively.
    assert(!input.RetireRecipients(state, thread, 19, retirement, neutral)); // Never notify a foreign stock recipient.
    Put(state, 0xd0 + api.layout.contextFocus, static_cast<void*>(nullptr));
    Put(state, 0x80 + api.layout.rootHover, first);
    assert(!input.Release(state, thread, 19, neutral));
    Put(state, 0x80 + api.layout.rootHover, static_cast<void*>(nullptr));
    Put(state, 0x80 + api.layout.rootPointerHeld, uint8_t(1));
    assert(!input.Release(state, thread, 19, neutral));
    assert(!input.RetireRecipients(state, thread, 19, retirement, neutral)); // Native latch is never cleared manually.
    Put(state, 0x80 + api.layout.rootPointerHeld, uint8_t(0));
    Put(state, 0xd0 + api.layout.contextFocus, first);
    Put(state, 0x80 + api.layout.rootHover, second);
    assert(!input.RetireRecipients(state, thread, 19, retirement, held));
    auto badRetirement = retirement; ++badRetirement.layout.rootRecipient;
    assert(!input.RetireRecipients(state, thread, 19, badRetirement, neutral));
    inputReentrant = &input; inputOwner = state; inputThread = thread; inputToken = 19;
    const auto retiresBefore = recipientRetireCalls;
    assert(input.RetireRecipients(state, thread, 19, retirement, neutral));
    assert(recipientRetireCalls == retiresBefore + 1);
    assert(input.RetireRecipients(state, thread, 19, retirement, neutral));
    assert(recipientRetireCalls == retiresBefore + 1); // Empty root: no repeated native notifications.
    inputReentrant = nullptr;
    assert(input.Release(state, thread, 19, neutral));
    assert(input.Consume(state, thread, 19) == R::PassThrough);
    assert(calls == characters + keys + mouseCalls + pointerCalls);
    assert(!QueueCustomDisplayDialogCommand(lease, state, thread, 19, CustomDisplayDialogCommand::Confirm));
    // Separate fresh sessions exercise native close, fault capture and teardown.
    for (int scenario = 0; scenario != 3; ++scenario) {
        tree = {};
        assert(BuildCustomDisplayCallbackDialog(Api(), root, context, tree) == CustomDisplayDialogBuildResult::Ok);
        CustomDisplayDialogInputSession next;
        auto ticket = ReserveCustomDisplayDialogCallbacks(state, thread, context, 20 + scenario);
        assert(AttachCustomDisplayDialogCallbacks(ticket, state, thread, tree, callbacks));
        Put(state, 0xd0 + api.layout.contextFocus, static_cast<void*>(nullptr));
        assert(next.Begin(state, thread, 20 + scenario, ticket, tree, api, access, neutral));
        SetOpen(tree.dialog, 1, 1);
        Put(state, 0xd0 + layout.contextModal, tree.dialog);
        assert(next.Activate(state, thread, 20 + scenario, neutral));
        if (scenario == 0) {
            SetOpen(tree.dialog, 0, 1);
            assert(next.Character(state, thread, 20, '1') == R::NativeClosed);
            assert(!next.Release(state, thread, 20, neutral));
            Put(state, 0xd0 + layout.contextModal, static_cast<void*>(nullptr));
            assert(next.Release(state, thread, 20, neutral));
        } else {
            SetOpen(tree.dialog, 1, 1);
            Put(state, 0xd0 + layout.contextModal, foreign);
            if (scenario == 1) {
                assert(next.Character(state, thread, 21, '1') == R::Fault);
                assert(next.Key(state, thread, 21, state, 10, 1) == R::Consumed);
                assert(!next.Release(state, thread, 21, neutral));
            }
            auto readsAtTeardown = reads;
            assert(next.BeforeOwnerTeardown(state, thread));
            assert(reads == readsAtTeardown && next.Consume(state, thread, 20 + scenario) == R::PassThrough);
        }
    }
    for (int scenario = 0; scenario != 4; ++scenario) {
        tree = {};
        Put(state, 0xd0 + api.layout.contextFocus, static_cast<void*>(nullptr));
        assert(BuildCustomDisplayCallbackDialog(Api(), root, context, tree) == CustomDisplayDialogBuildResult::Ok);
        CustomDisplayDialogController controller;
        auto token = controller.Prepare(state, thread, tree, access, &SetOpen);
        auto ticket = ReserveCustomDisplayDialogCallbacks(state, thread, context, token);
        assert(AttachCustomDisplayDialogCallbacks(ticket, state, thread, tree, callbacks));
        CustomDisplayDialogInputSession integrated;
        assert(integrated.Begin(state, thread, token, ticket, tree, api, access, neutral));
        assert(controller.Show(state, thread, token));
        Put(state, 0xd0 + layout.contextModal, tree.dialog);
        assert(integrated.Activate(state, thread, token, neutral));
        const CustomDisplayLimits limits{16, 16, 4096, 4096, 64 * 1024 * 1024};
        CustomDisplayDialogValidation validation;
        CustomDisplayDialogRequest request; request.definitionId = "unchanged";
        using D = CustomDisplayDialogDispatchResult;
        assert(integrated.Drain(state, thread, token, controller, limits, validation, request) == D::None);
        assert(integrated.Key(state, thread, token, nullptr, 61, 1) == R::Consumed);
        if (scenario == 1) {
            SetText(tree.editboxes[0], "0");
            assert(integrated.Drain(state, thread, token, controller, limits, validation, request) == D::InvalidValues);
            assert(controller.Current(state, thread, token) && objects.at(tree.dialog).open && request.definitionId == "unchanged");
            assert(!integrated.Release(state, thread, token, neutral));
            SetText(tree.editboxes[0], "2");
            assert(integrated.Key(state, thread, token, nullptr, 61, 0) == R::Consumed);
            assert(integrated.Key(state, thread, token, nullptr, 61, 1) == R::Consumed);
        }
        if (scenario == 2) assert(integrated.Command(state, thread, token, 3, 17, 1) == R::Consumed);
        if (scenario == 3) SetOpen(tree.dialog, 0, 1); // Pending confirm must lose to native closure.
        inputReentrant = &integrated; inputOwner = state; inputThread = thread; inputToken = token;
        duringClose = &CheckClosingCapture;
        const auto callsBefore = characters + keys + mouseCalls + pointerCalls;
        auto drained = integrated.Drain(state, thread, token, controller, limits, validation, request);
        assert(drained == (scenario < 2 ? D::Confirmed : D::Cancelled));
        assert(!controller.Current(state, thread, token) && !objects.at(tree.dialog).open);
        assert(request.definitionId == (scenario == 0 ? "swml_cd_v1_b1x1_p480x272" :
            scenario == 1 ? "swml_cd_v1_b2x1_p480x272" : "unchanged"));
        assert(characters + keys + mouseCalls + pointerCalls == callsBefore);
        duringClose = nullptr; inputReentrant = nullptr;
        assert(integrated.Consume(state, thread, token) == R::Consumed);
        assert(!integrated.Release(state, thread, token, neutral));
        Put(state, 0xd0 + layout.contextModal, static_cast<void*>(nullptr));
        assert(integrated.Release(state, thread, token, neutral));
        assert(!QueueCustomDisplayDialogCommand(ticket, state, thread, token, CustomDisplayDialogCommand::Confirm));
    }
    { // Complete source-backed safe-point flow, no caller-supplied complete flag.
        Put(state, 0xd0 + api.layout.contextFocus, static_cast<void*>(nullptr));
        tree = {};
        assert(BuildCustomDisplayCallbackDialog(Api(), root, context, tree) == CustomDisplayDialogBuildResult::Ok);
        auto* host = Raw(0x40);
        auto* window = Raw(0x300);
        auto sourceApi = retirement;
        sourceApi.layout.hostWindow = 0x20; sourceApi.layout.keyStates = 0x100; sourceApi.layout.keyCount = 349;
        sourceApi.layout.mouseStates = 0x60; sourceApi.layout.mouseCount = 8;
        Put(host, sourceApi.layout.hostWindow, window);
        CustomDisplayNativeHeldInputReader source;
        assert(source.Bind(state, thread, host, sourceApi, &InputCopy, nullptr));
        CustomDisplayHeldInput initial;
        assert(source.Read(state, thread, initial) && initial.Neutral());
        CustomDisplayDialogController controller;
        auto token = controller.Prepare(state, thread, tree, access, &SetOpen);
        auto ticket = ReserveCustomDisplayDialogCallbacks(state, thread, context, token);
        assert(AttachCustomDisplayDialogCallbacks(ticket, state, thread, tree, callbacks));
        CustomDisplayDialogInputSession integrated;
        assert(integrated.Begin(state, thread, token, ticket, tree, api, access, initial));
        const CustomDisplayLimits limits{16, 16, 4096, 4096, 64 * 1024 * 1024};
        CustomDisplayDialogValidation validation;
        CustomDisplayDialogRequest request; request.definitionId = "unchanged";
        using D = CustomDisplayDialogDispatchResult;
        assert(controller.Show(state, thread, token));
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(!QueueCustomDisplayDialogCommand(ticket, state, thread, token, CustomDisplayDialogCommand::Confirm));
        Put(state, 0xd0 + layout.contextModal, tree.dialog);
        Put(window, sourceApi.layout.keyStates + 348, uint8_t(1));
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(!QueueCustomDisplayDialogCommand(ticket, state, thread, token, CustomDisplayDialogCommand::Confirm));
        Put(window, sourceApi.layout.keyStates + 348, uint8_t(0));
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(integrated.Key(state, thread, token, nullptr, 61, 1) == R::Consumed);
        Put(window, sourceApi.layout.keyStates + 348, uint8_t(2)); // Incomplete source cannot dispatch pending confirmation.
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(controller.Current(state, thread, token) && request.definitionId == "unchanged");
        Put(window, sourceApi.layout.keyStates + 348, uint8_t(1));
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::Confirmed);
        assert(request.definitionId == "swml_cd_v1_b1x1_p480x272" && integrated.Consume(state, thread, token) == R::Consumed);
        Put(state, 0xd0 + layout.contextModal, static_cast<void*>(nullptr));
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(integrated.Consume(state, thread, token) == R::Consumed); // High native key still held.
        Put(window, sourceApi.layout.keyStates + 348, uint8_t(0));
        first = objects.at(const_cast<void*>(tree.editboxes[0])).content;
        Put(state, 0xd0 + api.layout.contextFocus, first);
        Put(state, 0x80 + api.layout.rootHover, first);
        inputReentrant = &integrated; inputOwner = state; inputThread = thread; inputToken = token;
        heldDuringRetirement = window;
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        inputReentrant = nullptr;
        heldDuringRetirement = nullptr;
        assert(integrated.Consume(state, thread, token) == R::Consumed); // Native callback invalidated the earlier neutral sample.
        Put(window, sourceApi.layout.keyStates + 348, uint8_t(0));
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(integrated.Consume(state, thread, token) == R::PassThrough);
        auto readsBefore = reads;
        assert(integrated.Advance(state, thread, token, source, sourceApi, controller, limits, validation, request) == D::None);
        assert(reads == readsBefore && source.BeforeOwnerTeardown(state, thread));
    }
    Reset();
    puts("PASS: source-backed safe-point activation/drain/retirement/release, high-key barriers, invalid-source pending-command retention and no post-release reads (fake engine only)");
    puts("PASS: local native-input adapter, submit suppression, configured Escape/cancel priority, focus/repeat/release routing, reentrancy, faults and neutral barriers (fake engine only)");
    puts("PASS: integrated input/deferred-controller drain, invalid-value retry, input blocked during native close, Escape precedence and held-close capture (fake engine only)");
    puts("PASS: modal-unpublication/recipient/hover/latch release barriers and native recipient retirement under reentrant capture (fake engine only)");
}
}

int main() {
    TestHeldReader();
    layout.parent = 0x28; layout.editboxContent = 0x88; layout.liveText = 0xc8; layout.dialogOpen = 0x40;
    TestInput();
    auto api = Api();
    { // Explicit engine string ownership, embedded NUL rejection and empty NUL buffer.
        CustomDisplayEngineString value(api.engine);
        assert(value.Assign("borrowed") && ReadString(value.Borrow()) == "borrowed");
        assert(!value.Assign(std::string_view("a\0b", 3)));
        assert(ReadString(value.Borrow()) == "borrowed");
        assert(value.Assign("") && value.Borrow()->data && !value.Borrow()->len);
        value.Consume([](sw::EStr* text) { assert(Consume(text).empty()); });
        assert(!value.Borrow()->data);
        assert(value.Assign("exception"));
        try {
            value.Consume([](sw::EStr* text) { Consume(text); throw 7; });
            assert(false);
        } catch (int error) { assert(error == 7 && !value.Borrow()->data); }
    }
    assert(strings.empty());
    Reset();
    CustomDisplayDialogNativeTree tree;
    assert(Build(tree) == CustomDisplayDialogBuildResult::Ok && tree.complete);
    assert(titleSeen == "Custom Display" && bodySeen.find("Width (pixels)") != std::string::npos);
    const char* defaults[] = {"1", "1", "480", "272"};
    for (size_t i = 0; i < 4; ++i) {
        const auto& box = objects.at(const_cast<void*>(tree.editboxes[i]));
        assert(box.initial == defaults[i] && box.allowed == "0123456789");
    }
    NoTemporaryLeaks(tree);
    auto before = allocations;
    assert(Build(tree) == CustomDisplayDialogBuildResult::InvalidArguments && allocations == before);

    CustomDisplayDialogTextAccess access{layout, &boxTable, &contentTable, &Copy, nullptr};
    CustomDisplayDialogController controller;
    int owner = 0, other = 1;
    const CustomDisplayLimits limits{16, 16, 4096, 4096, 64 * 1024 * 1024};
    auto wrongAccess = access; wrongAccess.editboxTable = &contentTable;
    auto callsBefore = openCalls;
    assert(!controller.Open(&owner, 7, tree, wrongAccess, &SetOpen));
    assert(openCalls == callsBefore && !objects.at(tree.dialog).open);
    auto token = controller.Open(&owner, 7, tree, access, &SetOpen);
    assert(token && objects.at(tree.dialog).open);
    assert(!controller.Open(&owner, 7, tree, access, &SetOpen));
    CustomDisplayDialogValidation validation;
    CustomDisplayDialogRequest request;
    request.definitionId = "unchanged";
    auto readsBefore = reads;
    assert(controller.Confirm(&other, 7, token, limits, validation, request) == CustomDisplayDialogConfirmResult::Stale);
    assert(controller.Confirm(&owner, 8, token, limits, validation, request) == CustomDisplayDialogConfirmResult::Stale);
    assert(reads == readsBefore && request.definitionId == "unchanged");
    SetText(tree.editboxes[0], "0");
    assert(controller.Confirm(&owner, 7, token, limits, validation, request) == CustomDisplayDialogConfirmResult::InvalidValues);
    assert(validation.field == 0 && objects.at(tree.dialog).open && request.definitionId == "unchanged");
    SetText(tree.editboxes[0], "2");
    const void* lastBox = tree.editboxes[3];
    Put(const_cast<void*>(lastBox), layout.parent, &other);
    assert(controller.Confirm(&owner, 7, token, limits, validation, request) == CustomDisplayDialogConfirmResult::TextUnavailable);
    assert(objects.at(tree.dialog).open && request.definitionId == "unchanged");
    Put(const_cast<void*>(lastBox), layout.parent, tree.dialog);
    // Even valid input cannot bypass a disabled policy.
    assert(controller.Confirm(&owner, 7, token, {}, validation, request) == CustomDisplayDialogConfirmResult::InvalidValues);
    reentrant = &controller; reentrantOwner = &owner; reentrantToken = token;
    assert(controller.Confirm(&owner, 7, token, limits, validation, request) == CustomDisplayDialogConfirmResult::Ok);
    assert(request.definitionId == "swml_cd_v1_b2x1_p480x272" && !objects.at(tree.dialog).open);
    readsBefore = reads;
    assert(controller.Confirm(&owner, 7, token, limits, validation, request) == CustomDisplayDialogConfirmResult::Stale);
    assert(reads == readsBefore);
    auto second = controller.Open(&owner, 7, tree, access, &SetOpen);
    assert(second > token && !controller.Cancel(&owner, 7, token));
    assert(!controller.OwnerLeaving(&owner, 8));
    reentrantToken = second;
    assert(controller.OwnerLeaving(&owner, 7) && !objects.at(tree.dialog).open);
    auto third = controller.Open(&owner, 7, tree, access, &SetOpen);
    auto calls = openCalls;
    assert(third > second && controller.NativeClosed(&owner, 7, third));
    assert(openCalls == calls); // Native Escape already closed it, do not call twice.
    auto fourth = controller.Open(&owner, 7, tree, access, &SetOpen);
    assert(fourth > third && !controller.BeforeOwnerTeardown(&other, 7));
    calls = openCalls;
    assert(controller.BeforeOwnerTeardown(&owner, 7) && openCalls == calls);
    Reset(); // Native parent deletion; all old pointers now unreadable.
    readsBefore = reads;
    assert(controller.Confirm(&owner, 7, fourth, limits, validation, request) == CustomDisplayDialogConfirmResult::Stale);
    assert(reads == readsBefore);
    assert(!controller.Open(&owner, 7, tree, access, &SetOpen)); // No resurrection after teardown.

    Reset(); tree = {};
    auto incompleteApi = Api(); incompleteApi.makeEditbox = nullptr;
    assert(BuildCustomDisplayNativeDialog(incompleteApi, &owner, &other, 1001, 1002, 1003, tree) ==
           CustomDisplayDialogBuildResult::InvalidArguments && !allocations);
    assert(BuildCustomDisplayNativeDialog(Api(), &owner, &other, 1001, 1001, 1003, tree) ==
           CustomDisplayDialogBuildResult::InvalidArguments && !allocations);

    for (unsigned failure = 1; failure <= 5; ++failure) {
        Reset(); failAllocation = failure; tree = {};
        assert(Build(tree) == CustomDisplayDialogBuildResult::AllocationFailed && !tree.complete);
        NoTemporaryLeaks(tree);
    }
    for (unsigned failure = 1; failure <= 10; ++failure) {
        Reset(); failReserve = failure; tree = {};
        assert(Build(tree) == CustomDisplayDialogBuildResult::StringFailed && !tree.complete);
        NoTemporaryLeaks(tree);
    }
    Reset();
    { // Real callback binding, immutable tickets and deferred commands.
        callbackOnly = true; tree = {};
        assert(BuildCustomDisplayCallbackDialog(Api(), &owner, &other, tree) == CustomDisplayDialogBuildResult::Ok);
        const auto thread = GetCurrentThreadId();
        CustomDisplayDialogController deferred;
        auto prepared = deferred.Prepare(&owner, thread, tree, access, &SetOpen);
        assert(prepared && !objects.at(tree.dialog).open && !deferred.Current(&owner, thread, prepared));
        assert(deferred.Confirm(&owner, thread, prepared, limits, validation, request) == CustomDisplayDialogConfirmResult::Stale);
        assert(!deferred.Cancel(&owner, thread, prepared));
        auto lease = ReserveCustomDisplayDialogCallbacks(&owner, thread, &other, prepared);
        assert(lease && !ReserveCustomDisplayDialogCallbacks(&owner, thread + 1, &other, prepared));
        CustomDisplayDialogCallbackAccess callbacks{layout, buttons, 0x180, &dialogTable, &buttonTable, &Copy, nullptr};
        readsBefore = reads;
        assert(!AttachCustomDisplayDialogCallbacks(lease, &other, thread, tree, callbacks));
        assert(!AttachCustomDisplayDialogCallbacks(lease, &owner, thread + 1, tree, callbacks));
        assert(reads == readsBefore);
        void* confirm = nullptr, *cancel = nullptr;
        assert(Copy(nullptr, tree.dialog, buttons.confirmButton, &confirm, sizeof(confirm)));
        assert(Copy(nullptr, tree.dialog, buttons.cancelButton, &cancel, sizeof(cancel)));
        using Callback = void (*)(void*, void*);
        Callback click = nullptr;
        void* confirmData = nullptr, *cancelData = nullptr;
        Put(cancel, buttons.releaseAction, int32_t(123));
        assert(!AttachCustomDisplayDialogCallbacks(lease, &owner, thread, tree, callbacks));
        assert(Copy(nullptr, confirm, buttons.callback, &click, sizeof(click)) && !click); // No partial writes.
        Put(cancel, buttons.releaseAction, int32_t(0));
        Put(cancel, layout.parent, &owner);
        assert(!AttachCustomDisplayDialogCallbacks(lease, &owner, thread, tree, callbacks));
        Put(cancel, layout.parent, tree.dialog);
        assert(AttachCustomDisplayDialogCallbacks(lease, &owner, thread, tree, callbacks));
        assert(!AttachCustomDisplayDialogCallbacks(lease, &owner, thread, tree, callbacks));
        assert(Copy(nullptr, confirm, buttons.callback, &click, sizeof(click)) && click);
        assert(Copy(nullptr, confirm, buttons.userData, &confirmData, sizeof(confirmData)));
        assert(Copy(nullptr, cancel, buttons.userData, &cancelData, sizeof(cancelData)) && cancelData != confirmData);
        CustomDisplayDialogCallbackEvent event{999, CustomDisplayDialogCommand::Cancel};
        readsBefore = reads; callsBefore = openCalls;
        click(&other, confirmData); // Not armed; opening click cannot confirm.
        assert(!TakeCustomDisplayDialogCallback(lease, &owner, thread, event) && event.token == 999);
        assert(deferred.Show(&owner, thread, prepared) && !deferred.Show(&owner, thread, prepared));
        assert(ArmCustomDisplayDialogCallbacks(lease, &owner, thread));
        click(&owner, confirmData); // Wrong native context.
        click(&other, &owner); // Foreign userdata, never dereferenced.
        std::thread foreign([&] { click(&other, confirmData); }); foreign.join();
        assert(!TakeCustomDisplayDialogCallback(lease, &owner, thread, event));
        click(&other, confirmData); click(&other, confirmData);
        assert(reads == readsBefore && openCalls == callsBefore + 1); // Thunk never reads/closes widgets.
        assert(TakeCustomDisplayDialogCallback(lease, &owner, thread, event));
        assert(event.token == prepared && event.command == CustomDisplayDialogCommand::Confirm);
        click(&other, confirmData);
        assert(!TakeCustomDisplayDialogCallback(lease, &owner, thread, event)); // Drain disarms.
        SetText(tree.editboxes[0], "0");
        assert(ArmCustomDisplayDialogCallbacks(lease, &owner, thread)); // Explicit same-token retry.
        click(&other, confirmData);
        assert(DispatchCustomDisplayDialogCallback(lease, &owner, thread, deferred, limits, validation, request) == CustomDisplayDialogDispatchResult::InvalidValues);
        assert(objects.at(tree.dialog).open && validation.field == 0);
        click(&other, confirmData); click(&other, cancelData); click(&other, confirmData);
        assert(DispatchCustomDisplayDialogCallback(lease, &owner, thread, deferred, limits, validation, request) == CustomDisplayDialogDispatchResult::Cancelled);
        assert(!objects.at(tree.dialog).open);
        auto nextLease = ReserveCustomDisplayDialogCallbacks(&owner, thread, &other, prepared); // Even same token/addresses.
        assert(nextLease > lease);
        assert(!AttachCustomDisplayDialogCallbacks(nextLease, &owner, thread, tree, callbacks)); // Never rebind old tree.
        assert(RetireCustomDisplayDialogCallbacks(nextLease, &owner, thread));
        Reset(); // Parent deleted; callbacks still safe after ALL widgets disappear.
        readsBefore = reads;
        click(&other, confirmData); click(&other, cancelData);
        assert(!TakeCustomDisplayDialogCallback(lease, &owner, thread, event) && reads == readsBefore);
        assert(!ArmCustomDisplayDialogCallbacks(lease, &owner, thread));
        assert(!RetireCustomDisplayDialogCallbacks(lease, &owner, thread));

        callbackOnly = true; tree = {};
        assert(BuildCustomDisplayCallbackDialog(Api(), &owner, &other, tree) == CustomDisplayDialogBuildResult::Ok);
        CustomDisplayDialogController replacement;
        auto replacementToken = replacement.Prepare(&owner, thread, tree, access, &SetOpen);
        auto replacementLease = ReserveCustomDisplayDialogCallbacks(&owner, thread, &other, replacementToken);
        assert(replacementLease > nextLease && replacementToken == prepared); // Owner address AND token reused.
        assert(AttachCustomDisplayDialogCallbacks(replacementLease, &owner, thread, tree, callbacks));
        assert(replacement.Show(&owner, thread, replacementToken));
        assert(ArmCustomDisplayDialogCallbacks(replacementLease, &owner, thread));
        click(&other, confirmData); // Old opening must never acquire replacementToken.
        assert(DispatchCustomDisplayDialogCallback(replacementLease, &owner, thread, replacement, limits, validation, request) == CustomDisplayDialogDispatchResult::None);
        assert(Copy(nullptr, tree.dialog, buttons.confirmButton, &confirm, sizeof(confirm)));
        void* replacementData = nullptr;
        assert(Copy(nullptr, confirm, buttons.userData, &replacementData, sizeof(replacementData)) && replacementData != confirmData);
        closingCallback = click; closingContext = &other; closingData = replacementData;
        click(&other, replacementData);
        assert(DispatchCustomDisplayDialogCallback(replacementLease, &owner, thread, replacement, limits, validation, request) == CustomDisplayDialogDispatchResult::Confirmed);
        assert(request.definitionId == "swml_cd_v1_b1x1_p480x272" && !objects.at(tree.dialog).open);
        assert(DispatchCustomDisplayDialogCallback(replacementLease, &owner, thread, replacement, limits, validation, request) == CustomDisplayDialogDispatchResult::None);
        Reset();
        // Bounded process-retained identities, never refund/reuse retired slots.
        unsigned count = 0;
        while (auto fresh = ReserveCustomDisplayDialogCallbacks(&owner, thread, &other, 1)) {
            assert(RetireCustomDisplayDialogCallbacks(fresh, &owner, thread));
            ++count;
        }
        assert(count == 256 - replacementLease && !ReserveCustomDisplayDialogCallbacks(&owner, thread, &other, 1));
    }
    Reset();
    { // Exact catalogue route; shifted layouts, no title/MD5/name heuristics.
        CustomDisplayCatalogueLayout selected{0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50};
        void* state = Raw(0x80), *ui = Raw(0x80), *catalogue = Raw(0x80);
        void* view = Raw(0x80), *list = Raw(0x80), *item = Raw(0x80);
        void* entry = Raw(0x80), *stockEntry = Raw(0x80);
        Put(state, selected.stateUi, ui); Put(ui, selected.uiCatalogue, catalogue);
        Put(catalogue, selected.catalogueView, view); Put(view, selected.viewList, list);
        Put(list, selected.listSelectedItem, item); Put(item, selected.itemEntry, entry);
        Put(entry, selected.entryDefinition, &owner);
        Put(stockEntry, selected.entryDefinition, &owner); // Same definition, different row.
        CustomDisplayCatalogueTarget target{state, 7, ui, catalogue, entry, &owner};
        auto click = [&](int32_t action = 0x61) {
            return ReadCustomDisplayCatalogueClick(target, state, 7, action, selected, &Copy, nullptr);
        };
        using Click = CustomDisplayCatalogueClick;
        readsBefore = reads;
        for (int32_t action : {0, 8, 18, 34, 96, 98, 100, 102}) assert(click(action) == Click::PassThrough);
        assert(ReadCustomDisplayCatalogueClick(target, state, 8, 0x61, selected, &Copy, nullptr) == Click::PassThrough);
        assert(ReadCustomDisplayCatalogueClick(target, &other, 7, 0x61, selected, &Copy, nullptr) == Click::PassThrough);
        assert(reads == readsBefore);
        assert(click() == Click::OpenRequested);
        Put(item, selected.itemEntry, stockEntry); assert(click() == Click::PassThrough);
        Put(item, selected.itemEntry, entry);
        Put(entry, 0, uint32_t(2)); assert(click() == Click::PassThrough);
        Put(entry, 0, uint32_t(0)); Put(entry, selected.entryDefinition, &other);
        assert(click() == Click::PassThrough);
        Put(entry, selected.entryDefinition, &owner);
        Put(list, selected.listSelectedItem, static_cast<void*>(nullptr)); assert(click() == Click::PassThrough);
        Put(list, selected.listSelectedItem, item);
        Put(catalogue, selected.catalogueView, &other); assert(click() == Click::Unreadable);
        Put(catalogue, selected.catalogueView, view);
        selected.itemEntry = 0; assert(click() == Click::Unreadable);
        selected.itemEntry = 0x48;
        target.entry = nullptr; readsBefore = reads;
        assert(click() == Click::PassThrough && reads == readsBefore);
    }
    Reset();
    puts("PASS: native four-field construction, consuming vs borrowed engine strings, closed partial trees, live validation/cancel/leave/Escape, stale/reentrant/wrong-thread callbacks and teardown (fake engine only)");
    puts("PASS: catalogue 0x61 exact owner/UI/row/definition route, shifted offsets, stock/hotbar/other actions untouched (fake memory only)");
    puts("PASS: closed preparation, callback-only buttons, immutable retained tickets, deferred/coalesced commands, cancel priority, wrong context/thread, no rebind and bounded exhaustion (fake engine only)");
}
