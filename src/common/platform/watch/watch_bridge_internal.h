#pragma once
#include <cstdint>
void WatchPresent(const uint8_t *pixels, int width, int height, int pitch);
void WatchWaitForActive();
int WatchFrameLimit();
void WatchScreenSize(int &width, int &height);
