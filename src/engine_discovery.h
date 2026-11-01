#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swml { namespace discovery {
enum class Status { Found, Missing, Ambiguous, Invalid };
struct Range { uint32_t start, size; uint32_t flags; };
struct Image {
    const uint8_t* base = nullptr;
    size_t size = 0;
    uint32_t timestamp = 0;
    uint32_t headers = 0;
    std::vector<Range> sections;
    bool Contains(uint32_t rva, size_t bytes) const;
    bool Executable(uint32_t rva, size_t bytes) const;
    bool ReadOnly(uint32_t rva, size_t bytes) const;
    bool Readable(uint32_t rva, size_t bytes) const;
    bool Pointer(uint32_t location, uint32_t& rva) const;
};
// An address operand's offsets are exported by Ghidra's x64 decoder, not guessed
// from opcode bytes at runtime. All other bytes (including field offsets) are fixed.
struct Edge { uint32_t offset, end, target; bool flow; };
struct RvaTable {
    uint32_t operand, baseOperand, baseEnd;
    const uint32_t* targetOffsets;
    size_t count;
};
struct Contract {
    uint32_t key, start, size, groups;
    const char* name;
    const char* bytes;
    const char* mask;
    const Edge* edges;
    size_t edgeCount;
    const RvaTable* rvaTables = nullptr;
    size_t rvaTableCount = 0;
};
struct Slot { uint32_t index, target; };
struct Table { uint32_t key, group; const char* type; const Slot* slots; size_t count; };
struct Symbol {
    uint32_t key = 0, rva = 0;
    std::string name;
    Status status = Status::Missing;
    std::vector<uint32_t> candidates;
    std::string reason;
};
struct Result {
    bool validImage = false;
    // Discovery is NOT a layout certificate. Until layout contracts are migrated,
    // an unknown build remains diagnostic-only and the old safety gate remains.
    bool reviewedBuild = false;
    std::vector<Symbol> symbols;
    uint32_t drawElementsIat = 0;
    uint32_t Address(uint32_t key) const {
        for (const auto& s : symbols) if (s.key == key && s.status == Status::Found) return s.rva;
        return 0;
    }
    bool GroupReady(uint32_t group) const;
};
using Progress = void (*)(const char* phase, size_t done, size_t total, const char* detail, bool failed);
bool ParseImage(const uint8_t* base, size_t readableBytes, Image& out);
bool Match(const Image& image, const Contract& contract, uint32_t start);
bool DecodeEdge(const Image& image, uint32_t start, const Edge& edge, uint32_t& target);
Symbol Find(const Image& image, const Contract& contract, bool reviewedFastPath, Progress progress = nullptr);
uint32_t FindImport(const Image& image, const char* dll, const char* name);
Result Discover(const uint8_t* base, size_t readableBytes, Progress progress = nullptr, bool forceScan = false);
// Production entry; uses the loaded module's readable image size. No hooks/writes.
Result DiscoverLoadedEngine(uint8_t* base);
} }
