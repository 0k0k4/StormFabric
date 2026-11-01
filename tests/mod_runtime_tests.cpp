#include "../src/mod_runtime.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <limits>
namespace swml { void LogError(const char*,...){} }
namespace {
SwModHost owner={},other={};
int events=0,tasks=0,mutations=0;SwHandle subscription=0,cancelled=0;
int luaEvents=0;
void LuaEvent(const SwEvent* e,void*){assert(e->surface==SW_SURFACE_NONE&&e->handle==42);++luaEvents;}
void Task(void*){++tasks;}
void Event(const SwEvent* e,void*){assert(e->type==SW_EVENT_UI_UPDATE);assert(e->structSize==sizeof(SwEvent));++events;}
void Mutate(const SwEvent*,void*){++mutations;assert(swml::RuntimePrimitives()->unsubscribe(&owner,subscription)==SW_OK);}
void CancelOther(void*){++tasks;assert(swml::RuntimePrimitives()->cancelTask(&owner,cancelled)==SW_OK);}
void Release(void*){swml::ReleaseRuntimeOwner(&owner);}
DWORD WINAPI Worker(void*){assert(!swml::RuntimePrimitives()->isGameThread());swml::PumpModRuntime(SW_SURFACE_HUD,false,true);return 0;}
}
int main(){
    auto* api=swml::RuntimePrimitives();auto* input=swml::InputPrimitives();SwHandle id=0;
    assert(api->runOnGameThread(&owner,&Task,nullptr,&id)==SW_INVALID_OWNER);
    swml::RegisterRuntimeOwner(&owner);swml::RegisterRuntimeOwner(&other);
    assert(api->subscribe(&owner,SW_EVENT_MASK(SW_EVENT_SIMULATION_TICK),&Event,nullptr,&id)==SW_UNAVAILABLE);
    assert(api->subscribe(&owner,SW_EVENT_MASK(SW_EVENT_UI_UPDATE),&Event,nullptr,&id)==SW_OK);
    assert(api->unsubscribe(&other,id)==SW_NOT_FOUND);
    assert(api->schedule(&owner,std::numeric_limits<double>::quiet_NaN(),0,&Task,nullptr,&subscription)==SW_INVALID_ARGUMENT);
    assert(api->runOnGameThread(&owner,&Task,nullptr,&subscription)==SW_OK);assert(tasks==0);
    swml::PumpModRuntime(SW_SURFACE_MENU,false,true);assert(tasks==1&&events==1&&api->isGameThread());
    assert(api->cancelTask(&owner,subscription)==SW_NOT_FOUND);
    SwHandle luaSub=0;assert(api->subscribe(&owner,SW_EVENT_MASK(SW_EVENT_LUA_CREATED),&LuaEvent,nullptr,&luaSub)==SW_OK);
    swml::QueueModLuaEvent(SW_EVENT_LUA_CREATED,42);assert(luaEvents==0);
    HANDLE worker=CreateThread(nullptr,0,&Worker,nullptr,0,nullptr);assert(worker);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);assert(events==1);
    assert(api->subscribe(&owner,SW_EVENT_MASK(SW_EVENT_UI_UPDATE),&Mutate,nullptr,&subscription)==SW_OK);
    swml::PumpModRuntime(SW_SURFACE_HUD,false,true);assert(mutations==1);
    assert(luaEvents==1);
    swml::PumpModRuntime(SW_SURFACE_HUD,false,true);assert(mutations==1);
    assert(api->runOnGameThread(&owner,&CancelOther,nullptr,&subscription)==SW_OK);
    assert(api->runOnGameThread(&owner,&Task,nullptr,&cancelled)==SW_OK);
    swml::PumpModRuntime(SW_SURFACE_HUD,false,true);assert(tasks==2);
    SwHandle action=0,action2=0;assert(input->registerAction(&owner,"toggle",&action)==SW_OK);
    assert(input->registerAction(&owner,"toggle",&action2)==SW_CONFLICT);
    assert(input->bindKey(&owner,action,VK_F8,0)==SW_OK);
    assert(input->registerAction(&other,"toggle",&action2)==SW_OK);
    assert(input->bindKey(&other,action2,VK_F8,0)==SW_CONFLICT);
    SwActionState state={};swml::PumpModRuntime(SW_SURFACE_EDITOR,false,true);
    assert(input->getActionState(&owner,action,&state)==SW_OK&&!state.down);
    double sim=42;assert(api->getSimulationTime(&sim)==SW_UNAVAILABLE&&sim==42);
    assert(api->runOnGameThread(&owner,&Release,nullptr,&subscription)==SW_OK);
    assert(api->runOnGameThread(&owner,&Task,nullptr,&cancelled)==SW_OK);
    int before=tasks;swml::PumpModRuntime(SW_SURFACE_MENU,false,true);assert(tasks==before);
    assert(input->getActionState(&owner,action,&state)==SW_INVALID_OWNER);
    swml::RegisterRuntimeOwner(&owner);assert(api->cancelTask(&owner,cancelled)==SW_NOT_FOUND);
    // All queued work is owner-scoped and bounded.
    for(int n=0;n<256;++n)assert(api->runOnGameThread(&owner,&Task,nullptr,&id)==SW_OK);
    assert(api->runOnGameThread(&owner,&Task,nullptr,&id)==SW_LIMIT_REACHED);
    swml::ReleaseRuntimeOwner(&owner);swml::ReleaseRuntimeOwner(&other);
    puts("mod_runtime_tests: OK");
}
