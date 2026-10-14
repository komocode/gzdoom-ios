#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "ios_platform.h"
#include "c_dispatch.h"
#include "c_console.h"
#include "menu.h"
#include "d_eventbase.h"
#include "m_joy.h"
#include "i_interface.h"
#include "v_video.h"

extern bool GUICapture;
static float moveX, moveY;
static NSMutableSet *heldButtons;
static UIView *controlsView;
static bool backgrounded;
static __weak UIWindow *gameWindow;

static void ResizeGameWindow()
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
    controlsView.frame = root.bounds;
}

static void Key(SDL_Scancode code, bool down)
{
    SDL_Event e{};
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = code;
    e.key.keysym.sym = SDL_GetKeyFromScancode(code);
    SDL_PushEvent(&e);
}
static void Tap(SDL_Scancode code) { Key(code, true); Key(code, false); }

@interface GZActionButton : UIControl
@property(nonatomic, copy) NSString *action;
@property(nonatomic, copy) NSString *heldAction;
@property(nonatomic) SDL_Scancode menuKey;
@property(nonatomic) SDL_Scancode heldKey;
@end
@implementation GZActionButton
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    self.backgroundColor = [UIColor colorWithWhite:1 alpha:0.32];
    if (GUICapture || !self.action.length)
    {
        self.heldKey = self.menuKey;
        Key(self.heldKey, true);
    }
    else
    {
        self.heldAction = self.action;
        AddCommandString(self.heldAction.UTF8String);
    }
    [heldButtons addObject:self];
}
- (void)releaseAction
{
    self.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.45];
    if (self.heldKey) { Key(self.heldKey, false); self.heldKey = SDL_SCANCODE_UNKNOWN; }
    if ([self.heldAction hasPrefix:@"+"]) AddCommandString([[NSString stringWithFormat:@"-%@", [self.heldAction substringFromIndex:1]] UTF8String]);
    self.heldAction = nil;
    [heldButtons removeObject:self];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self releaseAction]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self releaseAction]; }
@end

@interface GZStick : UIView
@property(nonatomic) CGPoint origin;
@property(nonatomic, strong) UIView *thumb;
@property(nonatomic) SDL_Scancode direction;
@end
@implementation GZStick
- (instancetype)init
{
    if ((self = [super init]))
    {
        self.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.4];
        self.layer.borderWidth = 1;
        self.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
        _thumb = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 44, 44)];
        _thumb.backgroundColor = [UIColor colorWithWhite:1 alpha:0.35];
        _thumb.layer.cornerRadius = 22;
        _thumb.userInteractionEnabled = NO;
        [self addSubview:_thumb];
        self.accessibilityLabel = @"Movement stick and menu navigation";
    }
    return self;
}
- (void)layoutSubviews { [super layoutSubviews]; self.layer.cornerRadius = self.bounds.size.width / 2; self.thumb.center = CGPointMake(self.bounds.size.width / 2, self.bounds.size.height / 2); }
- (void)update:(UITouch *)touch
{
    CGPoint point = [touch locationInView:self];
    float radius = self.bounds.size.width * 0.35;
    float x = (point.x - self.bounds.size.width / 2) / radius;
    float y = (point.y - self.bounds.size.height / 2) / radius;
    float length = sqrtf(x*x + y*y);
    if (length > 1) { x /= length; y /= length; }
    if (length < 0.12) x = y = 0;
    moveX = x; moveY = y;
    self.thumb.center = CGPointMake(self.bounds.size.width/2 + x*radius, self.bounds.size.height/2 + y*radius);
    SDL_Scancode dir = SDL_SCANCODE_UNKNOWN;
    if (GUICapture && length > 0.4)
        dir = fabsf(x) > fabsf(y) ? (x < 0 ? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT) : (y < 0 ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN);
    if (dir != self.direction)
    {
        if (self.direction) Key(self.direction, false);
        self.direction = dir;
        if (dir) Key(dir, true);
    }
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self update:touches.anyObject]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self update:touches.anyObject]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    moveX = moveY = 0;
    if (self.direction) Key(self.direction, false);
    self.direction = SDL_SCANCODE_UNKNOWN;
    [self setNeedsLayout];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
@end

@interface GZControls : UIView
@property(nonatomic, strong) GZStick *stick;
@property(nonatomic, strong) NSArray<GZActionButton *> *buttons;
@property(nonatomic, weak) UITouch *lookTouch;
@property(nonatomic) CGPoint lookPoint;
@end
@implementation GZControls
- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        self.multipleTouchEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        _stick = [GZStick new];
        [self addSubview:_stick];
        NSMutableArray *buttons = [NSMutableArray array];
        NSArray *titles = @[@"Fire / OK", @"Use", @"Jump", @"Weapon", @"Menu", @"Save"];
        NSArray *actions = @[@"+attack", @"+use", @"+jump", @"weapnext", @"", @"quicksave"];
        SDL_Scancode keys[] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RIGHT, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F6};
        for (int i = 0; i < 6; ++i)
        {
            GZActionButton *button = [GZActionButton new];
            button.action = actions[i]; button.menuKey = keys[i];
            button.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.45];
            button.layer.borderWidth = 1; button.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.4].CGColor;
            UILabel *label = [UILabel new]; label.text = titles[i]; label.textColor = UIColor.whiteColor;
            label.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
            label.textAlignment = NSTextAlignmentCenter; label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            label.userInteractionEnabled = NO;
            [button addSubview:label];
            button.accessibilityLabel = titles[i];
            [self addSubview:button]; [buttons addObject:button];
        }
        _buttons = buttons;
    }
    return self;
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    CGRect safe = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGFloat stickSize = MIN(146, safe.size.height * 0.38);
    self.stick.frame = CGRectMake(safe.origin.x + 24, CGRectGetMaxY(safe) - stickSize - 20, stickSize, stickSize);
    CGFloat size = MIN(72, safe.size.height * 0.21);
    for (NSUInteger i = 0; i < self.buttons.count; ++i)
    {
        GZActionButton *b = self.buttons[i];
        if (i < 4) b.frame = CGRectMake(CGRectGetMaxX(safe) - (i % 2 + 1)*(size+14) - 12, CGRectGetMaxY(safe) - (i / 2 + 1)*(size+14) - 12, size, size);
        else b.frame = CGRectMake(safe.origin.x + (i-4)*86 + 16, safe.origin.y + 12, 78, 40);
        b.layer.cornerRadius = i < 4 ? size / 2 : 12;
        b.subviews.firstObject.frame = b.bounds;
    }
}
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
    UIView *hit = [super hitTest:point withEvent:event];
    if (hit != self) return hit;
    return point.x > self.bounds.size.width * 0.4 ? self : nil;
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    if (!self.lookTouch) { self.lookTouch = touches.anyObject; self.lookPoint = [self.lookTouch locationInView:self]; }
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    if (![touches containsObject:self.lookTouch]) return;
    CGPoint p = [self.lookTouch locationInView:self];
    if (!GUICapture)
    {
        event_t e{}; e.type = EV_Mouse;
        e.x = (int)((p.x - self.lookPoint.x) * 3); e.y = (int)((self.lookPoint.y - p.y) * 3);
        D_PostEvent(&e);
    }
    self.lookPoint = p;
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { if ([touches containsObject:self.lookTouch]) self.lookTouch = nil; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
@end

void IOS_ResetControls()
{
    SDL_StopTextInput();
    moveX = moveY = 0;
    for (GZActionButton *b in heldButtons.allObjects) [b releaseAction];
    GZControls *view = (GZControls *)controlsView;
    [view.stick touchesEnded:[NSSet set] withEvent:nil];
    view.lookTouch = nil;
}
void IOS_AttachControls(SDL_Window *window)
{
    SDL_SysWMinfo wm{}; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) return;
    UIWindow *uiwindow = wm.info.uikit.window;
    gameWindow = uiwindow;
    uiwindow.windowScene = IOS_WindowScene();
    [uiwindow makeKeyAndVisible];
    [controlsView removeFromSuperview];
    heldButtons = [NSMutableSet set];
    controlsView = [[GZControls alloc] initWithFrame:uiwindow.rootViewController.view.bounds];
    [uiwindow.rootViewController.view addSubview:controlsView];
    ResizeGameWindow();
    static bool observing;
    if (!observing)
    {
        observing = true;
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            IOS_ResetControls(); backgrounded = true;
            if (sysCallbacks.WantGuiCapture && !sysCallbacks.WantGuiCapture()) Tap(SDL_SCANCODE_ESCAPE);
        }];
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { backgrounded = false; }];
    }
}
void IOS_AddTouchAxes(float *axes)
{
    if (GUICapture || backgrounded) return;
    axes[moveX > 0 ? AXIS_CODE_PAD_LTHUMB_RIGHT : AXIS_CODE_PAD_LTHUMB_LEFT] += fabsf(moveX);
    axes[moveY > 0 ? AXIS_CODE_PAD_LTHUMB_DOWN : AXIS_CODE_PAD_LTHUMB_UP] += fabsf(moveY);
}
void IOS_UpdatePlatform()
{
    // Show the system keyboard only for a save/settings text entry or console.
    auto textMenu = PClass::FindClass("TextEnterMenu");
    bool wantsKeyboard = !backgrounded && (ConsoleState == c_down || ConsoleState == c_falling ||
        (CurrentMenu && textMenu && CurrentMenu->IsKindOf(textMenu)));
    if (wantsKeyboard != bool(SDL_IsTextInputActive()))
    {
        if (wantsKeyboard) SDL_StartTextInput();
        else SDL_StopTextInput();
    }
    ResizeGameWindow();
    static Uint32 nextHardwareUpdate;
    Uint32 now = SDL_GetTicks();
    if (now >= nextHardwareUpdate) { IOS_UpdateHardware(); nextHardwareUpdate = now + 1000; }
    // Let the OS suspend and resume the app without busy-rendering in background.
    while (backgrounded) { SDL_PumpEvents(); SDL_Delay(30); }
}
