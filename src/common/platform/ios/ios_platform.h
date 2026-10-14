#pragma once
struct SDL_Window;
void IOS_AttachControls(SDL_Window *window);
void IOS_UpdatePlatform();
void IOS_ApplyHardwareDefaults();
void IOS_UpdateHardware();
bool IOS_SupportsBaseVertex();
void IOS_ResetControls();
void IOS_AddTouchAxes(float *axes);
const char *IOS_MoltenVKPath();
#ifdef __OBJC__
@class UIWindowScene;
UIWindowScene *IOS_WindowScene();
#endif
