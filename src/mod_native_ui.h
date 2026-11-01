#pragma once
#include "../include/sw_mod_sdk.h"
namespace swml {
const SwNativeUiApi* NativeUiPrimitives();
void InitializeModUi(SwUiSurface,void* parent,void* context);
void UpdateModUi(SwUiSurface,void* parent);
bool ModUiKeyboardCaptured(SwUiSurface,void* parent);
void ForgetModUi(SwUiSurface,void* parent);
void ReleaseModUiOwner(const SwModHost*);
}
