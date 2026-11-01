#include "mod_runtime.h"
#include "log.h"
#include <windows.h>
#pragma comment(lib,"user32.lib")
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace swml {
namespace {
struct Lease { bool active=true; };
struct Subscription { SwHandle id;std::shared_ptr<Lease> lease;const SwModHost* owner;uint64_t mask;SwEventCallback callback;void* data; };
struct Task { SwHandle id;std::shared_ptr<Lease> lease;SwTaskCallback callback;void* data;double due,repeat;bool active=true; };
struct Action { SwHandle id;std::shared_ptr<Lease> lease;const SwModHost* owner;std::string name;uint32_t key=0,modifiers=0;SwActionState state={}; };
std::recursive_mutex mutex;
std::mutex pendingMutex;
struct Pending { SwEventType type;SwHandle handle; };
std::vector<Pending> pending;
std::map<const SwModHost*,std::shared_ptr<Lease>> owners;
std::map<SwHandle,std::shared_ptr<Subscription>> subscriptions;
std::map<SwHandle,std::shared_ptr<Task>> tasks;
std::map<SwHandle,Action> actions;
SwHandle next=1;
DWORD thread=0;
uint64_t sequence=0,capabilities=SW_CAP_UI_EVENTS|SW_CAP_MAIN_THREAD_TASKS|SW_CAP_KEYBOARD_ACTIONS;
double last=0,delta=0;
bool pumping=false;
constexpr size_t kPerOwnerLimit=256;
double Now() { return double(GetTickCount64())/1000.; }
std::shared_ptr<Lease> Owner(const SwModHost* owner) { auto it=owners.find(owner);return it==owners.end()?nullptr:it->second; }
bool EventCpp(SwEventCallback fn,const SwEvent* event,void* data) {try{fn(event,data);return true;}catch(...){return false;}}
bool SafeEvent(SwEventCallback fn,const SwEvent* event,void* data) {
    __try {return EventCpp(fn,event,data);} __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool TaskCpp(SwTaskCallback fn,void* data) {try{fn(data);return true;}catch(...){return false;}}
bool SafeTask(SwTaskCallback fn,void* data) {
    __try {return TaskCpp(fn,data);} __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uint64_t Caps(){std::lock_guard<std::recursive_mutex> lock(mutex);return capabilities;}
bool IsThread(){std::lock_guard<std::recursive_mutex> lock(mutex);return thread && thread==GetCurrentThreadId();}
SwResult SimTime(double* out){return out?SW_UNAVAILABLE:SW_INVALID_ARGUMENT;}
SwResult Subscribe(const SwModHost* owner,uint64_t mask,SwEventCallback callback,void* data,SwHandle* out){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);
    if(!lease)return SW_INVALID_OWNER;
    const uint64_t allowed=SW_EVENT_MASK(SW_EVENT_UI_UPDATE)|SW_EVENT_MASK(SW_EVENT_UI_READY)|SW_EVENT_MASK(SW_EVENT_UI_CLOSED)|
        SW_EVENT_MASK(SW_EVENT_ACTION)|SW_EVENT_MASK(SW_EVENT_LUA_CREATED)|SW_EVENT_MASK(SW_EVENT_LUA_DESTROYED);
    if(!callback || !out || !mask)return SW_INVALID_ARGUMENT;
    if(mask&~allowed)return SW_UNAVAILABLE;
    size_t count=0;for(const auto& item:subscriptions)if(item.second->lease==lease)++count;
    if(count>=kPerOwnerLimit || !next)return SW_LIMIT_REACHED;
    auto s=std::make_shared<Subscription>(Subscription{next++,lease,owner,mask,callback,data});
    subscriptions[s->id]=s;*out=s->id;return SW_OK;
}
SwResult Unsubscribe(const SwModHost* owner,SwHandle id){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    auto it=subscriptions.find(id);if(it==subscriptions.end() || it->second->lease!=lease)return SW_NOT_FOUND;
    it->second->callback=nullptr;subscriptions.erase(it);return SW_OK;
}
SwResult Schedule(const SwModHost* owner,double delay,double repeat,SwTaskCallback callback,void* data,SwHandle* out){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    if(!out || !callback || !std::isfinite(delay) || !std::isfinite(repeat) || delay<0 || repeat<0 || delay>86400 || repeat>86400)return SW_INVALID_ARGUMENT;
    size_t count=0;for(const auto& item:tasks)if(item.second->lease==lease)++count;
    if(count>=kPerOwnerLimit || !next)return SW_LIMIT_REACHED;
    auto task=std::make_shared<Task>(Task{next++,lease,callback,data,Now()+delay,repeat,true});
    tasks[task->id]=task;*out=task->id;return SW_OK;
}
SwResult Run(const SwModHost* owner,SwTaskCallback callback,void* data,SwHandle* out){return Schedule(owner,0,0,callback,data,out);}
SwResult Cancel(const SwModHost* owner,SwHandle id){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    auto it=tasks.find(id);if(it==tasks.end() || it->second->lease!=lease)return SW_NOT_FOUND;
    it->second->active=false;tasks.erase(it);return SW_OK;
}
SwResult RegisterAction(const SwModHost* owner,const char* name,SwHandle* out){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    if(!name || !out || !*name || strnlen_s(name,65)>64)return SW_INVALID_ARGUMENT;
    size_t count=0;for(const auto& item:actions)if(item.second.lease==lease){++count;if(item.second.name==name)return SW_CONFLICT;}
    if(count>=64 || !next)return SW_LIMIT_REACHED;
    Action action;action.id=next++;action.owner=owner;action.lease=lease;action.name=name;
    actions.emplace(action.id,action);*out=action.id;return SW_OK;
}
SwResult Bind(const SwModHost* owner,SwHandle id,uint32_t key,uint32_t modifiers){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    if(!key || key>255 || modifiers>7)return SW_INVALID_ARGUMENT;
    auto it=actions.find(id);if(it==actions.end() || it->second.lease!=lease)return SW_NOT_FOUND;
    for(const auto& item:actions)if(item.first!=id && item.second.key==key && item.second.modifiers==modifiers)return SW_CONFLICT;
    it->second.key=key;it->second.modifiers=modifiers;it->second.state={};return SW_OK;
}
SwResult ActionState(const SwModHost* owner,SwHandle id,SwActionState* out){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    if(!out)return SW_INVALID_ARGUMENT;auto it=actions.find(id);if(it==actions.end() || it->second.lease!=lease)return SW_NOT_FOUND;
    *out=it->second.state;return SW_OK;
}
SwResult UnregisterAction(const SwModHost* owner,SwHandle id){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return SW_INVALID_OWNER;
    auto it=actions.find(id);if(it==actions.end() || it->second.lease!=lease)return SW_NOT_FOUND;actions.erase(it);return SW_OK;
}
const SwRuntimeApi runtime={sizeof(SwRuntimeApi),&Caps,&Subscribe,&Unsubscribe,&Run,&Schedule,&Cancel,&Now,&IsThread,&SimTime};
const SwInputApi input={sizeof(SwInputApi),&RegisterAction,&Bind,&ActionState,&UnregisterAction};
}
void RegisterRuntimeOwner(const SwModHost* owner){if(!owner)return;std::lock_guard<std::recursive_mutex> lock(mutex);if(!owners.count(owner))owners[owner]=std::make_shared<Lease>();}
void ReleaseRuntimeOwner(const SwModHost* owner){
    std::lock_guard<std::recursive_mutex> lock(mutex);auto lease=Owner(owner);if(!lease)return;
    lease->active=false;owners.erase(owner);
    for(auto it=subscriptions.begin();it!=subscriptions.end();)if(it->second->lease==lease){it->second->callback=nullptr;it=subscriptions.erase(it);}else ++it;
    for(auto it=tasks.begin();it!=tasks.end();)if(it->second->lease==lease){it->second->active=false;it=tasks.erase(it);}else ++it;
    for(auto it=actions.begin();it!=actions.end();)if(it->second.lease==lease)it=actions.erase(it);else ++it;
}
bool RuntimeOwnerValid(const SwModHost* owner){std::lock_guard<std::recursive_mutex> lock(mutex);return bool(Owner(owner));}
void SetRuntimeCapabilities(uint64_t value){std::lock_guard<std::recursive_mutex> lock(mutex);capabilities=value;}
void QueueModLuaEvent(SwEventType type,SwHandle id){
    if(type!=SW_EVENT_LUA_CREATED&&type!=SW_EVENT_LUA_DESTROYED)return;
    std::lock_guard<std::mutex> lock(pendingMutex);
    if(pending.size()<4096)pending.push_back({type,id});
    else LogError("P0: Lua lifecycle event queue full; event dropped");
}
void EmitModEvent(SwEventType type,SwUiSurface surface,SwHandle handle,uint32_t value,const SwModHost* onlyOwner){
    // Holding a recursive mutex across callbacks serializes release with execution,
    // but permits a callback to cancel itself, register work or unsubscribe.
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!thread || thread!=GetCurrentThreadId())return;
    std::vector<std::shared_ptr<Subscription>> selected;
    for(const auto& item:subscriptions)if(item.second->mask&SW_EVENT_MASK(type))selected.push_back(item.second);
    SwEvent event={sizeof(SwEvent),type,handle,surface,value,sequence,Now(),delta};
    for(const auto& s:selected)if(s->lease->active && s->callback && (!onlyOwner || s->owner==onlyOwner))
        if(!SafeEvent(s->callback,&event,s->data)){s->callback=nullptr;LogError("P0: plugin event callback failed; disabled");}
}
void PumpModRuntime(SwUiSurface surface,bool focused,bool captured){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!thread)thread=GetCurrentThreadId();
    if(thread!=GetCurrentThreadId() || pumping)return;
    pumping=true;
    double now=Now();delta=last?now-last:0;last=now;++sequence;
    std::vector<Pending> lifecycle;
    {std::lock_guard<std::mutex> pendingLock(pendingMutex);lifecycle.swap(pending);}
    for(const auto& event:lifecycle)EmitModEvent(event.type,SW_SURFACE_NONE,event.handle);
    uint32_t modifiers=(GetAsyncKeyState(VK_CONTROL)&0x8000?SW_KEY_CTRL:0)|(GetAsyncKeyState(VK_SHIFT)&0x8000?SW_KEY_SHIFT:0)|(GetAsyncKeyState(VK_MENU)&0x8000?SW_KEY_ALT:0);
    struct Edge{SwHandle id;const SwModHost* owner;std::shared_ptr<Lease> lease;uint32_t value;};std::vector<Edge> edges;
    for(auto& item:actions){auto& a=item.second;bool down=focused && !captured && a.key && modifiers==a.modifiers && (GetAsyncKeyState(a.key)&0x8000);
        a.state.pressed=down&&!a.state.down;a.state.released=!down&&a.state.down;a.state.down=down;a.state.sequence=sequence;
        if(a.state.pressed || a.state.released)edges.push_back({a.id,a.owner,a.lease,down?1u:0u});}
    for(const auto& edge:edges)if(edge.lease->active)EmitModEvent(SW_EVENT_ACTION,surface,edge.id,edge.value,edge.owner);
    std::vector<std::shared_ptr<Task>> due;
    for(const auto& item:tasks)if(item.second->due<=now)due.push_back(item.second);
    // Bound one pulse's workload: new tasks run on a later pulse, never recursively.
    size_t executed=0;
    for(const auto& t:due){if(executed>=256)break;if(!t->lease->active || !t->active)continue;++executed;
        if(t->repeat>0)t->due=now+t->repeat;else {tasks.erase(t->id);t->active=false;}
        if(!SafeTask(t->callback,t->data)){t->active=false;tasks.erase(t->id);LogError("P0: plugin task callback failed; cancelled");}}
    EmitModEvent(SW_EVENT_UI_UPDATE,surface);
    pumping=false;
}
const SwRuntimeApi* RuntimePrimitives(){return &runtime;}
const SwInputApi* InputPrimitives(){return &input;}
}
