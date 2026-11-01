// Read-only discovery: mapped executable bytes, never execute native functions.
#include "../src/engine_discovery.h"
#include "../src/custom_display_layout.h"
#include "../src/custom_display_dialog_layout.h"
#include "../src/custom_display_dialog_native.h"
#include "../src/custom_display_dialog_callbacks.h"
#include "../src/custom_display_dialog_input.h"
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
    swml::CustomDisplayLayout displayLayout;
    assert(swml::DiscoverCustomDisplayLayout(parsed,result,displayLayout));
    assert(displayLayout.definition==0x58 && displayLayout.width==0x4b8 &&
        displayLayout.height==0x4bc && displayLayout.scale==0x4c0 &&
        displayLayout.video==0x4c8 && displayLayout.overlay==0x4d8 &&
        displayLayout.definitionId==0x288 && displayLayout.archiveId==0x328 &&
        displayLayout.createSlot==89 && displayLayout.releaseSlot==90);
    assert(displayLayout.minX==0xc0 && displayLayout.maxX==0xcc &&
           displayLayout.minZ==0xc8 && displayLayout.maxZ==0xd4);
    auto rejectedLayout=displayLayout;
    auto unreviewed=result;unreviewed.reviewedBuild=false;
    assert(!swml::DiscoverCustomDisplayLayout(parsed,unreviewed,rejectedLayout));
    // Stale successful discovery must not authorize changed layout code/slots.
    for(uint32_t at:{0x3660a0u,0x3167e2u,0xb71168u}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayLayout(parsed,result,rejectedLayout));
        assert(rejectedLayout.width==displayLayout.width);
        image[at]^=1;
    }
    puts("PASS: custom display operand-derived layout; unknown/stale code/identity/slot evidence rejected");
    swml::CustomDisplayDialogWidgetLayout dialog;
    assert(swml::DiscoverCustomDisplayDialogWidgetLayout(parsed,result,dialog));
    assert(dialog.parent==0x20 && dialog.editboxContent==0x78 && dialog.initialText==0x58 && dialog.liveText==0xb8 &&
        dialog.allowedText==0x108 && dialog.maxCharacters==0xcc && dialog.contentAllocation==0x120 &&
        dialog.dialogOpen==0x81 && dialog.contextModal==0x1a0 && dialog.rootContext==0x50 &&
        dialog.rootModal==0x1f0 && dialog.openSlot==37);
    auto rejectedDialog=dialog;
    assert(!swml::DiscoverCustomDisplayDialogWidgetLayout(parsed,unreviewed,rejectedDialog));
    // Includes stale code, a masked CALL relationship, a widget slot and RTTI.
    for(uint32_t at:{0x24f690u+0x124,0x24f8a0u+0x21,0xb0b5c8u+3*8,0xafb968u-8}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayDialogWidgetLayout(parsed,result,rejectedDialog));
        assert(rejectedDialog.liveText==dialog.liveText && rejectedDialog.rootModal==dialog.rootModal);
        image[at]^=1;
    }
    auto fabricated=result;
    for(auto& symbol:fabricated.symbols) if(symbol.key==0x24f8a0) ++symbol.rva;
    assert(!swml::DiscoverCustomDisplayDialogWidgetLayout(parsed,fabricated,rejectedDialog));
    puts("PASS: native dialog widget discovery, live-text/modal operand cross-checks, stale code/call/RTTI/slot rejection (read-only, no UI installed)");
    swml::CustomDisplayVehicleObserverLayout vehicle;
    assert(swml::DiscoverCustomDisplayVehicleObserverLayout(parsed,result,vehicle));
    assert(vehicle.stateRoot==0x3f8 && vehicle.widgets.parent==0x20 &&
        vehicle.uiInitSlot==1 && vehicle.uiUpdateSlot==3 && vehicle.uiTeardownSlot==2 &&
        vehicle.uiDestroySlot==0 && vehicle.stateTeardownSlot==2 && vehicle.stateActionsSlot==20);
    auto rejectedVehicle=vehicle;
    for(uint32_t at:{0x7cf750u+0x18,0x62c530u+0xa,0xb99940u+3*8,0xb44838u+20*8}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayVehicleObserverLayout(parsed,result,rejectedVehicle));
        assert(rejectedVehicle.stateRoot==vehicle.stateRoot);
        image[at]^=1;
    }
    puts("PASS: vehicle-only observer contracts, root operand and six slots; stale-body/call/table rejection (no hooks installed)");
    swml::CustomDisplayVehicleActivityLayout activity;
    assert(swml::DiscoverCustomDisplayVehicleActivityLayout(parsed,result,activity));
    assert(activity.stateEnterSlot==3 && activity.stateLeaveSlot==4 && activity.lifetime.stateRoot==0x3f8);
    auto rejectedActivity=activity;
    for(uint32_t at:{0x848d20u+0xb5,0x848d20u+0x73,0x7cfce0u+0x20,
                    0x7d0770u+1,0xb44838u+3*8,0xb44838u-8}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayVehicleActivityLayout(parsed,result,rejectedActivity));
        assert(rejectedActivity.stateEnterSlot==activity.stateEnterSlot);
        image[at]^=1;
    }
    auto fabricatedActivity=result;
    for(auto& symbol:fabricatedActivity.symbols) if(symbol.key==0x848d20) ++symbol.rva;
    assert(!swml::DiscoverCustomDisplayVehicleActivityLayout(parsed,fabricatedActivity,rejectedActivity));
    assert(!swml::DiscoverCustomDisplayVehicleActivityLayout(parsed,unreviewed,rejectedActivity));
    puts("PASS: retained vehicle enter/leave contracts, switch-derived slots; stale/fabricated/unreviewed evidence rejected");
    swml::CustomDisplayDialogConstructionLayout construction;
    assert(swml::DiscoverCustomDisplayDialogConstructionLayout(parsed,result,construction));
    assert(construction.dialogBytes==0xe8 && construction.editboxBytes==0x80);
    swml::CustomDisplayDialogNativeApi nativeDialog;
    assert(swml::BindCustomDisplayDialogNativeApi(parsed,result,nativeDialog) && nativeDialog.Ready());
    assert(nativeDialog.dialogBytes==construction.dialogBytes && nativeDialog.editboxBytes==construction.editboxBytes);
    assert(reinterpret_cast<const void*>(nativeDialog.makeDialog)==parsed.base+result.Address(0x25a020));
    auto rejectedConstruction=construction;
    for(uint32_t at:{0x62ac80u+0x88c,0x627370u+0x2fc,0x6276e8u+1,
                     0x25a020u+1,0x24f560u+1,0xb6e2f0u+8,0xb0b5c8u-8}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayDialogConstructionLayout(parsed,result,rejectedConstruction));
        assert(!swml::BindCustomDisplayDialogNativeApi(parsed,result,nativeDialog));
        assert(rejectedConstruction.dialogBytes==construction.dialogBytes);
        image[at]^=1;
    }
    auto forgedConstruction=result;
    for(auto& symbol:forgedConstruction.symbols) if(symbol.key==0x24f560) ++symbol.rva;
    assert(!swml::BindCustomDisplayDialogNativeApi(parsed,forgedConstruction,nativeDialog));
    assert(!swml::BindCustomDisplayDialogNativeApi(parsed,unreviewed,nativeDialog));
    puts("PASS: native dialog constructor bindings and caller-derived allocations; stale/forged/unreviewed evidence rejected (no execution)");
    swml::CustomDisplayCatalogueLayout catalogue;
    assert(swml::DiscoverCustomDisplayCatalogueLayout(parsed,result,catalogue));
    assert(catalogue.stateUi==0x8a8 && catalogue.uiCatalogue==0x150 && catalogue.catalogueView==0x78 &&
        catalogue.viewList==0xb0 && catalogue.listSelectedItem==0x90 && catalogue.itemEntry==0x58 &&
        catalogue.entryDefinition==8);
    auto rejectedCatalogue=catalogue;
    for(uint32_t at:{0x7fa7a0u+0x4770,0x7fa7a0u+0x4777,0x7fa7a0u+0x4792,
                     0x611050u+3,0x611050u+7,0x611050u+0xe,0x611050u+0x1a}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayCatalogueLayout(parsed,result,rejectedCatalogue));
        assert(rejectedCatalogue.itemEntry==catalogue.itemEntry);
        image[at]^=1;
    }
    assert(!swml::DiscoverCustomDisplayCatalogueLayout(parsed,unreviewed,rejectedCatalogue));
    auto forgedCatalogue=result;
    for(auto& symbol:forgedCatalogue.symbols) if(symbol.key==0x611050) ++symbol.rva;
    assert(!swml::DiscoverCustomDisplayCatalogueLayout(parsed,forgedCatalogue,rejectedCatalogue));
    puts("PASS: catalogue-specific pointer-chain operands, stale selection getter/case/identity rejection (no hook)");
    swml::CustomDisplayDialogCallbackLayout callbacks;
    assert(swml::DiscoverCustomDisplayDialogCallbackLayout(parsed,result,callbacks));
    assert(callbacks.confirmButton==0xd0 && callbacks.cancelButton==0xd8 && callbacks.buttonBytes==0x100 &&
        callbacks.callback==0xd8 && callbacks.userData==0xe0 && callbacks.releaseAction==0x90 && callbacks.pressActions==0x94);
    swml::CustomDisplayDialogCallbackAccess callbackAccess;
    auto copy=[](void*,const void*,size_t,void*,size_t)->bool { return false; };
    assert(swml::BindCustomDisplayDialogCallbackAccess(parsed,result,copy,nullptr,callbackAccess));
    assert(callbackAccess.buttonTable==parsed.base+result.Address(0xbace08));
    auto rejectedCallbacks=callbacks;
    for(uint32_t at:{0x25a190u+0x6b2,0x25a190u+0x4d8,0x249200u+0x17c,
                     0x249900u+0x14d,0xbace08u+18*8,0xbace08u-8,0x1df720u+1}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayDialogCallbackLayout(parsed,result,rejectedCallbacks));
        assert(!swml::BindCustomDisplayDialogCallbackAccess(parsed,result,copy,nullptr,callbackAccess));
        assert(rejectedCallbacks.callback==callbacks.callback);
        image[at]^=1;
    }
    auto forgedCallbacks=result;
    for(auto& symbol:forgedCallbacks.symbols) if(symbol.key==0x249900) ++symbol.rva;
    assert(!swml::DiscoverCustomDisplayDialogCallbackLayout(parsed,forgedCallbacks,rejectedCallbacks));
    assert(!swml::BindCustomDisplayDialogCallbackAccess(parsed,unreviewed,copy,nullptr,callbackAccess));
    puts("PASS: native button callback/action operands and RTTI, submit-handler evidence, tampered/stale/forged rejection (no execution)");
    swml::CustomDisplayDialogInputLayout inputLayout;
    swml::CustomDisplayDialogInputApi inputApi;
    assert(swml::DiscoverCustomDisplayDialogInputLayout(parsed,result,inputLayout));
    assert(inputLayout.stateRoot==0x3f8 && inputLayout.stateContext==0x448 &&
        inputLayout.stateQueue==0x888 && inputLayout.contextFocus==0x190 &&
        inputLayout.escapeKind==0x140 && inputLayout.escapeCode==0x148 &&
        inputLayout.submitKey==0x3d && inputLayout.keypadSubmitKey==0x5e &&
        inputLayout.rootHover==0x1d8 && inputLayout.rootPointerHeld==0x488);
    assert(swml::BindCustomDisplayDialogInputApi(parsed,result,inputApi));
    assert(reinterpret_cast<const void*>(inputApi.character)==parsed.base+result.Address(0x24e5c0));
    assert(reinterpret_cast<const void*>(inputApi.pointer)==parsed.base+result.Address(0x1e3180));
    auto rejectedInput=inputLayout;
    const auto retainedInputApi=inputApi;
    for(uint32_t at:{0x7f09d0u+0x74,0x7f0af0u+0x1ba,0x25a9e0u+0x31,
        0x25a9e0u+0x3d,0x1df720u+0x29a,0x1df720u+0x29f,0xb05f40u+20*8,
        0xb44838u+12*8,0x1e3180u+0xbb,0x1e3180u+0xf7,0x1e3180u+0x244,0x21a200u+1}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayDialogInputLayout(parsed,result,rejectedInput));
        assert(!swml::BindCustomDisplayDialogInputApi(parsed,result,inputApi));
        assert(rejectedInput.submitKey==inputLayout.submitKey && inputApi.pointer==retainedInputApi.pointer);
        image[at]^=1;
    }
    for(uint32_t key:{0x7f09d0u,0x24e5c0u,0x24e2e0u,0x1e2d40u,0x1e3180u,0x21a200u,0xb0b5c8u}) {
        auto forgedInput=result;
        for(auto& symbol:forgedInput.symbols) if(symbol.key==key) ++symbol.rva;
        assert(!swml::BindCustomDisplayDialogInputApi(parsed,forgedInput,inputApi));
    }
    assert(!swml::BindCustomDisplayDialogInputApi(parsed,unreviewed,inputApi));
    puts("PASS: operand-derived native input/submit/Escape layouts, helper bindings and tampered/stale/forged rejection (no execution)");
    swml::CustomDisplayNativeInputStateLayout nativeInput;
    swml::CustomDisplayNativeInputStateApi nativeInputApi;
    assert(result.GroupReady(swml::CustomDisplayNativeInputStateGroup));
    assert(swml::DiscoverCustomDisplayNativeInputStateLayout(parsed,result,nativeInput));
    assert(nativeInput.hostWindow==0xd38 && nativeInput.keyStates==0x80 && nativeInput.keyCount==349 &&
        nativeInput.mouseStates==0x78 && nativeInput.mouseCount==8 && nativeInput.rootRecipient==0x1e0 &&
        nativeInput.rootHover==0x1d8 && nativeInput.rootContext==0x50 && nativeInput.rootPointerHeld==0x488);
    assert(swml::BindCustomDisplayNativeInputStateApi(parsed,result,nativeInputApi));
    assert(reinterpret_cast<const void*>(nativeInputApi.retireRecipients)==parsed.base+result.Address(0x1e2e20));
    auto rejectedNativeInput=nativeInput;
    const auto retainedNativeInputApi=nativeInputApi;
    for(uint32_t at:{0x1bf810u+0x14,0x97090u+0x40,0x97090u+0x51,0x97090u+0x89,
        0x97090u+0x97,0x97090u+0xa7,0x97090u+0xd6,0x1e2e20u+0x10,0x1e2e20u+0x3b,
        0x1e1bc0u+0x4c,0x1bf810u+0x84}) {
        image[at]^=1;
        assert(!swml::DiscoverCustomDisplayNativeInputStateLayout(parsed,result,rejectedNativeInput));
        assert(!swml::BindCustomDisplayNativeInputStateApi(parsed,result,nativeInputApi));
        assert(rejectedNativeInput.keyCount==nativeInput.keyCount && nativeInputApi.retireRecipients==retainedNativeInputApi.retireRecipients);
        image[at]^=1;
    }
    for(uint32_t key:{0x1bf810u,0x97090u,0x1e2e20u,0x1e1bc0u}) {
        auto forgedNativeInput=result;
        for(auto& symbol:forgedNativeInput.symbols) if(symbol.key==key) ++symbol.rva;
        assert(!swml::BindCustomDisplayNativeInputStateApi(parsed,forgedNativeInput,nativeInputApi));
    }
    assert(!swml::BindCustomDisplayNativeInputStateApi(parsed,unreviewed,nativeInputApi));
    puts("PASS: native-window exhaustive keyboard/mouse bounds, host and recipient-retirement operands, tampered/stale/forged rejection (read-only, no native calls)");
    assert(result.drawElementsIat==0xa925f0);
    auto loaded=DiscoverLoadedEngine(image.data());
    assert(loaded.validImage && loaded.GroupReady(1));
    // Unknown metadata triggers diagnostic scanning but does NOT certify layouts.
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data()+dos->e_lfanew);
    nt->FileHeader.TimeDateStamp^=1;
    auto unknown=Discover(image.data(),image.size(),&TestProgress);
    assert(unknown.validImage && !unknown.reviewedBuild);
    assert(unknown.Address(0xb88818)==0xb88818);
    assert(unknown.GroupReady(1) && unknown.GroupReady(2) && unknown.GroupReady(32));
    assert(unknown.GroupReady(64) && unknown.GroupReady(128));
    assert(unknown.GroupReady(256));
    assert(!swml::DiscoverCustomDisplayDialogWidgetLayout(parsed,unknown,rejectedDialog));
    assert(unknown.GroupReady(512));
    assert(!swml::DiscoverCustomDisplayVehicleObserverLayout(parsed,unknown,rejectedVehicle));
    assert(unknown.GroupReady(1024));
    assert(!swml::DiscoverCustomDisplayVehicleActivityLayout(parsed,unknown,rejectedActivity));
    assert(unknown.GroupReady(2048));
    assert(!swml::BindCustomDisplayDialogNativeApi(parsed,unknown,nativeDialog));
    assert(unknown.GroupReady(4096));
    assert(!swml::DiscoverCustomDisplayCatalogueLayout(parsed,unknown,rejectedCatalogue));
    assert(unknown.GroupReady(8192));
    assert(!swml::DiscoverCustomDisplayDialogCallbackLayout(parsed,unknown,rejectedCallbacks));
    // New input contracts need not be uniquely recoverable on unknown builds;
    // neither diagnostic matches nor a group bit would certify activation.
    assert(!swml::DiscoverCustomDisplayDialogInputLayout(parsed,unknown,rejectedInput));
    assert(!swml::BindCustomDisplayNativeInputStateApi(parsed,unknown,nativeInputApi));
    assert(!unknown.GroupReady(4) && !unknown.GroupReady(8)); // unsolved duplicate bodies never guessed
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
