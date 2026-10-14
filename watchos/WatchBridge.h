#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Native screen size in pixels; call before GZDWatchStart.
void GZDWatchSetScreen(int width, int height);
void GZDWatchStart(const char *iwad);
void GZDWatchSetActive(bool active);
CGImageRef GZDWatchCopyImage(void) CF_RETURNS_RETAINED;
uint64_t GZDWatchFrameCount(void);
const char *GZDWatchError(void);
#ifdef __cplusplus
}
#endif
