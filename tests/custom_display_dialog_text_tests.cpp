#include "../src/custom_display_dialog_text.h"
#include "../src/custom_display_dialog_model.h"
#include "../src/game.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace swml;
using Result = CustomDisplayDialogTextResult;
namespace {
struct Range { const void* base; size_t bytes; };
struct Memory {
    std::vector<Range> ranges;
    unsigned reads = 0;
    static bool Copy(void* context, const void* base, size_t offset, void* out, size_t bytes) {
        auto& self = *static_cast<Memory*>(context);
        ++self.reads;
        for (const auto& range : self.ranges) {
            if (range.base == base && offset <= range.bytes && bytes <= range.bytes - offset) {
                memcpy(out, static_cast<const uint8_t*>(base) + offset, bytes);
                return true;
            }
        }
        return false;
    }
};
template<class T, size_t N> void Put(std::array<uint8_t, N>& bytes, size_t at, T value) {
    assert(at <= N && sizeof(T) <= N - at);
    memcpy(bytes.data() + at, &value, sizeof(value));
}
}

int main() {
    static_assert(sizeof(sw::EStr) == 16, "native string descriptor");
    // Deliberately non-native offsets prove that reads use the derived layout,
    // not literals copied from the reference executable.
    CustomDisplayDialogWidgetLayout layout;
    layout.parent = 0x28;
    layout.editboxContent = 0x88;
    layout.liveText = 0xc8;
    std::array<std::array<uint8_t, 0xa0>, 4> boxes{};
    std::array<std::array<uint8_t, 0x140>, 4> content{};
    std::array<std::array<char, 32>, 4> text{};
    std::array<const void*, 4> widgets{};
    int owner = 1, otherOwner = 2, parent = 3, boxTable = 4, contentTable = 5;
    Memory memory;
    const std::array<const char*, 4> values{"2", "1", "480", "272"};
    for (size_t i = 0; i < boxes.size(); ++i) {
        widgets[i] = boxes[i].data();
        Put(boxes[i], 0, &boxTable);
        Put(boxes[i], layout.parent, &parent);
        Put(boxes[i], layout.editboxContent, content[i].data());
        // Initial text descriptor points to unreadable memory; must NOT be read.
        Put(boxes[i], 0x58, sw::EStr{reinterpret_cast<char*>(1), UINT32_MAX, 0});
        Put(content[i], 0, &contentTable);
        Put(content[i], layout.parent, boxes[i].data());
        strcpy_s(text[i].data(), text[i].size(), values[i]);
        Put(content[i], layout.liveText, sw::EStr{text[i].data(), uint32_t(strlen(values[i])), 32});
        memory.ranges.push_back({boxes[i].data(), boxes[i].size()});
        memory.ranges.push_back({content[i].data(), content[i].size()});
        memory.ranges.push_back({text[i].data(), text[i].size()});
    }
    CustomDisplayDialogTextAccess access{layout, &boxTable, &contentTable, &Memory::Copy, &memory};
    CustomDisplayDialogTextSession session;
    CustomDisplayDialogTextSnapshot snapshot;
    assert(session.Read(&owner, 1, 7, snapshot) == Result::Unavailable && !memory.reads);
    const auto first = session.Bind(&owner, 7, &parent, widgets, access);
    assert(first);
    assert(session.Read(&otherOwner, first, 7, snapshot) == Result::StaleOwner);
    assert(session.Read(&owner, first + 1, 7, snapshot) == Result::StaleOwner);
    assert(session.Read(&owner, first, 8, snapshot) == Result::WrongThread);
    assert(!memory.reads);
    assert(session.Read(&owner, first, 7, snapshot) == Result::Ok);
    CustomDisplayDialogRequest request;
    const CustomDisplayLimits limits{16,16,4096,4096,64*1024*1024};
    assert(ValidateCustomDisplayDialog(snapshot.Fields(), limits, request).Ok());
    assert(request.definitionId == "swml_cd_v1_b2x1_p480x272");
    auto original = snapshot;
    // Live input changes while wrapper's initial text remains untouched.
    text[2][0] = '5';
    assert(session.Read(&owner, first, 7, snapshot) == Result::Ok);
    assert(snapshot.Fields()[2] == "580" && original.Fields()[2] == "480");
    text[2][0] = '4';
    auto rejected = [&](Result expected) {
        snapshot = original;
        assert(session.Read(&owner, first, 7, snapshot) == expected);
        assert(snapshot.text == original.text && snapshot.length == original.length);
    };
    Put(boxes[3], 0, &contentTable); rejected(Result::WrongType);
    Put(boxes[3], 0, &boxTable);
    Put(content[3], 0, &boxTable); rejected(Result::WrongType);
    Put(content[3], 0, &contentTable);
    Put(boxes[3], layout.parent, &otherOwner); rejected(Result::WrongParent);
    Put(boxes[3], layout.parent, &parent);
    Put(content[3], layout.parent, boxes[0].data()); rejected(Result::WrongParent);
    Put(content[3], layout.parent, boxes[3].data());
    for (const auto& malformed : {sw::EStr{nullptr, 3, 32}, {nullptr, 0, 32},
            {text[3].data(), 3, 3}, {text[3].data(), 0, 0}}) {
        Put(content[3], layout.liveText, malformed); rejected(Result::InvalidString);
    }
    Put(content[3], layout.liveText, sw::EStr{reinterpret_cast<char*>(1), 11, 32});
    rejected(Result::TooLong); // Never touches the invalid text pointer.
    Put(content[3], layout.liveText, sw::EStr{reinterpret_cast<char*>(1), 3, 32});
    rejected(Result::Unreadable);
    Put(content[3], layout.liveText, sw::EStr{text[3].data(), 3, 32});
    text[3][3] = 'x'; rejected(Result::InvalidString); text[3][3] = '\0';
    // Embedded NUL remains in the bounded view and fails numeric validation.
    text[3][1] = '\0';
    assert(session.Read(&owner, first, 7, snapshot) == Result::Ok);
    assert(ValidateCustomDisplayDialog(snapshot.Fields(), limits, request).number ==
        CustomDisplayNumberError::InvalidCharacter);
    text[3][1] = '7';
    Put(content[3], layout.liveText, sw::EStr{});
    assert(session.Read(&owner, first, 7, snapshot) == Result::Ok && snapshot.Fields()[3].empty());
    assert(ValidateCustomDisplayDialog(snapshot.Fields(), limits, request).number ==
        CustomDisplayNumberError::Empty);
    Put(content[3], layout.liveText, sw::EStr{text[3].data(), 3, 32});
    const auto second = session.Bind(&owner, 7, &parent, widgets, access);
    assert(second != first);
    auto before = memory.reads;
    assert(session.Read(&owner, first, 7, snapshot) == Result::StaleOwner && memory.reads == before);
    assert(session.Read(&owner, second, 7, snapshot) == Result::Ok);
    session.Invalidate(); // Must precede native child destruction.
    memory.ranges.clear(); before = memory.reads;
    assert(session.Read(&owner, second, 7, snapshot) == Result::Unavailable && memory.reads == before);
    auto duplicate = widgets; duplicate[3] = duplicate[0];
    assert(!session.Bind(&owner, 7, &parent, duplicate, access));
    assert(!session.Bind(&owner, 0, &parent, widgets, access));
    assert(!session.Bind(nullptr, 7, &parent, widgets, access));
    puts("PASS: native live-text reader, copied independent fields, ownership/thread gate before reads, stale callbacks, parent/type checks, bounded/atomic malformed-memory rejection (fake native memory only)");
}
