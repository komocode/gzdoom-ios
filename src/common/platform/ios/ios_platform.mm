#import <UIKit/UIKit.h>
#ifndef GZDOOM_TVOS
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif
#import <AVFoundation/AVFoundation.h>
#include <SDL2/SDL.h>
#include <clocale>
#include <vector>
#include <string>
#include "ios_platform.h"
#include "cmdlib.h"
#include "m_argv.h"
#include "i_system.h"

FString sys_ostype;
FArgs *Args;
extern int GameMain();
extern void I_StartupJoysticks();
static UIWindow *libraryWindow;
static NSString *selectedGame;
static UIWindowScene *gameScene;
UIWindowScene *IOS_WindowScene() { return gameScene; }
static UIColor *LibraryBackground()
{
#ifdef GZDOOM_TVOS
    return UIColor.blackColor;
#else
    return UIColor.systemBackgroundColor;
#endif
}

@interface GZDoomSceneDelegate : NSObject <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation GZDoomSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    gameScene = (UIWindowScene *)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:gameScene];
    self.window.rootViewController = [UIViewController new];
    self.window.rootViewController.view.backgroundColor = LibraryBackground();
    [self.window makeKeyAndVisible];
#ifndef GZDOOM_TVOS
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)
    {
        UIWindowSceneGeometryPreferencesIOS *orientation = [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
        [gameScene requestGeometryUpdateWithPreferences:orientation errorHandler:^(NSError *error) { NSLog(@"Orientation: %@", error.localizedDescription); }];
    }
#endif
}
@end

static NSString *GamesDirectory()
{
#ifdef GZDOOM_TVOS
    NSURL *docs = [[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
#else
    NSURL *docs = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
#endif
    NSString *path = [docs.path stringByAppendingPathComponent:@"Games"];
    [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
    return path;
}

static BOOL IsIWAD(NSString *path)
{
    NSFileHandle *file = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!file) return NO;
    NSData *header = [file readDataOfLength:12];
    [file closeFile];
    if (header.length != 12 || memcmp(header.bytes, "IWAD", 4)) return NO;
    const uint8_t *p = (const uint8_t *)header.bytes;
    auto read32 = [](const uint8_t *b) -> uint32_t { return b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24); };
    uint64_t count = read32(p + 4), offset = read32(p + 8);
    uint64_t size = [[[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil] fileSize];
    return count > 0 && offset >= 12 && offset <= size && count <= (size - offset) / 16;
}

@interface GZGameLibrary : UITableViewController
#ifndef GZDOOM_TVOS
<UIDocumentPickerDelegate>
#endif
@property(nonatomic, strong) NSArray<NSString *> *games;
@end
@implementation GZGameLibrary
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"GZDoom";
#ifndef GZDOOM_TVOS
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Import WAD" style:UIBarButtonItemStylePlain target:self action:@selector(importGame)];
#endif
    self.tableView.backgroundColor = LibraryBackground();
    UILabel *intro = [[UILabel alloc] initWithFrame:CGRectMake(24, 0, 600, 100)];
#ifdef GZDOOM_TVOS
    intro.text = @"Choose a game\nUse a game controller to move, look, fire, and open the menu.";
#else
    intro.text = @"Choose a game\nMove with the left stick, drag to look, and tap Fire or Use. Controllers and keyboards are supported.";
#endif
    intro.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    intro.numberOfLines = 0;
    intro.textAlignment = NSTextAlignmentCenter;
    self.tableView.tableHeaderView = intro;
    [self reloadGames];
}
- (void)reloadGames
{
    NSMutableArray *games = [NSMutableArray array];
    for (NSString *folder in @[GamesDirectory(), [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Games"]])
        for (NSString *file in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil])
        {
            NSString *path = [folder stringByAppendingPathComponent:file];
            if ([file.pathExtension.lowercaseString isEqualToString:@"wad"] && IsIWAD(path)) [games addObject:path];
        }
    self.games = [games sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) { return [a.lastPathComponent localizedCaseInsensitiveCompare:b.lastPathComponent]; }];
    [self.tableView reloadData];
    UILabel *empty = [[UILabel alloc] initWithFrame:self.tableView.bounds];
#ifdef GZDOOM_TVOS
    empty.text = @"No games are installed. Add game assets to the app before building.";
#else
    empty.text = @"Import an IWAD to start playing.\nYou can also copy WADs into Games using Files.";
#endif
    empty.numberOfLines = 0;
    empty.textAlignment = NSTextAlignmentCenter;
    self.tableView.backgroundView = self.games.count ? nil : empty;
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section { return self.games.count; }
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)index
{
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSString *path = self.games[index.row];
    cell.textLabel.text = path.lastPathComponent.stringByDeletingPathExtension;
    cell.detailTextLabel.text = @"Play game";
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)index
{
    selectedGame = self.games[index.row];
    [NSUserDefaults.standardUserDefaults setObject:selectedGame.lastPathComponent forKey:@"LastGame"];
}
#ifndef GZDOOM_TVOS
- (void)importGame
{
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.delegate = self;
    picker.allowsMultipleSelection = YES;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    for (NSURL *url in urls)
    {
        BOOL scoped = [url startAccessingSecurityScopedResource];
        if (IsIWAD(url.path))
        {
            NSString *destination = [GamesDirectory() stringByAppendingPathComponent:url.lastPathComponent];
            // Keep an existing game and its saves; import another copy with a unique name.
            if ([[NSFileManager defaultManager] fileExistsAtPath:destination])
                destination = [GamesDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.wad", url.lastPathComponent.stringByDeletingPathExtension, NSUUID.UUID.UUIDString]];
            NSError *error;
            if (![[NSFileManager defaultManager] copyItemAtPath:url.path toPath:destination error:&error])
                [self showError:error.localizedDescription];
        }
        else [self showError:@"Choose an IWAD game archive. This file is not a valid IWAD."];
        if (scoped) [url stopAccessingSecurityScopedResource];
    }
    [self reloadGames];
}
- (void)showError:(NSString *)message
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Import game" message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
#endif
@end

void I_DetectOS()
{
    sys_ostype = [[NSString stringWithFormat:@"%@ %@", UIDevice.currentDevice.systemName, UIDevice.currentDevice.systemVersion] UTF8String];
    printf("OS: %s\n", sys_ostype.GetChars());
}
const char *IOS_MoltenVKPath()
{
    static std::string path = std::string(NSBundle.mainBundle.bundlePath.UTF8String) + "/Frameworks/MoltenVK.framework/MoltenVK";
    return path.c_str();
}
void Mac_I_FatalError(const char *error)
{
    NSLog(@"GZDoom error: %s", error);
    IOS_ResetControls();
    UIViewController *vc = UIApplication.sharedApplication.keyWindow.rootViewController;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Unable to start game" message:[NSString stringWithUTF8String:error] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [vc presentViewController:alert animated:YES completion:nil];
    // Fatal engine errors cannot safely resume the game loop. Keep the error readable.
    for (;;) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}

extern "C" int SDL_main(int argc, char **argv)
{
    @autoreleasepool
    {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        setlocale(LC_ALL, "C");
        setenv("LC_NUMERIC", "C", 1);
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
        SDL_SetHint(SDL_HINT_IDLE_TIMER_DISABLED, "1");
#ifdef GZDOOM_TVOS
        SDL_SetHint(SDL_HINT_APPLE_TV_REMOTE_ALLOW_ROTATION, "0");
#else
        if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)
            SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
        // SDL otherwise chooses GLES automatically on UIKit. Vulkan is selected below.
        if (SDL_Init(0) < 0) return 1;
        while (!gameScene) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        BOOL autostart = NO, suppliedIWAD = NO;
        for (int i = 1; i < argc; ++i)
        {
            if (!strcmp(argv[i], "-ios-autostart")) autostart = YES;
            if (!strcmp(argv[i], "-iwad")) suppliedIWAD = YES;
        }
        GZGameLibrary *library = [GZGameLibrary new];
        if (!suppliedIWAD)
        {
            libraryWindow = [[UIWindow alloc] initWithWindowScene:gameScene];
            libraryWindow.rootViewController = [[UINavigationController alloc] initWithRootViewController:library];
            [libraryWindow makeKeyAndVisible];
            [library loadViewIfNeeded];
            if (autostart && library.games.count) selectedGame = library.games.firstObject;
            while (!selectedGame) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
        std::vector<std::string> arguments;
        for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
        if (selectedGame) { arguments.emplace_back("-iwad"); arguments.emplace_back(selectedGame.UTF8String); }
        arguments.emplace_back("+queryiwad"); arguments.emplace_back("false");
        arguments.emplace_back("+freelook"); arguments.emplace_back("true");
        arguments.emplace_back("+vid_fullscreen"); arguments.emplace_back("true");
        arguments.emplace_back("+logfile");
        arguments.emplace_back([[[GamesDirectory() stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"engine.log"] UTF8String]);
        std::vector<char *> raw;
        for (auto &s : arguments) raw.push_back(s.data());
        raw.push_back(nullptr);
        Args = new FArgs((int)arguments.size(), raw.data());
        progdir = NSBundle.mainBundle.bundlePath.UTF8String;
        progdir += "/";
        [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:nil];
        [[AVAudioSession sharedInstance] setActive:YES error:nil];
        I_StartupJoysticks();
        libraryWindow.hidden = YES;
        libraryWindow = nil;
#ifdef GZDOOM_TVOS
        // Native focus belongs to the library; the SDL view handles controller
        // input after the game has taken over its window.
        SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, "0");
#endif
        const int result = GameMain();
        SDL_Quit();
        return result;
    }
}
