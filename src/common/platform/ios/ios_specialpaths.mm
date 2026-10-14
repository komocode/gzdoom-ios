#import <Foundation/Foundation.h>
#include "cmdlib.h"
#include "i_specialpaths.h"

static FString Path(NSSearchPathDirectory directory, NSString *suffix, bool create = true)
{
#ifdef GZDOOM_TVOS
    // tvOS provides purgeable app storage; it has no user Documents directory.
    directory = NSCachesDirectory;
    suffix = [@"GZDoom" stringByAppendingPathComponent:suffix];
#endif
    NSURL *base = [[NSFileManager defaultManager] URLsForDirectory:directory inDomains:NSUserDomainMask].firstObject;
    NSString *path = [base.path stringByAppendingPathComponent:suffix];
    if (create) [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
    return path.UTF8String;
}
FString M_GetAppDataPath(bool create) { return Path(NSApplicationSupportDirectory, @"GZDoom", create); }
FString M_GetCachePath(bool create) { return Path(NSCachesDirectory, @"GZDoom", create); }
FString M_GetAutoexecPath() { return Path(NSDocumentDirectory, @"") + "/autoexec.cfg"; }
FString M_GetConfigPath(bool reading) { return Path(NSApplicationSupportDirectory, @"GZDoom") + "/gzdoom.ini"; }
FString M_GetScreenshotsPath() { return Path(NSDocumentDirectory, @"Screenshots") + "/"; }
FString M_GetSavegamesPath() { return Path(NSDocumentDirectory, @"Savegames") + "/"; }
FString M_GetDocumentsPath() { return Path(NSDocumentDirectory, @"") + "/"; }
FString M_GetDemoPath() { return Path(NSDocumentDirectory, @"Demos") + "/"; }
FString M_GetMacAppSupportPath(bool create) { return M_GetAppDataPath(create); }
void M_GetMacSearchDirectories(FString &docs, FString &user, FString &local)
{
    docs = Path(NSDocumentDirectory, @"Games");
    user = docs;
    local = progdir + "Games";
}
FString M_GetNormalizedPath(const char *path)
{
    return [NSString stringWithUTF8String:path].stringByStandardizingPath.UTF8String;
}
