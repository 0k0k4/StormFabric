// Real pass-through hooks with fake native calls; no hooks installed in a game.
#include "../src/custom_display_observer.cpp"
#include <cassert>
#include <stdexcept>
#include <vector>

namespace {
bool throwLog = false;
unsigned logs = 0, creates = 0, releases = 0;
unsigned requests = 0;
bool throwRequest = false;
DWORD thread = 0;
void* expectedSelf = nullptr; void* expectedQueue = nullptr;
int32_t expectedQuality = 0;
alignas(16) uint8_t monitor[0x600] = {}, definition[0x400] = {};
template<class T> void Put(void* base, size_t offset, T value) {
    memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}
void NativeCreate(void* self, void* queue, int32_t quality) {
    assert(self == expectedSelf && queue == expectedQueue && quality == expectedQuality);
    assert(GetCurrentThreadId() == thread); ++creates;
    Put(self, 0x4b8, uint32_t(32)); Put(self, 0x4bc, uint32_t(32));
    Put(self, 0x4c0, uint32_t(1));
    Put(self, 0x4c8, reinterpret_cast<void*>(0x1234)); Put(self, 0x4d0, queue);
    Put(self, 0x4d8, reinterpret_cast<void*>(0x5678)); Put(self, 0x4e0, queue);
}
void NativeRelease(void* self, void* queue) {
    assert(self == expectedSelf && queue == expectedQueue);
    assert(GetCurrentThreadId() == thread); ++releases;
    memset(static_cast<uint8_t*>(self) + 0x4c8, 0, 32);
}
void NativeRequest(void* queue,int32_t width,int32_t height,int32_t colour,int32_t depth,
                   swml::CustomDisplayResourceHandle* handle) {
    assert(queue == expectedQueue && GetCurrentThreadId() == thread);
    assert(colour == 5 && depth == 1);
    const auto scale = swml::CustomDisplaySupersamplingForQuality(expectedQuality);
    const bool video = requests % 2 == 0;
    assert(width == (video ? 24 * int32_t(scale) : 24));
    assert(height == (video ? 16 * int32_t(scale) : 16));
    ++requests;
    if (throwRequest) throw std::runtime_error("test queue failure after admission");
    handle->object = reinterpret_cast<void*>(uintptr_t(video ? 0x1234 : 0x5678));
    handle->manager = queue;
}
}
namespace swml {
void LogInfo(const char*, ...) { ++logs; if (throwLog) throw std::runtime_error("test logger failure"); }
void LogError(const char*, ...) {}
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
}
int wmain(int argc,wchar_t** argv) {
    using namespace swml;
    if (argc==3 && wcscmp(argv[1],L"--pin-test")==0) {
        HANDLE pin=PinSmallArchive(argv[2]);
        if (pin==INVALID_HANDLE_VALUE) return 2;
        HANDLE writer=CreateFileW(argv[2],GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        assert(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
        CloseHandle(pin);
        return 0;
    }
    CustomDisplayLayout l{0x58,0x4b8,0x4bc,0x4c0,0x4c8,0x4d8,0x288,0x328,89,90};
    Put(monitor, l.definition, static_cast<void*>(definition));
    const char* id = "swml_cd_probe_v1_1x1";
    const char* archive = "d7b0869911c6cc3f3204060c2854c674";
    Put(definition,l.definitionId,id);Put(definition,l.definitionId+8,uint32_t(strlen(id)));
    Put(definition,l.archiveId,archive);Put(definition,l.archiveId+8,uint32_t(strlen(archive)));
    CustomDisplaySnapshot s;
    std::vector<uint8_t> before(monitor,monitor+sizeof(monitor));
    assert(ReadCustomDisplaySnapshot(monitor,l,s) && s.valid);
    assert(strcmp(s.definitionId,id)==0 && strcmp(s.archiveId,archive)==0);
    assert(memcmp(before.data(),monitor,sizeof(monitor))==0);
    auto rejected = s;
    assert(!ReadCustomDisplaySnapshot(nullptr,l,rejected));
    assert(rejected.valid && strcmp(rejected.definitionId,id)==0);
    auto invalid = l;invalid.video=UINT32_MAX;
    assert(!ReadCustomDisplaySnapshot(monitor,invalid,rejected));
    Put(definition,l.definitionId+8,uint32_t(128));
    assert(!ReadCustomDisplaySnapshot(monitor,l,rejected));
    Put(definition,l.definitionId+8,uint32_t(2));
    Put(definition,l.definitionId,"a\nb");
    assert(!ReadCustomDisplaySnapshot(monitor,l,rejected));
    Put(definition,l.definitionId,reinterpret_cast<const char*>(1));
    assert(!ReadCustomDisplaySnapshot(monitor,l,rejected));
    auto* inaccessible=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS);
    assert(inaccessible);
    assert(!ReadCustomDisplaySnapshot(inaccessible,l,rejected));
    Put(monitor,l.definition,inaccessible);
    assert(!ReadCustomDisplaySnapshot(monitor,l,rejected));
    Put(monitor,l.definition,static_cast<void*>(definition));
    VirtualFree(inaccessible,0,MEM_RELEASE);
    Put(definition,l.definitionId,id);Put(definition,l.definitionId+8,uint32_t(strlen(id)));

    g_layout=l;g_create=&NativeCreate;g_release=&NativeRelease;
    expectedSelf=monitor;expectedQueue=&l;thread=GetCurrentThreadId();
    for (bool loggerFails : {false,true}) {
        throwLog=loggerFails;
        for(int32_t quality:{1,2,3,-1,INT32_MIN,INT32_MAX}) {
            expectedQuality=quality;
            unsigned start=creates;
            Create(monitor,expectedQueue,quality);
            assert(creates==start+1);
            assert(ReadCustomDisplaySnapshot(monitor,l,s));
            assert(s.width==32 && s.height==32 && s.video==reinterpret_cast<void*>(0x1234));
            assert(s.videoManager==expectedQueue && s.overlayManager==expectedQueue);
            unsigned end=releases;
            Release(monitor,expectedQueue);
            assert(releases==end+1);
            assert(ReadCustomDisplaySnapshot(monitor,l,s) && !s.video && !s.overlay);
        }
    }
    // Logging cap leaves every subsequent native invocation intact.
    g_events=kMaxEvents;unsigned startLogs=logs;
    expectedQuality=3;
    Create(monitor,expectedQueue,3);Release(monitor,expectedQueue);
    assert(logs==startLogs && creates==13 && releases==13);
    // An unreadable optional snapshot must not swallow the stock invocation.
    g_events=0;g_layout.definitionId=0;
    Create(monitor,expectedQueue,3);Release(monitor,expectedQueue);
    assert(creates==14 && releases==14);
    throwLog=false;
    // Not opted in: must not even dereference a bogus module argument.
    char old[32768]={};DWORD oldSize=GetEnvironmentVariableA("SWML_CD_OBSERVE",old,sizeof(old));
    for(const char* value:{"", "0", "true", "01", "1extra", "111111111111111111"}) {
        SetEnvironmentVariableA("SWML_CD_OBSERVE",value);
        assert(!InstallCustomDisplayObserver(reinterpret_cast<uint8_t*>(1)));
    }
    SetEnvironmentVariableA("SWML_CD_OBSERVE",oldSize?old:nullptr);
    // The local low-resolution experiment is independently opt-in and must not
    // affect stock/witness definitions, mismatched archives or other object types.
    g_layout=l;g_layout.minX=0xc0;g_layout.maxX=0xcc;g_layout.minZ=0xc8;g_layout.maxZ=0xd4;
    g_monitorTable=&l;Put(monitor,0,g_monitorTable);g_request=&NativeRequest;
    auto setId=[&](const char* name,const char* content) {
        Put(definition,l.definitionId,name);Put(definition,l.definitionId+8,uint32_t(strlen(name)));
        Put(definition,l.archiveId,content);Put(definition,l.archiveId+8,uint32_t(strlen(content)));
    };
    setId(kSmallId,kSmallArchive);
    assert(!TrySmallCreate(monitor,expectedQueue,3) && requests==0);
    g_smallEnabled=true;
    for(auto identity:{"monitor_1", "swml_cd_probe_v1_1x1", "swml_cd_v1_b1x1_p480x272"}) {
        setId(identity,kSmallArchive);
        assert(!TrySmallCreate(monitor,expectedQueue,3) && requests==0);
    }
    setId(kSmallId,archive);
    assert(!TrySmallCreate(monitor,expectedQueue,3) && requests==0);
    setId(kSmallId,kSmallArchive);
    Put(monitor,0,static_cast<void*>(nullptr));
    assert(!TrySmallCreate(monitor,expectedQueue,3) && requests==0);
    Put(monitor,0,g_monitorTable);
    Put(definition,g_layout.maxX,int32_t(1));
    assert(!TrySmallCreate(monitor,expectedQueue,3) && requests==0);
    Put(definition,g_layout.maxX,int32_t(0));
    assert(!TrySmallCreate(monitor,nullptr,3) && requests==0);
    g_events=kMaxEvents; // this path must not rely on observation logging capacity
    for(int32_t quality:{1,2,3,0,-1}) {
        expectedQuality=quality;
        unsigned stockCreates=creates;
        unsigned previousRequests=requests;
        Create(monitor,expectedQueue,quality);
        assert(creates==stockCreates && requests==previousRequests+2);
        assert(ReadCustomDisplaySnapshot(monitor,g_layout,s));
        assert(s.width==24 && s.height==16 && s.scale==CustomDisplaySupersamplingForQuality(quality));
        // A duplicate create must neither queue again nor corrupt live dimensions.
        Create(monitor,expectedQueue,quality);
        assert(requests==previousRequests+2 && creates==stockCreates);
        assert(ReadCustomDisplaySnapshot(monitor,g_layout,s) && s.width==24 && s.height==16);
        const auto charged=g_smallAdmission.ChargedTargetBytes();
        Release(monitor,expectedQueue);
        assert(g_smallAdmission.ChargedTargetBytes()==charged);
    }
    // A request exception never grants credit back and must not trigger the
    // stock creator after a custom event may already have been published.
    const auto charged=g_smallAdmission.ChargedTargetBytes();
    expectedQuality=3;throwRequest=true;
    unsigned stockCreates=creates;
    bool threw=false;
    try { Create(monitor,expectedQueue,3); } catch (const std::runtime_error&) { threw=true; }
    assert(threw && creates==stockCreates && g_smallAdmission.ChargedTargetBytes()==charged+78336);
    throwRequest=false;
    // Reset only the spy's alternating call index, NEVER the contribution ledger.
    requests=0;
    for(unsigned attempt=0;attempt<200;++attempt) {
        memset(monitor+l.video,0,32);
        Create(monitor,expectedQueue,3);
        if(creates>stockCreates) break;
    }
    assert(creates==stockCreates+1 && g_smallAdmission.ChargedTargetBytes()<=8*1024*1024);
    assert(ReadCustomDisplaySnapshot(monitor,g_layout,s) && s.width==32 && s.height==32);
    g_smallEnabled=false;
    // A bogus/missing approved archive never enables the experiment.
    assert(PinSmallArchive(L"build\\tests\\no-such-approved-archive.bin")==INVALID_HANDLE_VALUE);
    puts("PASS: read-only bounded snapshots, fault containment, exact pass-through arguments/thread, logging faults/cap, strict opt-in (no game hooks)");
    puts("PASS: separate small-resolution gate, exact identity/archive/type/bounds, native thread/quality/format/handle order, lifetime budget and stock isolation (test doubles only)");
}
