#pragma once

// Entry point for platform code that may be plain C++. The layer is a
// CAMetalLayer the renderer draws into.

class SystemBaseFrameBuffer;

SystemBaseFrameBuffer *CreateMetalFrameBuffer(void *metalLayer, bool fullscreen);
bool MetalIsSupported();
