#include "engine_discovery.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>
#include <limits>
#include <climits>

namespace swml { namespace discovery {
namespace {
#include "engine_discovery.generated.h"
constexpr uint32_t kExecute = IMAGE_SCN_MEM_EXECUTE;
constexpr uint32_t kRead = IMAGE_SCN_MEM_READ;
constexpr uint32_t kWrite = IMAGE_SCN_MEM_WRITE;
const char* StatusName(Status s) {
    switch (s) { case Status::Found: return "FOUND"; case Status::Ambiguous: return "AMBIGUOUS";
    case Status::Invalid: return "INVALID"; default: return "MISSING"; }
}
template<class T> bool Load(const Image& image, uint32_t rva, T& out) {
    if (!image.Contains(rva,sizeof(T))) return false;
    memcpy(&out,image.base+rva,sizeof(T)); return true;
}
bool CStr(const Image& image, uint32_t rva, const char* expected, bool ignoreCase = false) {
    size_t n = strlen(expected)+1;
    if (!image.Readable(rva,n)) return false;
    return ignoreCase ? _strnicmp(reinterpret_cast<const char*>(image.base+rva),expected,n)==0 :
        memcmp(image.base+rva,expected,n)==0;
}
std::vector<uint32_t> FindTable(const Image& image, const Table& table) {
    std::vector<uint32_t> result;
    // MSVC x64 COL: signature, offset, cdOffset, TypeDescriptor RVA,
    // ClassHierarchyDescriptor RVA, self RVA. RTTI is read-only, not executable.
    for (const Range& s : image.sections) {
        if (!(s.flags&kRead) || (s.flags&(kWrite|kExecute))) continue;
        for (uint64_t p=s.start; p+8<=uint64_t(s.start)+s.size; p+=8) {
            uint32_t col=0;
            if (!image.Pointer(uint32_t(p),col) || !image.ReadOnly(col,24)) continue;
            uint32_t signature=0, offset=0, type=0, self=0;
            Load(image,col,signature); Load(image,col+4,offset); Load(image,col+12,type);Load(image,col+20,self);
            if (signature!=1 || offset!=0 || self!=col || type>UINT32_MAX-16 ||
                // MSVC places TypeDescriptors in .data (their cache field is mutable).
                // The COL and vtable must be read-only; the bounded type name need not be.
                !image.Readable(type,16+strlen(table.type)+1) || image.Executable(type,1) ||
                !CStr(image,type+16,table.type)) continue;
            uint32_t start=uint32_t(p+8);
            if (image.ReadOnly(start,(size_t(table.slots[table.count-1].index)+1)*8)) result.push_back(start);
        }
    }
    return result;
}
const Contract* ByKey(uint32_t key) {
    for (const auto& c : kDiscoveryContracts) if (c.key==key) return &c;
    return nullptr;
}
template<class R> auto ByKey(R& result, uint32_t key) -> decltype(&result.symbols[0]) {
    for (auto& s : result.symbols) if (s.key==key) return &s;
    return nullptr;
}
bool Relation(const Image& image, const Contract& contract, uint32_t start, const Result& result, std::string& reason) {
    for (size_t j=0;j<contract.edgeCount;++j) {
        const Edge& edge=contract.edges[j]; uint32_t target=0;
        char detail[240];
        if (!DecodeEdge(image,start,edge,target)) {
            snprintf(detail,sizeof(detail),"rel32 at +0x%X decodes outside executable/image bounds",edge.offset);
            reason=detail;return false;
        }
        const Contract* dependency=ByKey(edge.target);
        if (dependency) {
            const auto* symbol=ByKey(result,edge.target);
            if(symbol && symbol->status==Status::Invalid) {
                snprintf(detail,sizeof(detail),"dependency %s rejected (key 0x%X); see its reason",dependency->name,edge.target);
                reason=detail;return false;
            }
            uint32_t resolved=result.Address(edge.target);
            if (resolved) {
                if (target!=resolved) {
                    snprintf(detail,sizeof(detail),"rel32 +0x%X -> 0x%X, expected resolved %s at 0x%X",edge.offset,target,dependency->name,resolved);
                    reason=detail;return false;
                }
            } else if (!Match(image,*dependency,target)) {
                if(!image.Executable(target,dependency->size)) {
                    snprintf(detail,sizeof(detail),"dependency %s -> 0x%X has invalid executable span (%u bytes)",dependency->name,target,dependency->size);
                } else {
                    uint32_t offset=0;
                    for(;offset<dependency->size;++offset) if((image.base[target+offset]&uint8_t(dependency->mask[offset]))!=
                        (uint8_t(dependency->bytes[offset])&uint8_t(dependency->mask[offset])))break;
                    if(offset==dependency->size)
                        snprintf(detail,sizeof(detail),"%s at 0x%X: indexed jump-table/base/target validation failed (rel32 +0x%X)",dependency->name,target,edge.offset);
                    else snprintf(detail,sizeof(detail),"%s at 0x%X differs +0x%X (expected %02X, got %02X; rel32 +0x%X)",dependency->name,target,offset,
                        unsigned(uint8_t(dependency->bytes[offset])),unsigned(image.base[target+offset]),edge.offset);
                }
                reason=detail;return false;
            }
        } else if (edge.flow && !image.Executable(target,1)) {
            snprintf(detail,sizeof(detail),"call/jump rel32 +0x%X -> non-executable RVA 0x%X",edge.offset,target);
            reason=detail;return false;
        }
        // An unknown data reference is intentionally NOT a layout certificate.
    }
    return true;
}
void Report(Progress progress,const char* phase,size_t n,size_t total,const Symbol& s) {
    if (!progress) return;
    char detail[640];
    snprintf(detail,sizeof(detail),"%s: %s (RVA 0x%X, candidates %zu)%s%s",s.name.c_str(),StatusName(s.status),s.rva,s.candidates.size(),
        s.reason.empty()?"":" | ",s.reason.c_str());
    progress(phase,n,total,detail,s.status!=Status::Found);
}
struct ScanContext { Progress progress; size_t done, total; unsigned lastBucket=UINT_MAX; };
thread_local ScanContext* g_scanContext=nullptr;
struct ScanScope {
    ScanContext* previous;
    explicit ScanScope(ScanContext& context):previous(g_scanContext){g_scanContext=&context;}
    ~ScanScope(){g_scanContext=previous;}
};
void PhaseScanProgress(const char* symbol,size_t done,size_t total,const char*,bool) {
    auto* context=g_scanContext;
    if(!context || !context->progress)return;
    unsigned bucket=total ? unsigned((std::min)(size_t(10),done*10/total)) : 0;
    if(bucket==context->lastBucket)return;
    context->lastBucket=bucket;
    char detail[240];snprintf(detail,sizeof(detail),"scanning executable sections: %s (local %u%%)",symbol,bucket*10);
    // The outer bar counts completed symbols; local byte progress is detail only.
    // This avoids alternating 60% overall with 2% for each new function scan.
    context->progress("Functions",context->done,context->total,detail,false);
}
}
bool Image::Contains(uint32_t rva, size_t bytes) const { return base && rva<=size && bytes<=size-rva; }
bool Image::Executable(uint32_t rva,size_t bytes) const {
    if (!Contains(rva,bytes)) return false;
    for (const auto& s:sections) if ((s.flags&kExecute) && rva>=s.start &&
        uint64_t(rva)+bytes<=uint64_t(s.start)+s.size) return true;
    return false;
}
bool Image::ReadOnly(uint32_t rva,size_t bytes) const {
    if (!Contains(rva,bytes)) return false;
    for (const auto& s:sections) if ((s.flags&kRead) && !(s.flags&(kWrite|kExecute)) && rva>=s.start &&
        uint64_t(rva)+bytes<=uint64_t(s.start)+s.size) return true;
    return false;
}
bool Image::Readable(uint32_t rva,size_t bytes) const {
    if(!Contains(rva,bytes))return false;
    if(uint64_t(rva)+bytes<=headers)return true;
    for(const auto& s:sections) if((s.flags&kRead) && rva>=s.start &&
        uint64_t(rva)+bytes<=uint64_t(s.start)+s.size)return true;
    return false;
}
bool Image::Pointer(uint32_t location,uint32_t& rva) const {
    uint64_t value=0;
    if(!Readable(location,8) || !Load(*this,location,value))return false;
    uint64_t baseAddress=reinterpret_cast<uint64_t>(base);
    if(value<baseAddress || value-baseAddress>=size)return false;
    rva=uint32_t(value-baseAddress);return true;
}
bool ParseImage(const uint8_t* base,size_t bytes,Image& out) {
    out={}; Image temp; temp.base=base;temp.size=bytes;
    IMAGE_DOS_HEADER dos={};IMAGE_NT_HEADERS64 nt={};
    if (!Load(temp,0,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 ||
        !Load(temp,uint32_t(dos.e_lfanew),nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.SizeOfImage>bytes || nt.OptionalHeader.SizeOfImage<sizeof(dos) ||
        nt.OptionalHeader.SizeOfHeaders>nt.OptionalHeader.SizeOfImage ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96) return false;
    temp.size=nt.OptionalHeader.SizeOfImage; temp.timestamp=nt.FileHeader.TimeDateStamp;
    temp.headers=nt.OptionalHeader.SizeOfHeaders;
    uint64_t section=uint64_t(dos.e_lfanew)+sizeof(IMAGE_NT_HEADERS64);
    if (section>UINT32_MAX || !temp.Contains(uint32_t(section),nt.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER))) return false;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER s={};Load(temp,uint32_t(section+i*sizeof(s)),s);
        uint32_t size=(std::max)(s.Misc.VirtualSize,s.SizeOfRawData);
        if(!size)continue;
        if(!temp.Contains(s.VirtualAddress,size))return false;
        for(const auto& old:temp.sections) if(uint64_t(s.VirtualAddress)<uint64_t(old.start)+old.size &&
            uint64_t(old.start)<uint64_t(s.VirtualAddress)+size)return false;
        // Windows can discard/decommit .reloc before or after DLL initialization.
        // Never inspect a discardable section for code or RTTI.
        if(!(s.Characteristics&IMAGE_SCN_MEM_DISCARDABLE))
            temp.sections.push_back({s.VirtualAddress,size,s.Characteristics});
    }
    out=std::move(temp);return true;
}
bool Match(const Image& image,const Contract& c,uint32_t start) {
    if (!c.size || !image.Executable(start,c.size))return false;
    for(size_t i=0;i<c.size;++i) if((image.base[start+i]&uint8_t(c.mask[i]))!=(uint8_t(c.bytes[i])&uint8_t(c.mask[i])))return false;
    for(size_t i=0;i<c.rvaTableCount;++i) {
        const auto& table=c.rvaTables[i];uint32_t tableRva=0,baseRva=0;
        if(table.operand>c.size || c.size-table.operand<4 || table.baseEnd>c.size ||
            !table.count || table.count>256 || !table.targetOffsets ||
            !DecodeEdge(image,start,{table.baseOperand,table.baseEnd,0,false},baseRva) || baseRva!=0 ||
            !Load(image,start+table.operand,tableRva) || !image.ReadOnly(tableRva,table.count*4))return false;
        for(size_t j=0;j<table.count;++j) {
            uint32_t target=0;
            if(table.targetOffsets[j]>=c.size || !Load(image,tableRva+uint32_t(j*4),target) ||
                uint64_t(start)+table.targetOffsets[j]!=target || !image.Executable(target,1))return false;
        }
    }
    return true;
}
bool DecodeEdge(const Image& image,uint32_t start,const Edge& edge,uint32_t& target) {
    if(edge.offset>edge.end || edge.end-edge.offset<4 || !image.Executable(start,edge.end))return false;
    int32_t displacement=0;
    if(!Load(image,start+edge.offset,displacement))return false;
    int64_t decoded=int64_t(start)+edge.end+displacement;
    if(decoded<0 || uint64_t(decoded)>=image.size)return false;
    target=uint32_t(decoded);return true;
}
Symbol Find(const Image& image,const Contract& c,bool fast,Progress progress) {
    Symbol result;result.key=c.key;result.name=c.name;
    if(!c.size || !c.bytes || !c.mask || c.key<c.start){result.status=Status::Invalid;return result;}
    if(fast && Match(image,c,c.start)) {
        result.candidates.push_back(c.start);result.rva=c.key;result.status=Status::Found;return result;
    }
    // Find the longest fixed run to avoid quadratic full-body comparisons.
    size_t anchor=0,length=0,run=0;
    for(size_t i=0;i<c.size;++i) {
        run=uint8_t(c.mask[i])==255 ? run+1 : 0;
        if(run>length){length=run;anchor=i+1-run;}
    }
    if(!length){result.status=Status::Invalid;return result;}
    size_t total=0,done=0;
    for(const auto& s:image.sections) if(s.flags&kExecute)total+=s.size;
    for(const auto& s:image.sections) {
        if(!(s.flags&kExecute) || s.size<c.size)continue;
        const uint8_t* end=image.base+s.start+s.size;
        const uint8_t* cursor=image.base+s.start+anchor;
        while(cursor+length<=end) {
            // Chunked searching keeps terminal progress alive even for missing symbols.
            const uint8_t* chunkEnd=(std::min)(end,cursor+(std::min)(size_t(end-cursor),size_t(256*1024+length-1)));
            const auto* found=std::search(cursor,chunkEnd,reinterpret_cast<const uint8_t*>(c.bytes)+anchor,
                reinterpret_cast<const uint8_t*>(c.bytes)+anchor+length);
            if(found==chunkEnd) {
                size_t advance=size_t(chunkEnd-cursor)-length+1;
                cursor+=advance;
            } else {
                uint32_t start=uint32_t(found-image.base-anchor);
                if(start>=s.start && Match(image,c,start)) {
                    result.candidates.push_back(start);
                    // Bounded memory for pathological trivial/duplicate functions.
                    if(result.candidates.size()>256){result.status=Status::Ambiguous;return result;}
                }
                cursor=found+1;
            }
            if(progress) progress(c.name,done+size_t(cursor-(image.base+s.start)),total,"scanning executable sections",false);
        }
        done+=s.size;
    }
    if(result.candidates.size()==1) {
        result.rva=result.candidates[0]+(c.key-c.start);result.status=Status::Found;
    } else if(result.candidates.size()>1) {
        result.status=Status::Ambiguous;result.reason="multiple full-body matches; waiting for corroboration";
    } else result.reason="no complete masked-body match; not proof of semantic incompatibility";
    return result;
}
bool Result::GroupReady(uint32_t group) const {
    if(!validImage)return false;
    for(const auto& c:kDiscoveryContracts)if((c.groups&group) && !Address(c.key))return false;
    for(const auto& t:kDiscoveryTables)if((t.group&group) && !Address(t.key))return false;
    return true;
}
uint32_t FindImport(const Image& image,const char* dll,const char* name) {
    IMAGE_DOS_HEADER dos={};IMAGE_NT_HEADERS64 nt={};
    if(!Load(image,0,dos) || dos.e_lfanew<0 || !Load(image,uint32_t(dos.e_lfanew),nt) ||
        nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_IMPORT)return 0;
    const auto& dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!dir.VirtualAddress || !image.Readable(dir.VirtualAddress,dir.Size))return 0;
    uint32_t result=0;
    for(size_t offset=0;offset+sizeof(IMAGE_IMPORT_DESCRIPTOR)<=dir.Size;offset+=sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR d={};Load(image,dir.VirtualAddress+uint32_t(offset),d);
        if(!d.Name)break;
        if(!CStr(image,d.Name,dll,true))continue;
        // OriginalFirstThunk may be absent; loaded IAT then contains resolved
        // pointers rather than import names. Do not guess in that case.
        if(!d.OriginalFirstThunk || !d.FirstThunk)return 0;
        for(size_t i=0;i<image.size/8;++i) {
            uint64_t lookup=uint64_t(d.OriginalFirstThunk)+i*8, slot=uint64_t(d.FirstThunk)+i*8, thunk=0;
            if(lookup>UINT32_MAX || slot>UINT32_MAX || !image.Readable(uint32_t(lookup),8) ||
                !Load(image,uint32_t(lookup),thunk) || !image.Readable(uint32_t(slot),8))return 0;
            if(!thunk)break;
            if(thunk&IMAGE_ORDINAL_FLAG64)continue;
            if(thunk>UINT32_MAX-2)return 0;
            if(CStr(image,uint32_t(thunk)+2,name)) {
                if(result)return 0; // duplicated import identity is ambiguous
                result=uint32_t(slot);
            }
        }
    }
    return result;
}
Result Discover(const uint8_t* base,size_t readableBytes,Progress progress,bool forceScan) {
    Result result; Image image;
    if(progress)progress("PE image",0,1,"checking x64 headers and section boundaries",false);
    if(!ParseImage(base,readableBytes,image)) {
        if(progress)progress("PE image",1,1,"INVALID: malformed or unsupported PE image; no hooks",true);
        return result;
    }
    result.validImage=true;
    result.reviewedBuild=image.timestamp==0x6a392154 && image.size==0xd89000;
    if(progress)progress("PE image",1,1,result.reviewedBuild?"reviewed build: validated RVA fast path":"unknown build: read-only discovery, layout certification pending",false);
    size_t total=0;
    for(const auto& c:kDiscoveryContracts)if(c.groups)++total;
    size_t n=0;
    for(const auto& c:kDiscoveryContracts) {
        if(!c.groups)continue; // Callee witnesses validate at decoded targets, not by a second scan.
        ScanContext context={progress,n,total};ScanScope scope(context);
        result.symbols.push_back(Find(image,c,result.reviewedBuild&&!forceScan,&PhaseScanProgress));
        Report(progress,"Functions",++n,total,result.symbols.back());
    }
    n=0;
    for(const auto& table:kDiscoveryTables) {
        Symbol s;s.key=table.key;s.name=table.type;
        auto candidates=FindTable(image,table);
        for(uint32_t start:candidates) {
            bool valid=true;
            for(size_t i=0;i<table.count;++i) {
                const Slot& slot=table.slots[i];uint32_t target=0;
                const auto* contract=ByKey(slot.target);
                if(!contract || !image.Pointer(start+slot.index*8,target) || !Match(image,*contract,target)) {valid=false;break;}
                const Symbol* existing=ByKey(result,slot.target);
                if(existing && existing->status==Status::Found && existing->rva!=target) {valid=false;break;}
            }
            if(valid)s.candidates.push_back(start);
        }
        if(s.candidates.size()==1) {
            s.status=Status::Found;s.rva=s.candidates[0];
            // Class identity plus all required slot bodies disambiguates shared trivial methods.
            for(size_t i=0;i<table.count;++i) {
                uint32_t target=0;image.Pointer(s.rva+table.slots[i].index*8,target);
                auto* fn=ByKey(result,table.slots[i].target);
                if(fn && fn->status==Status::Ambiguous && fn->candidates.size()<=256 &&
                    std::find(fn->candidates.begin(),fn->candidates.end(),target)!=fn->candidates.end()) {
                    fn->rva=target;fn->status=Status::Found;
                    fn->reason.clear();
                }
            }
        } else if(s.candidates.size()>1)s.status=Status::Ambiguous;
        result.symbols.push_back(std::move(s));
        Report(progress,"RTTI/vtables",++n,std::size(kDiscoveryTables),result.symbols.back());
    }
    n=0;
    // Check decoded external calls/data references against resolved symbols and
    // full direct-callee witnesses. Propagate invalid dependencies to a fixed point.
    bool changed;
    do {
        changed=false;
        for(auto& s:result.symbols) {
            const auto* c=ByKey(s.key);
            if(s.status!=Status::Found || !c)continue;
            uint32_t start=s.rva-(c->key-c->start);
            if(!Relation(image,*c,start,result,s.reason)) {s.status=Status::Invalid;changed=true;}
        }
    } while(changed);
    for(const auto& t:kDiscoveryTables) {
        auto* table=ByKey(result,t.key);
        if(!table || table->status!=Status::Found)continue;
        for(size_t i=0;i<t.count;++i) {
            const auto* method=ByKey(result,t.slots[i].target);
            if(method && method->status==Status::Invalid) {
                char detail[240];snprintf(detail,sizeof(detail),"slot %u dependency %s rejected; see its reason",t.slots[i].index,method->name.c_str());
                table->status=Status::Invalid;table->reason=detail;break;
            }
        }
    }
    for(const auto& s:result.symbols)Report(progress,"Relationships",++n,result.symbols.size(),s);
    result.drawElementsIat=FindImport(image,"OPENGL32.dll","glDrawElements");
    if(progress)progress("OpenGL import",1,1,result.drawElementsIat?"FOUND: glDrawElements via import identity":"MISSING/INVALID: glDrawElements import",!result.drawElementsIat);
    size_t good=0;for(const auto& s:result.symbols)if(s.status==Status::Found)++good;
    if(progress) {
        char summary[256];snprintf(summary,sizeof(summary),"%zu/%zu targets found; %s; discovery never installs hooks",good,result.symbols.size(),
            result.reviewedBuild?"reviewed-layout gate retained":"UNKNOWN LAYOUT: activation disabled");
        progress("Discovery complete",1,1,summary,good!=result.symbols.size() || !result.reviewedBuild);
    }
    return result;
}
namespace {
bool LoadedSize(uint8_t* base,size_t& size,size_t& headers) {
    // Tiny SEH boundary without C++ destructors. No arbitrary profile execution.
    __try {
        if(!base)return false;
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>1024*1024)return false;
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage>512*1024*1024)return false;
        size=nt->OptionalHeader.SizeOfImage;
        headers=nt->OptionalHeader.SizeOfHeaders;
        if(headers>size || headers>1024*1024 ||
            uint64_t(dos->e_lfanew)+sizeof(*nt)+uint64_t(nt->FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER)>headers)return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ReadableRange(uint8_t* base,size_t size) {
    uintptr_t cursor=reinterpret_cast<uintptr_t>(base),end=cursor+size;
    if(end<cursor)return false;
    while(cursor<end) {
        MEMORY_BASIC_INFORMATION info={};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || info.Protect==PAGE_EXECUTE)return false;
        uintptr_t next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=cursor)return false;
        cursor=next;
    }
    return true;
}
}
static Result s_cachedResult;
static bool s_hasCachedResult = false;
Result DiscoverLoadedEngine(uint8_t* base) {
    if (s_hasCachedResult) return s_cachedResult;
    size_t size=0,headers=0;
    if(!LoadedSize(base,size,headers) || !ReadableRange(base,headers)){LogError("Discovery: invalid loaded PE image");return {};}
    // Check readable committed coverage before any C++ scan; inaccessible sections
    // are rejected instead of risking an access violation during startup.
    Image image;
    if(!ParseImage(base,size,image)){LogError("Discovery: invalid loaded PE sections");return {};}
    for(const auto& section:image.sections) {
        if(!ReadableRange(base+section.start,section.size)) {
            LogError("Discovery: unreadable image region; discovery refused");return {};
        }
    }
    s_cachedResult = Discover(base,size,&LogDiscoveryProgress);
    s_hasCachedResult = true;
    return s_cachedResult;
}
} }
