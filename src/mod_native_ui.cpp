#include "mod_native_ui.h"
#include "mod_runtime.h"
#include "engine_api.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
namespace swml {
namespace {
struct Surface {void* parent=nullptr;void* context=nullptr;bool ready=false;};
Surface surfaces[3];
struct Node {
    SwHandle id=0,parent=0;const SwModHost* owner=nullptr;SwUiSurface surface=SW_SURFACE_MENU;
    void* native=nullptr;bool text=false,retainedToken=false;SwEventCallback click=nullptr;void* data=nullptr;
};
std::map<SwHandle,std::unique_ptr<Node>> nodes;
// The legacy click trampoline retains its token until shutdown. These loader
// tokens remain valid after native tree destruction but are made inert first.
std::vector<std::unique_ptr<Node>> retired;
SwHandle next=1;
// Native make_text font scalars are NOT UI layout heights. Match the existing
// native menu/notification widgets; .028/.03 here made glyphs ~60x too large.
constexpr float kBodyFont=.00044f,kTitleFont=.00050f,kButtonFont=.00048f,kLineSpacing=.8f;
SwResult Check(const SwModHost* owner) {if(!RuntimeOwnerValid(owner))return SW_INVALID_OWNER;return RuntimePrimitives()->isGameThread()?SW_OK:SW_WRONG_THREAD;}
Node* Find(const SwModHost* owner,SwHandle id){auto it=nodes.find(id);return it!=nodes.end()&&it->second->owner==owner?it->second.get():nullptr;}
bool Layout(const SwUiLayout* l){return l&&l->anchor>=0&&l->anchor<=8&&std::isfinite(l->x)&&std::isfinite(l->y)&&std::isfinite(l->width)&&std::isfinite(l->height);}
bool String(const char* s){return s&&strnlen_s(s,4097)<=4096;}
bool HasRoom(const SwModHost* owner){size_t n=0;for(const auto& item:nodes)if(item.second->owner==owner)++n;return n<512&&next;}
SwHandle Insert(std::unique_ptr<Node> node){node->id=next++;SwHandle id=node->id;nodes[id]=std::move(node);return id;}
void Retire(SwHandle id){auto it=nodes.find(id);if(it==nodes.end())return;auto node=std::move(it->second);nodes.erase(it);node->owner=nullptr;node->native=nullptr;node->click=nullptr;node->data=nullptr;
    if(!node->retainedToken){return;}retired.push_back(std::move(node));}
void ForgetBranch(SwHandle id){std::vector<SwHandle> children;for(const auto& item:nodes)if(item.second->parent==id)children.push_back(item.first);for(auto child:children)ForgetBranch(child);Retire(id);}
SwResult Root(const SwModHost* owner,SwUiSurface surface,SwHandle* out){
    auto result=Check(owner);if(result!=SW_OK)return result;
    if(!out||surface<SW_SURFACE_MENU||surface>SW_SURFACE_EDITOR)return SW_INVALID_ARGUMENT;
    auto& s=surfaces[surface];if(!s.parent||!s.context)return SW_UNAVAILABLE;
    if(!HasRoom(owner))return SW_LIMIT_REACHED;
    SwUiLayout l={0,0,-1,-1,4};void* p=GetEngineApi()->ui->composite(s.context,s.parent,&l);if(!p)return SW_UNAVAILABLE;
    auto node=std::make_unique<Node>();node->owner=owner;node->surface=surface;node->native=p;
    *out=Insert(std::move(node));return SW_OK;
}
SwResult Text(const SwModHost* owner,SwHandle parent,const SwUiLayout* l,const char* text,uint32_t color,SwHandle* out){
    auto result=Check(owner);if(result!=SW_OK)return result;
    if(!out||!Layout(l)||!String(text))return SW_INVALID_ARGUMENT;
    Node* p=Find(owner,parent);if(!p)return SW_NOT_FOUND;if(p->text)return SW_INVALID_ARGUMENT;
    if(!HasRoom(owner))return SW_LIMIT_REACHED;
    void* n=GetEngineApi()->ui->text(surfaces[p->surface].context,p->native,l,text,3,color,kBodyFont,kLineSpacing);if(!n)return SW_UNAVAILABLE;
    auto node=std::make_unique<Node>();node->owner=owner;node->parent=parent;node->surface=p->surface;node->native=n;node->text=true;
    *out=Insert(std::move(node));return SW_OK;
}
SwResult Window(const SwModHost* owner,SwHandle parent,const SwUiLayout* l,const char* title,SwHandle* out){
    auto result=Check(owner);if(result!=SW_OK)return result;
    if(!out||!Layout(l)||!String(title))return SW_INVALID_ARGUMENT;
    Node* p=Find(owner,parent);if(!p)return SW_NOT_FOUND;if(p->text)return SW_INVALID_ARGUMENT;
    if(!HasRoom(owner))return SW_LIMIT_REACHED;
    auto* ui=GetEngineApi()->ui;auto& s=surfaces[p->surface];
    void* n=ui->composite(s.context,p->native,l);if(!n)return SW_UNAVAILABLE;
    SwUiLayout bg={0,0,-1,-1,4};ui->rectangle(s.context,n,&bg,0xee222222);
    SwUiLayout heading={.02f,-.02f,l->width-.04f,.045f,0};ui->text(s.context,n,&heading,title,0,0xffffffff,kTitleFont,kLineSpacing);
    auto node=std::make_unique<Node>();node->owner=owner;node->parent=parent;node->surface=p->surface;node->native=n;
    *out=Insert(std::move(node));return SW_OK;
}
bool ClickCpp(SwEventCallback fn,const SwEvent* e,void* data){try{fn(e,data);return true;}catch(...){return false;}}
bool SafeClick(SwEventCallback fn,const SwEvent* e,void* data){__try{return ClickCpp(fn,e,data);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
void Click(void*,void* data){auto* node=static_cast<Node*>(data);if(!node||!node->owner||!node->click||Check(node->owner)!=SW_OK)return;
    SwEvent e={sizeof(SwEvent),SW_EVENT_UI_CLICK,node->id,node->surface,1,0,RuntimePrimitives()->getMonotonicTime(),0};
    auto fn=node->click;void* user=node->data;if(!SafeClick(fn,&e,user)){node->click=nullptr;LogError("P0: native UI click callback failed; disabled");}}
SwResult Button(const SwModHost* owner,SwHandle parent,const SwUiLayout* l,const char* text,SwEventCallback callback,void* data,SwHandle* out){
    auto result=Check(owner);if(result!=SW_OK)return result;
    if(!out||!Layout(l)||l->width<=0||l->height<=0||!String(text))return SW_INVALID_ARGUMENT;
    Node* p=Find(owner,parent);if(!p)return SW_NOT_FOUND;if(p->text)return SW_INVALID_ARGUMENT;
    if(!HasRoom(owner)||retired.size()+nodes.size()>=8192)return SW_LIMIT_REACHED;
    auto node=std::make_unique<Node>();node->owner=owner;node->parent=parent;node->surface=p->surface;node->click=callback;node->data=data;node->retainedToken=true;
    auto* ui=GetEngineApi()->ui;auto& s=surfaces[p->surface];
    node->native=ui->rectangleButton(owner,s.context,p->native,l,0xff444444,0xff666666,0xff333333,true,&Click,node.get());if(!node->native)return SW_UNAVAILABLE;
    // Unlike composite roots, native text cannot use negative fill-parent bounds:
    // it wraps the caption into a tiny column. Give the centred label a real box.
    SwUiLayout label={0,0,l->width,l->height,4};ui->text(s.context,node->native,&label,text,4,0xffffffff,kButtonFont,kLineSpacing);
    *out=Insert(std::move(node));return SW_OK;
}
SwResult SetText(const SwModHost* owner,SwHandle id,const char* text){auto r=Check(owner);if(r!=SW_OK)return r;if(!String(text))return SW_INVALID_ARGUMENT;auto* n=Find(owner,id);if(!n)return SW_NOT_FOUND;if(!n->text)return SW_INVALID_ARGUMENT;GetEngineApi()->ui->setText(surfaces[n->surface].context,n->native,text);return SW_OK;}
SwResult Visible(const SwModHost* owner,SwHandle id,bool visible){auto r=Check(owner);if(r!=SW_OK)return r;auto* n=Find(owner,id);if(!n)return SW_NOT_FOUND;GetEngineApi()->ui->setVisible(n->native,visible);return SW_OK;}
SwResult Destroy(const SwModHost* owner,SwHandle id){auto r=Check(owner);if(r!=SW_OK)return r;auto* n=Find(owner,id);if(!n)return SW_NOT_FOUND;
    auto s=surfaces[n->surface];void* parent=n->parent?nodes.at(n->parent)->native:s.parent;void* native=n->native;
    ForgetBranch(id); // Invalidate callbacks BEFORE the native remove destroys the subtree.
    GetEngineApi()->ui->removeChild(s.context,parent,native);return SW_OK;
}
SwResult Notification(const SwModHost* owner,uint32_t level,const char* title,const char* message){auto r=Check(owner);if(r!=SW_OK)return r;if(level>3||!String(title)||!String(message))return SW_INVALID_ARGUMENT;Notify(static_cast<LogLevel>(level),title,message);return SW_OK;}
const SwNativeUiApi api={sizeof(SwNativeUiApi),&Root,&Window,&Text,&Button,&SetText,&Visible,&Destroy,&Notification};
}
const SwNativeUiApi* NativeUiPrimitives(){return &api;}
void InitializeModUi(SwUiSurface surface,void* parent,void* context){auto& s=surfaces[surface];if(s.parent)ForgetModUi(surface,s.parent);s={parent,context,true};}
void UpdateModUi(SwUiSurface surface,void* parent){auto& s=surfaces[surface];if(s.parent!=parent||!s.context)return;
    if(s.ready){s.ready=false;EmitModEvent(SW_EVENT_UI_READY,surface);}}
bool ModUiKeyboardCaptured(SwUiSurface surface,void* parent){auto& s=surfaces[surface];if(s.parent!=parent||!s.context)return true;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(s.context)+sw::off::kCtxFocused)!=nullptr;}
void ForgetModUi(SwUiSurface surface,void* parent){auto& s=surfaces[surface];if(s.parent!=parent)return;
    std::vector<SwHandle> ids;for(const auto& item:nodes)if(item.second->surface==surface)ids.push_back(item.first);for(auto id:ids)Retire(id);
    s={};EmitModEvent(SW_EVENT_UI_CLOSED,surface);
}
void ReleaseModUiOwner(const SwModHost* owner){
    std::vector<SwHandle> roots;for(const auto& item:nodes)if(item.second->owner==owner&&!item.second->parent)roots.push_back(item.first);
    for(auto id:roots){auto* n=Find(owner,id);if(!n)continue;
        if(RuntimePrimitives()->isGameThread())Destroy(owner,id);
        else ForgetBranch(id); // No dereference from a worker thread. Tree teardown owns remaining nodes.
    }
}
}
