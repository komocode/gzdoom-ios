#import <UIKit/UIKit.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "ios_platform.h"
#include "i_interface.h"
#include "c_console.h"
#include "menu.h"

static __weak UIWindow *gameWindow;
static bool backgrounded;

static void ResizeWindow()
{
    UIWindowScene *scene = IOS_WindowScene();
    if (!gameWindow || !scene) return;
    CGRect frame = scene.coordinateSpace.bounds;
    if (!CGRectEqualToRect(gameWindow.frame, frame)) gameWindow.frame = frame;
    UIView *root = gameWindow.rootViewController.view;
    if (!CGRectEqualToRect(root.frame, gameWindow.bounds))
    {
        root.frame = gameWindow.bounds;
        [root setNeedsLayout];
        [root layoutIfNeeded];
    }
}

void IOS_ResetControls() { SDL_StopTextInput(); }
void IOS_AddTouchAxes(float *) { }
void IOS_AttachControls(SDL_Window *window)
{
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) return;
    gameWindow = wm.info.uikit.window;
    gameWindow.windowScene = IOS_WindowScene();
    [gameWindow makeKeyAndVisible];
    ResizeWindow();
    static bool observing;
    if (observing) return;
    observing = true;
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
        IOS_ResetControls();
        backgrounded = true;
        if (sysCallbacks.WantGuiCapture && !sysCallbacks.WantGuiCapture())
        {
            SDL_Event e{};
            e.type = SDL_KEYDOWN;
            e.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
            e.key.keysym.sym = SDLK_ESCAPE;
            SDL_PushEvent(&e);
            e.type = SDL_KEYUP;
            SDL_PushEvent(&e);
        }
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) { backgrounded = false; }];
}

void IOS_UpdatePlatform()
{
    auto textMenu = PClass::FindClass("TextEnterMenu");
    bool wantsKeyboard = !backgrounded && (ConsoleState == c_down || ConsoleState == c_falling ||
        (CurrentMenu && textMenu && CurrentMenu->IsKindOf(textMenu)));
    if (wantsKeyboard != bool(SDL_IsTextInputActive()))
    {
        if (wantsKeyboard) SDL_StartTextInput();
        else SDL_StopTextInput();
    }
    ResizeWindow();
    static Uint32 nextUpdate;
    Uint32 now = SDL_GetTicks();
    if (now >= nextUpdate) { IOS_UpdateHardware(); nextUpdate = now + 1000; }
    while (backgrounded) { SDL_PumpEvents(); SDL_Delay(30); }
}
