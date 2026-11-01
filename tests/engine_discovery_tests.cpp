// Read-only discovery: mapped executable bytes, never execute native functions.
#include "../src/engine_discovery.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <chrono>

namespace swml {
void LogError(const char*, ...) {}
void LogDiscoveryProgress(const char*,size_t,size_t,const char*,bool) {}
}
using namespace swml::discovery;
#include "../src/engine_discovery.generated.h"
static size_t ExpectedPrimaryCount() {
    size_t count = 0;
    for (const auto& contract : kDiscoveryContracts) if (contract.groups) ++count;
    return count;
}
static size_t g_lastFunctionDone=0;
static void TestProgress(const char* phase,size_t done,size_t total,const char* detail,bool failed) {
    if(strcmp(phase,"PE image")==0 && done==0)g_lastFunctionDone=0;
    if(strcmp(phase,"Functions")==0) {
        assert(total==ExpectedPrimaryCount() && done>=g_lastFunctionDone && done<=total);
        g_lastFunctionDone=done;
    }
    // This diagnostic executable also lets a developer inspect discovery without launching the game.
    if(strncmp(detail,"scanning executable sections",28)!=0 && (failed || done==0 || done==total))
        printf("[%s] %zu/%zu %s%s\n",phase,done,total,failed?"! ":"",detail);
}
static std::vector<uint8_t> MapPe(const char* path) {
    std::ifstream f(path,std::ios::binary);assert(f);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)),{});
    assert(file.size()>sizeof(IMAGE_DOS_HEADER));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    assert(dos->e_lfanew>0 && size_t(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)<=file.size());
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data()+dos->e_lfanew);
    std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage);
    assert(nt->OptionalHeader.SizeOfHeaders<=file.size());
    memcpy(image.data(),file.data(),nt->OptionalHeader.SizeOfHeaders);
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& s=sections[i];
        assert(size_t(s.PointerToRawData)+s.SizeOfRawData<=file.size());
        assert(size_t(s.VirtualAddress)+s.SizeOfRawData<=image.size());
        memcpy(image.data()+s.VirtualAddress,file.data()+s.PointerToRawData,s.SizeOfRawData);
    }
    uint64_t delta=reinterpret_cast<uint64_t>(image.data())-nt->OptionalHeader.ImageBase;
    const auto& reloc=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    assert(size_t(reloc.VirtualAddress)+reloc.Size<=image.size());
    size_t offset=0;
    while(offset<reloc.Size) {
        auto* block=reinterpret_cast<IMAGE_BASE_RELOCATION*>(image.data()+reloc.VirtualAddress+offset);
        assert(block->SizeOfBlock>=8 && offset+block->SizeOfBlock<=reloc.Size);
        auto* entries=reinterpret_cast<uint16_t*>(block+1);
        for(size_t i=0;i<(block->SizeOfBlock-8)/2;++i) {
            if((entries[i]>>12)!=IMAGE_REL_BASED_DIR64)continue;
            uint32_t rva=block->VirtualAddress+(entries[i]&0xfff);
            assert(size_t(rva)+8<=image.size());uint64_t value=0;
            memcpy(&value,image.data()+rva,8);value+=delta;memcpy(image.data()+rva,&value,8);
        }
        offset+=block->SizeOfBlock;
    }
    return image;
}
static void Synthetic() {
    std::vector<uint8_t> bytes(8192,0xcc);
    Image image;image.base=bytes.data();image.size=bytes.size();
    image.sections={{512,2048,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ},
                    {4096,1024,IMAGE_SCN_MEM_READ}};
    const char code[]="\x48\x89\x5c\x24\x18\xe8\x00\x00\x00\x00\xc3";
    const char mask[]="\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\xff";
    Edge edge={6,10,1000,true};
    Contract c={600,600,11,1,"test_moved_function",code,mask,&edge,1};
    memcpy(bytes.data()+800,code,11);
    int32_t displacement=1000-(800+10);memcpy(bytes.data()+806,&displacement,4);
    auto found=Find(image,c,false);assert(found.status==Status::Found && found.rva==800);
    uint32_t target=0;assert(DecodeEdge(image,800,edge,target) && target==1000);
    displacement=1200-(800+10);memcpy(bytes.data()+806,&displacement,4);
    assert(Match(image,c,800));assert(DecodeEdge(image,800,edge,target) && target==1200);
    memcpy(bytes.data()+1000,bytes.data()+800,11);
    auto duplicate=Find(image,c,false);assert(duplicate.status==Status::Ambiguous && duplicate.candidates.size()==2);
    bytes[1000]=0xcc;bytes[803]^=1;
    assert(Find(image,c,false).status==Status::Missing);
    bytes[803]^=1;memset(bytes.data()+800,0xcc,11);memcpy(bytes.data()+4096,code,11);
    assert(Find(image,c,false).status==Status::Missing); // never scan data as code
    memcpy(bytes.data()+2558,code,11);assert(!Match(image,c,2558)); // crosses .text boundary
    Edge invalid={8,10,0,true};assert(!DecodeEdge(image,800,invalid,target));
    displacement=INT32_MIN;memcpy(bytes.data()+806,&displacement,4);
    assert(!DecodeEdge(image,800,edge,target));
    Image bad;assert(!ParseImage(bytes.data(),bytes.size(),bad));
    printf("PASS: moved functions, masked rel32 decoding, unique/duplicate/missing patterns, executable boundaries\n");
    // Indexed image-base-relative jump tables may move, but every destination
    // and the image-base-producing instruction must still be corroborated.
    char tableCode[64],tableMask[64];memset(tableCode,0x90,64);memset(tableMask,0xff,64);
    memset(tableMask+6,0,4);memset(tableMask+24,0,4);
    const uint32_t targets[]={40,44};RvaTable typedTable={24,6,10,targets,2};
    Contract typed={800,800,64,1,"typed_table",tableCode,tableMask,nullptr,0,&typedTable,1};
    memcpy(bytes.data()+800,tableCode,64);
    displacement=-(800+10);memcpy(bytes.data()+806,&displacement,4);
    uint32_t tableRva=4096;memcpy(bytes.data()+824,&tableRva,4);
    uint32_t entries[]={840,844};memcpy(bytes.data()+4096,entries,8);
    assert(Match(image,typed,800));
    tableRva=4160;memcpy(bytes.data()+824,&tableRva,4);memcpy(bytes.data()+4160,entries,8);
    assert(Match(image,typed,800));
    uint32_t wrong=848;memcpy(bytes.data()+4164,&wrong,4);assert(!Match(image,typed,800));
    memcpy(bytes.data()+4160,entries,8);displacement+=1;memcpy(bytes.data()+806,&displacement,4);
    assert(!Match(image,typed,800));
    printf("PASS: moved indexed tables, unchanged local branch targets, altered entries/base rejected\n");
}
int main(int argc,char** argv) {
    Synthetic();if(argc<2)return 0;
    auto image=MapPe(argv[1]);Image parsed;assert(ParseImage(image.data(),image.size(),parsed));
    const auto snapshot=image;
    auto begin=std::chrono::steady_clock::now();
    auto result=Discover(image.data(),image.size(),&TestProgress,argc>2 && strcmp(argv[2],"--scan")==0);
    assert(image==snapshot); // read-only discovery must not alter any mapped byte
    if(argc>2 && strcmp(argv[2],"--diagnose")==0) {
        printf("OFFLINE ONLY: timestamp 0x%X, image 0x%zX, reviewed layout %s\n",parsed.timestamp,parsed.size,result.reviewedBuild?"yes":"NO");
        for(const auto& s:result.symbols) if(s.status!=Status::Found)
            printf("TARGET key 0x%X -> RVA 0x%X: %s: %s\n",s.key,s.rva,s.name.c_str(),s.reason.c_str());
        for(uint32_t group:{1u,2u,4u,8u,16u,32u,64u,128u,256u,512u})printf("Discovery group %u: %s (not a layout certificate)\n",group,result.GroupReady(group)?"resolved":"UNRESOLVED");
        return result.validImage?0:1;
    }
    assert(result.validImage && result.reviewedBuild);
    for(uint32_t group : {1u,2u,16u,32u,64u,128u,256u,512u})assert(result.GroupReady(group));
    for(uint32_t key : {0x366010u,0x366100u,0x20da70u,0x365b80u,0x316770u})
        assert(result.Address(key)==key);
    if(argc<=2)assert(result.GroupReady(4) && result.GroupReady(8));
    assert(result.Address(0xb88818)==0xb88818);
    assert(result.Address(0xb70ea0)==0xb70ea0);
    nt->FileHeader.TimeDateStamp^=1;
    // An overwritten native slot must not be accepted as the same RTTI class.
    uint64_t original=0;memcpy(&original,image.data()+0xb88818+8,8);
    uint64_t external=1;memcpy(image.data()+0xb88818+8,&external,8);
    auto conflict=Discover(image.data(),image.size());
    assert(!conflict.GroupReady(1) && !conflict.Address(0xb88818));
    memcpy(image.data()+0xb88818+8,&original,8);
    // Matching masked bytes is insufficient: a changed call destination is invalid.
    int32_t oldDisplacement=0;memcpy(&oldDisplacement,image.data()+0x74a539,4);
    int32_t wrongDisplacement=0x244540-(0x74a538+5);memcpy(image.data()+0x74a539,&wrongDisplacement,4);
    auto wrongCall=Discover(image.data(),image.size());
    assert(!wrongCall.GroupReady(32) && !wrongCall.Address(0x74a538));
    for(const auto& s:wrongCall.symbols)if(s.key==0x74a538)
        assert(s.reason.find("expected resolved")!=std::string::npos);
    memcpy(image.data()+0x74a539,&oldDisplacement,4);
    // Additional monitor slots are certified through RTTI + full function bodies;
    // a patched release slot must never fall back to the reference address.
    const uint32_t releaseSlot=0xb70ea0+90*8;
    memcpy(&original,image.data()+releaseSlot,8);
    memcpy(image.data()+releaseSlot,&external,8);
    auto badRelease=Discover(image.data(),image.size());
    assert(!badRelease.GroupReady(64) && !badRelease.Address(0xb70ea0));
    memcpy(image.data()+releaseSlot,&original,8);
    // The request CALL's operand is masked, but its full callee still has to
    // match. Redirecting it to an unrelated executable function is rejected.
    const uint32_t targetCallOperand=0x366010+175;
    memcpy(&oldDisplacement,image.data()+targetCallOperand,4);
    wrongDisplacement=0x244540-(targetCallOperand+4);
    memcpy(image.data()+targetCallOperand,&wrongDisplacement,4);
    auto badRequest=Discover(image.data(),image.size());
    assert(!badRequest.GroupReady(64) && !badRequest.Address(0x366010));
    memcpy(image.data()+targetCallOperand,&oldDisplacement,4);
    // Field offsets are NOT wildcarded address operands. A changed logical
    // dimension field is a different layout, even on otherwise identical code.
    const uint32_t widthField=0x366010+134;
    const uint8_t savedField=image[widthField];image[widthField]^=1;
    auto badField=Discover(image.data(),image.size());
    assert(!badField.GroupReady(64) && !badField.Address(0x366010));
    image[widthField]=savedField;
    assert(image==snapshot);
    const auto& importDir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data()+importDir.VirtualAddress);
    for(size_t i=0;i<importDir.Size/sizeof(*imports) && imports[i].Name;++i) {
        if(_stricmp(reinterpret_cast<const char*>(image.data()+imports[i].Name),"OPENGL32.dll"))continue;
        auto saved=imports[i].OriginalFirstThunk;imports[i].OriginalFirstThunk=0;
        assert(!FindImport(parsed,"OPENGL32.dll","glDrawElements"));imports[i].OriginalFirstThunk=saved;
    }
    assert(!ParseImage(image.data(),image.size()-1,parsed));
    assert(ParseImage(image.data(),image.size(),parsed));
    auto sections=IMAGE_FIRST_SECTION(nt);
    auto originalSize=sections[0].Misc.VirtualSize;sections[0].Misc.VirtualSize=UINT32_MAX;
    assert(!ParseImage(image.data(),image.size(),parsed));sections[0].Misc.VirtualSize=originalSize;
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
    printf("PASS: reference ASLR, RTTI/vtable slots, import-by-name, unknown-build read-only discovery, legacy-hook rejection (%lld ms)\n",static_cast<long long>(elapsed));
}
