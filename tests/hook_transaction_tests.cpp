#include "../src/hook_transaction.h"
#include <cassert>
#include <cstdio>
static unsigned calls = 0, failOn = 0;
static BOOL WINAPI Protect(LPVOID p, SIZE_T n, DWORD flags, PDWORD old) {
    if (++calls == failOn) return FALSE;
    return VirtualProtect(p,n,flags,old);
}
int main() {
    SYSTEM_INFO info; GetSystemInfo(&info);
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr,info.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    assert(memory);
    auto** a = reinterpret_cast<void**>(memory);
    auto** b = reinterpret_cast<void**>(memory+info.dwPageSize);
    *a=reinterpret_cast<void*>(1);*b=reinterpret_cast<void*>(2);
    DWORD old; assert(VirtualProtect(memory,info.dwPageSize*2,PAGE_READONLY,&old));
    void *first=nullptr,*second=nullptr;
    swml::SlotPatch patches[]={{a,*a,reinterpret_cast<void*>(3),&first},{b,*b,reinterpret_cast<void*>(4),&second}};
    calls=0;failOn=2;
    assert(!swml::PatchSlots(patches,2,&Protect));
    assert(*a==reinterpret_cast<void*>(1) && *b==reinterpret_cast<void*>(2) && !first && !second);
    MEMORY_BASIC_INFORMATION page; VirtualQuery(memory,&page,sizeof(page)); assert(page.Protect==PAGE_READONLY);
    patches[1].expected=reinterpret_cast<void*>(5);calls=0;failOn=0;
    assert(!swml::PatchSlots(patches,2,&Protect) && calls==0);
    patches[1].expected=reinterpret_cast<void*>(2);
    assert(swml::PatchSlots(patches,2,&Protect));
    assert(*a==reinterpret_cast<void*>(3) && *b==reinterpret_cast<void*>(4));
    assert(first==reinterpret_cast<void*>(1) && second==reinterpret_cast<void*>(2));
    VirtualQuery(memory,&page,sizeof(page)); assert(page.Protect==PAGE_READONLY);
    VirtualFree(memory,0,MEM_RELEASE);
    puts("PASS: two-page vtable installation, protection failure/conflict before writes, original pointers/protection retained");
}
