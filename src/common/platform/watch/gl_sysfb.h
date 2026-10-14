#pragma once
#include "v_video.h"

// These types keep the desktop renderer's compiled interfaces linkable. The
// Watch factory exclusively creates its own software framebuffer.
class SystemBaseFrameBuffer : public DFrameBuffer
{
public:
    SystemBaseFrameBuffer(void *, bool) : DFrameBuffer(320, 240) { }
    bool IsFullscreen() override { return true; }
    int GetClientWidth() override { return 320; }
    int GetClientHeight() override { return 240; }
    void ToggleFullscreen(bool) override { }
    void SetWindowSize(int, int) override { }
};
class SystemGLFrameBuffer : public SystemBaseFrameBuffer
{
public:
    SystemGLFrameBuffer(void *monitor, bool fullscreen) : SystemBaseFrameBuffer(monitor, fullscreen) { }
    void SetVSync(bool) override { }
    void SwapBuffers() { }
};
