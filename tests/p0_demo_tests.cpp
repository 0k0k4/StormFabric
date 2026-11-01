// Exercise the real demo against production runtime/storage/UI wrappers with fake
// native widgets. No game execution; this does NOT prove native rendering.
#include "../examples/p0_demo/p0_demo.cpp"
#include "../src/mod_runtime.h"
#include "../src/mod_native_ui.h"
#include "../src/mod_storage.h"
#include "../src/log.h"
#include <windows.h>
#include <cassert>
#include <memory>
#include <vector>
#include "test_temp.h"
namespace {
struct DemoWidget { SwUiClickFn callback=nullptr;void* data=nullptr; };
std::vector<std::unique_ptr<DemoWidget>> demoWidgets;
SwEngineApi demoEngine={};
void* DemoMake(){demoWidgets.push_back(std::make_unique<DemoWidget>());return demoWidgets.back().get();}
void* DemoComposite(void*,void*,const SwUiLayout*){return DemoMake();}
void* DemoText(void*,void*,const SwUiLayout*,const char*,int,uint32_t,float,float){return DemoMake();}
void* DemoRect(void*,void*,const SwUiLayout*,uint32_t){return DemoMake();}
void* DemoButton(const SwModHost*,void*,void*,const SwUiLayout*,uint32_t,uint32_t,uint32_t,bool,SwUiClickFn fn,void* data){auto* w=static_cast<DemoWidget*>(DemoMake());w->callback=fn;w->data=data;return w;}
void DemoRemove(void*,void*,void*){}
void DemoSetText(void*,void*,const char*){}
void DemoVisible(void*,bool){}
void DemoLog(const char*,const char* text){puts(text);}
}
namespace swml {
const SwEngineApi* GetEngineApi(){return &demoEngine;}
bool IsEngineApiOwner(const SwModHost* owner){return RuntimeOwnerValid(owner);}
void LogError(const char*,...){}
void Notify(LogLevel,const std::string&,const std::string&,float){}
}
int main(){
    SwUiApi raw={};raw.composite=&DemoComposite;raw.text=&DemoText;raw.rectangle=&DemoRect;
    raw.rectangleButton=&DemoButton;raw.removeChild=&DemoRemove;raw.setText=&DemoSetText;raw.setVisible=&DemoVisible;
    demoEngine.abiVersion=SW_ENGINE_API_VERSION;demoEngine.structSize=sizeof(demoEngine);demoEngine.ui=&raw;
    demoEngine.runtime=swml::RuntimePrimitives();demoEngine.input=swml::InputPrimitives();demoEngine.nativeUi=swml::NativeUiPrimitives();demoEngine.storage=swml::GetStorageApi();
    const std::string directory = TestTempRoot() + "\\p0-demo-tests-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    assert(std::filesystem::create_directory(std::filesystem::u8path(directory)));
    SwModHost testHost={};testHost.sdkVersion=SW_MOD_SDK_VERSION;testHost.modId="p0_demo";testHost.modDir=directory.c_str();testHost.log=&DemoLog;testHost.api=&demoEngine;
    swml::RegisterRuntimeOwner(&testHost);assert(swml::RegisterModStorageOwner(&testHost)==SW_STORAGE_OK);
    assert(SwModInit(&testHost));assert(failed.load()==0);
    int parent=1,context=2;swml::InitializeModUi(SW_SURFACE_HUD,&parent,&context);
    swml::PumpModRuntime(SW_SURFACE_HUD,false,true);
    assert(failed.load()==0&&roots[SW_SURFACE_HUD]!=0&&timerCalls==0);
    for(int i=0;i<3;++i){Sleep(270);swml::PumpModRuntime(SW_SURFACE_HUD,false,true);}
    assert(failed.load()==0&&timerCalls==3&&timer==0);
    DemoWidget* button=nullptr;for(const auto& widget:demoWidgets)if(widget->callback)button=widget.get();assert(button);
    button->callback(nullptr,button->data);swml::PumpModRuntime(SW_SURFACE_HUD,false,true);assert(failed.load()==0);
    swml::ForgetModUi(SW_SURFACE_HUD,&parent);assert(roots[SW_SURFACE_HUD]==0);
    swml::ReleaseModUiOwner(&testHost);swml::ReleaseRuntimeOwner(&testHost);swml::ReleaseModStorageOwner(&testHost);SwModUnload();
    button->callback(nullptr,button->data); // Retired native token is inert.
    puts("p0_demo_tests: PASS (production SDK wrappers, fake native widgets; no gameplay proof)");
}
