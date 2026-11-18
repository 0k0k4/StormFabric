#pragma once
#include "../include/sw_mod_sdk.h"
namespace swml {
const SwImageApi* ImagePrimitives();
// True when the game imports GDI32!SwapBuffers and nobody redirected that import yet.
// Read-only: the import is patched lazily, on the first image created.
bool ImageOverlayAvailable();
// Called by the native UI update hooks every UI frame: images tied to a surface are
// shown only while that surface keeps pulsing.
void NoteImageSurfacePulse(SwUiSurface surface);
// Destroys every image of the owner (textures are freed on the render thread later).
void ReleaseImageOwner(const SwModHost* owner);
// What the SwapBuffers hook runs before presenting (exposed for tests). False on failure.
bool DrawImageOverlay(void* hdc);
// Render thread, current GL context (a native monitor draw): uploads pending pixels of the
// owner's image and returns a texture holding it in GL orientation (bottom row first, like a
// render target). Restores every GL binding it touches.
// srgb: store the copy as GL_SRGB8_ALPHA8 (same bytes, decoded by the sampler) when the
// native texture it replaces is sRGB-encoded.
bool ImageTextureForDraw(const SwModHost* owner, uint64_t image, bool srgb, unsigned* texture);
// Tests: route mouse input even when the system cursor is hidden.
void SetImageInputCursorCheck(bool enabled);
}
