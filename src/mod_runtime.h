#pragma once
#include "../include/sw_mod_sdk.h"
namespace swml {
void RegisterRuntimeOwner(const SwModHost*);
void ReleaseRuntimeOwner(const SwModHost*);
bool RuntimeOwnerValid(const SwModHost*);
void PumpModRuntime(SwUiSurface, bool focused, bool keyboardCaptured);
void EmitModEvent(SwEventType, SwUiSurface, SwHandle=0,uint32_t=0,const SwModHost* onlyOwner=nullptr);
void QueueModLuaEvent(SwEventType,SwHandle);
const SwRuntimeApi* RuntimePrimitives();
const SwInputApi* InputPrimitives();
void SetRuntimeCapabilities(uint64_t);
}
