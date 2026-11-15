// Validate file-backed PE data under simulated ASLR. No game code is executed.
#include "../src/engine_profile.h"
#include "../src/game.h"
#include "../src/native_video.h"
#include "../src/engine_bindings.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace swml {
void LogError(const char* f, ...) { va_list args;va_start(args,f);vprintf(f,args);puts("");va_end(args); }
void LogInfo(const char*, ...) {}
void Log(const char* f, ...) { va_list args;va_start(args,f);vprintf(f,args);puts("");va_end(args);fflush(stdout); }
void LogDiscoveryProgress(const char*,size_t,size_t,const char*,bool) {}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 3;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    assert(bytes.size() >= sizeof(IMAGE_DOS_HEADER));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    assert(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0);
    assert(static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= bytes.size());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
    assert(nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64);
    std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage, 0);
    assert(nt->OptionalHeader.SizeOfHeaders <= bytes.size() && nt->OptionalHeader.SizeOfHeaders <= image.size());
    memcpy(image.data(), bytes.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        assert(section[i].PointerToRawData + static_cast<size_t>(section[i].SizeOfRawData) <= bytes.size());
        assert(section[i].VirtualAddress + static_cast<size_t>(section[i].SizeOfRawData) <= image.size());
        memcpy(image.data() + section[i].VirtualAddress, bytes.data() + section[i].PointerToRawData,
               section[i].SizeOfRawData);
    }

    uint64_t delta = reinterpret_cast<uint64_t>(image.data()) - nt->OptionalHeader.ImageBase;
    const auto& reloc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    assert(reloc.VirtualAddress + static_cast<size_t>(reloc.Size) <= image.size());
    size_t pos = 0;
    while (pos < reloc.Size) {
        auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image.data() + reloc.VirtualAddress + pos);
        assert(block->SizeOfBlock >= 8 && pos + block->SizeOfBlock <= reloc.Size);
        auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
        for (size_t i = 0; i < (block->SizeOfBlock - 8) / 2; ++i) {
            if ((entries[i] >> 12) != IMAGE_REL_BASED_DIR64) continue;
            size_t address = block->VirtualAddress + (entries[i] & 0xFFF);
            assert(address + 8 <= image.size());
            uint64_t value;
            memcpy(&value, image.data() + address, 8);
            value += delta;
            memcpy(image.data() + address, &value, 8);
        }
        pos += block->SizeOfBlock;
    }

    const auto snapshot=image;
    assert(swml::ValidateReviewedEngineMemory(image.data()));
    sw::Api api; assert(sw::ResolveApiImage(api,image.data()));
    assert(api.base==image.data() && reinterpret_cast<uint8_t*>(api.make_button)==image.data()+swml::EngineRva(image.data(),0x262680));
    assert(image==snapshot);
    assert(swml::EngineRva(image.data(),0xdeadbeef)==0);
    // Changed callee witnesses and the independently reviewed constructor/layout
    // are rejected even if a top-level native function has not changed.
    size_t witness=swml::EngineRva(image.data(),0x1bd080);assert(witness);
    image[witness]^=1;assert(!swml::ValidateReviewedEngineMemory(image.data()));image[witness]^=1;
    size_t constructor=nt->FileHeader.TimeDateStamp==0x6a392154 ? 0x365b80 : 0x364e20;
    image[constructor+0x90]^=1;assert(!swml::ValidateEngineLayouts(image.data()));image[constructor+0x90]^=1;
    assert(swml::ValidateLuaProfile(image.data()));
    assert(swml::ValidateQolEditorProfile(image.data()));
    assert(swml::ValidateVideoProfile(image.data()));
    // Capability separation: a corrupted optional editor target rejects the
    // QoL/editor profile while the core Lua service stays available.
    size_t editorSlot = swml::EngineRva(image.data(), 0x63f550); assert(editorSlot);
    image[editorSlot] ^= 1;
    assert(!swml::ValidateQolEditorProfile(image.data()));
    assert(swml::ValidateLuaProfile(image.data()));
    image[editorSlot] ^= 1;
    assert(swml::ValidateQolEditorProfile(image.data()));
    size_t scriptTable = swml::EngineRva(image.data(), 0xb67728); assert(scriptTable);
    image[scriptTable + 8] ^= 1; // slot 1: editor hook conflict, not a core dependency
    assert(!swml::ValidateQolEditorProfile(image.data()));
    assert(swml::ValidateLuaProfile(image.data()));
    image[scriptTable + 8] ^= 1;
    assert(swml::ValidateQolEditorProfile(image.data()));
    // Addon/mission script VMs: newer reviewed build only, complete method bodies
    // and unhooked vtable slots required; independent of the core Lua service.
    const bool newerBuild = nt->FileHeader.TimeDateStamp == 0x6a9a9e20;
    swml::AddonLuaProfile addon{};
    assert(swml::ValidateAddonLuaProfile(image.data()) == newerBuild);
    assert(swml::ResolveAddonLuaProfile(image.data(), &addon) == newerBuild);
    assert(!swml::ResolveAddonLuaProfile(image.data(), nullptr) && !swml::ValidateAddonLuaProfile(nullptr));
    if (newerBuild) {
        assert(addon.vtable[1] == addon.close && addon.vtable[3] == addon.init);
        auto* close = static_cast<uint8_t*>(addon.close); auto* init = static_cast<uint8_t*>(addon.init);
        for (uint8_t* byte : {close, close + 47, init, init + 3000, init + 6373}) {
            *byte ^= 1;
            assert(!swml::ValidateAddonLuaProfile(image.data()) && swml::ValidateLuaProfile(image.data()));
            *byte ^= 1;
        }
        for (int slot : {1, 3}) {
            void* original = addon.vtable[slot];
            addon.vtable[slot] = image.data(); // an already hooked slot is a conflict, never replaced
            assert(!swml::ValidateAddonLuaProfile(image.data()) && swml::ValidateLuaProfile(image.data()));
            addon.vtable[slot] = original;
        }
        assert(swml::ValidateAddonLuaProfile(image.data()));
    }
    // Protected call: newer build only, complete lua_pcallk body including its tail chunks.
    auto* pcallk = static_cast<uint8_t*>(swml::ResolveLuaPcallk(image.data()));
    assert((pcallk != nullptr) == newerBuild && !swml::ResolveLuaPcallk(nullptr));
    if (pcallk) {
        for (size_t offset : {size_t(0), size_t(309), size_t(515)}) {
            pcallk[offset] ^= 1;
            assert(!swml::ResolveLuaPcallk(image.data()) && swml::ValidateLuaProfile(image.data()));
            pcallk[offset] ^= 1;
        }
        assert(swml::ResolveLuaPcallk(image.data()) == pcallk);
    }
    for (uint32_t key : {0x74a538,0x6d87d0,0x74aa50,0x688ec0,0x367c80,0xb70ea0}) {
        size_t rva = swml::EngineRva(image.data(),key); assert(rva);
        image[rva] ^= 1;
        assert(!swml::ValidateVideoProfile(image.data()));
        image[rva] ^= 1;
    }
    assert(swml::ValidateVideoProfile(image.data()));
    assert(sw::ValidateIngameNotificationProfile(image.data()));
    size_t editorInit=swml::EngineRva(image.data(),0x640e80),hudTable=swml::EngineRva(image.data(),0xb423c0),
        hudUpdate=swml::EngineRva(image.data(),0x5c78b0),editorTable=swml::EngineRva(image.data(),0xb71d60),luaTable=swml::EngineRva(image.data(),0xb6dac0);
    image[editorInit] ^= 1;
    assert(!sw::ValidateIngameNotificationProfile(image.data()));
    image[editorInit] ^= 1;
    image[hudTable + 8] ^= 1;
    assert(!sw::ValidateIngameNotificationProfile(image.data()));
    image[hudTable + 8] ^= 1;
    image[hudTable + 16] ^= 1;
    assert(!sw::ValidateIngameNotificationProfile(image.data()));
    image[hudTable + 16] ^= 1;
    image[hudUpdate] ^= 1;
    assert(!sw::ValidateIngameNotificationProfile(image.data()));
    image[hudUpdate] ^= 1;
    image[editorTable + 3 * 8] ^= 1;
    assert(!sw::ValidateIngameNotificationProfile(image.data()));
    image[editorTable + 3 * 8] ^= 1;
    assert(sw::ValidateIngameNotificationProfile(image.data()));
    image[0x097920] ^= 1;
    assert(!swml::ValidateLuaProfile(image.data()));
    image[0x097920] ^= 1;
    image[luaTable + 8] ^= 1;
    assert(!swml::ValidateLuaProfile(image.data()));
    image[luaTable + 8] ^= 1;
    auto* mappedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    mappedNt->FileHeader.TimeDateStamp ^= 1;
    assert(!swml::ValidateReviewedEngineMemory(image.data()));
    assert(!sw::ResolveApiImage(api,image.data()));
    assert(!swml::EngineRva(image.data(),0x97920));
    assert(!swml::ValidateLuaProfile(image.data()));
    assert(!swml::ValidateLuaProfile(nullptr));
    assert(!swml::ValidateAddonLuaProfile(image.data())); // wrong build identity
    assert(!swml::ResolveLuaPcallk(image.data()));
    assert(!swml::ValidateVideoProfile(image.data()));
    assert(!swml::ValidateVideoProfile(nullptr));
    assert(!sw::ValidateIngameNotificationProfile(image.data()));
    assert(!sw::ValidateIngameNotificationProfile(nullptr));
    puts("PASS: Lua, video and notification profiles + simulated ASLR; altered code/vtables and wrong build rejected");
}
