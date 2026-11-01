#include "../src/mod_native_ui.h"
#include "../src/mod_runtime.h"
#include "../src/log.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>
namespace {
struct Fake {SwUiClickFn click=nullptr;void* data=nullptr;};
std::vector<std::unique_ptr<Fake>> objects;
int removes=0,textWrites=0,clicks=0;
std::vector<float> fontSizes;
SwUiLayout lastLabel={};
SwModHost owner={},other={};SwHandle clicked=0;
void* Make(){objects.push_back(std::make_unique<Fake>());return objects.back().get();}
void* Composite(void*,void*,const SwUiLayout*){return Make();}
void* Text(void*,void*,const SwUiLayout* layout,const char*,int align,uint32_t,float font,float spacing){
    // Regression: layout heights (.028/.03) must never be passed as native font scalars.
    assert(font>=.0004f&&font<=.0008f&&spacing==.8f);
    if(align==4){assert(layout->width>0&&layout->height>0);lastLabel=*layout;}
    fontSizes.push_back(font);return Make();
}
void* Rectangle(void*,void*,const SwUiLayout*,uint32_t){return Make();}
void* Button(const SwModHost*,void*,void*,const SwUiLayout*,uint32_t,uint32_t,uint32_t,bool,SwUiClickFn fn,void* data){auto* p=static_cast<Fake*>(Make());p->click=fn;p->data=data;return p;}
void Remove(void*,void*,void*){++removes;}
void SetText(void*,void*,const char*){++textWrites;}
void Visible(void*,bool){}
void Click(const SwEvent* event,void*){assert(event->type==SW_EVENT_UI_CLICK);clicked=event->handle;++clicks;}
SwEngineApi engine={};
}
namespace swml {
const SwEngineApi* GetEngineApi(){return &engine;}
void LogError(const char*,...){}
void Notify(LogLevel,const std::string&,const std::string&,float){}
}
int main(){
    SwUiApi raw={};raw.composite=&Composite;raw.text=&Text;raw.rectangle=&Rectangle;raw.rectangleButton=&Button;raw.removeChild=&Remove;raw.setText=&SetText;raw.setVisible=&Visible;engine.ui=&raw;
    auto* api=swml::NativeUiPrimitives();SwHandle root=0,text=0,button=0;SwUiLayout l={0,0,1,.1f,4};
    assert(api->createHudRoot(&owner,SW_SURFACE_HUD,&root)==SW_INVALID_OWNER);
    swml::RegisterRuntimeOwner(&owner);swml::RegisterRuntimeOwner(&other);
    assert(api->createHudRoot(&owner,SW_SURFACE_HUD,&root)==SW_WRONG_THREAD);
    swml::PumpModRuntime(SW_SURFACE_MENU,false,true);
    assert(api->createHudRoot(&owner,SW_SURFACE_HUD,&root)==SW_UNAVAILABLE);
    int nativeParent=1,context=2;swml::InitializeModUi(SW_SURFACE_HUD,&nativeParent,&context);
    assert(api->createHudRoot(&owner,SW_SURFACE_HUD,&root)==SW_OK);
    SwHandle window=0;assert(api->createWindow(&owner,root,&l,"window",&window)==SW_OK);
    assert(fontSizes.size()==1&&fontSizes.back()==.00050f);
    assert(api->text(&owner,root,&l,"test",0xffffffff,&text)==SW_OK);
    assert(fontSizes.back()==.00044f);
    assert(api->setText(&other,text,"bad")==SW_NOT_FOUND);
    assert(api->setText(&owner,root,"bad")==SW_INVALID_ARGUMENT);
    assert(api->setText(&owner,text,"ok")==SW_OK&&textWrites==1);
    assert(api->button(&owner,root,&l,"button",&Click,nullptr,&button)==SW_OK);
    assert(fontSizes.back()==.00048f);
    assert(lastLabel.x==0&&lastLabel.y==0&&lastLabel.anchor==4&&lastLabel.width==l.width&&lastLabel.height==l.height);
    SwUiLayout badButton=l;badButton.width=-1;
    assert(api->button(&owner,root,&badButton,"bad",&Click,nullptr,&button)==SW_INVALID_ARGUMENT);
    badButton=l;badButton.height=0;
    assert(api->button(&owner,root,&badButton,"bad",&Click,nullptr,&button)==SW_INVALID_ARGUMENT);
    Fake* clickObject=nullptr;for(const auto& obj:objects)if(obj->click)clickObject=obj.get();assert(clickObject);
    clickObject->click(nullptr,clickObject->data);assert(clicks==1&&clicked==button);
    assert(api->destroy(&owner,root)==SW_OK&&removes==1);
    clickObject->click(nullptr,clickObject->data);assert(clicks==1);
    assert(api->setText(&owner,text,"stale")==SW_NOT_FOUND);
    assert(api->createHudRoot(&owner,SW_SURFACE_HUD,&root)==SW_OK);
    swml::ForgetModUi(SW_SURFACE_HUD,&nativeParent);assert(removes==1);
    assert(api->destroy(&owner,root)==SW_NOT_FOUND);
    swml::InitializeModUi(SW_SURFACE_HUD,&nativeParent,&context);
    assert(api->createHudRoot(&owner,SW_SURFACE_HUD,&root)==SW_OK);
    swml::ReleaseModUiOwner(&owner);swml::ReleaseRuntimeOwner(&owner);assert(removes==2);
    swml::RegisterRuntimeOwner(&owner);assert(api->destroy(&owner,root)==SW_NOT_FOUND);
    assert(api->createHudRoot(&owner,SW_SURFACE_MENU,&root)==SW_UNAVAILABLE);
    swml::ForgetModUi(SW_SURFACE_HUD,&nativeParent);swml::ReleaseRuntimeOwner(&owner);swml::ReleaseRuntimeOwner(&other);
    puts("mod_native_ui_tests: OK");
}
