/* ACNH 本机离线原型启动器：双击 App 后设置已验证的启动环境并 exec 游戏。
   存档、着色器缓存和日志写到用户目录，App 包保持只读。不提交着色器贡献、不上架。 */
#import <AppKit/AppKit.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <unistd.h>
#include "game_version.h"

static void show_error(NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"游戏无法启动";
    alert.informativeText = detail;
    [alert runModal];
}

/* 启动前选择本次画面与语言设置，取消时不启动游戏。 */
static BOOL launch_options(void) {
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSAlert *dialog = [NSAlert new]; dialog.messageText = @"集合啦！动物森友会";
    dialog.informativeText = @"选择画面和语言后开始游戏。MetalFX 仅在输出画面需要放大且设备支持时生效。";
    [dialog addButtonWithTitle:@"开始游戏"]; [dialog addButtonWithTitle:@"取消"];
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 180)];
    NSArray *titles = @[@"显示", @"画面模式", @"语言", @"MetalFX 超分", @"居民"];
    NSArray *choices = @[@[@"窗口", @"全屏"], @[@"主机", @"掌机"], @[@"跟随系统", @"简体中文", @"繁體中文", @"English", @"日本語"], @[@"关闭", @"开启"], @[@"居民 1", @"居民 2（首次创建）"]];
    NSMutableArray *controls = [NSMutableArray array];
    for (NSUInteger i = 0; i < titles.count; i++) {
        CGFloat y = 146 - i * 34;
        NSTextField *label = [NSTextField labelWithString:titles[i]]; label.frame = NSMakeRect(0, y, 110, 24); [view addSubview:label];
        NSPopUpButton *menu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(120, y, 230, 26) pullsDown:NO];
        [menu addItemsWithTitles:choices[i]]; [view addSubview:menu]; [controls addObject:menu];
    }
    dialog.accessoryView = view; [NSApp activateIgnoringOtherApps:YES];
    if ([dialog runModal] != NSAlertFirstButtonReturn) return NO;
    setenv("ACNH_FULLSCREEN", [controls[0] indexOfSelectedItem] ? "1" : "0", 1);
    setenv("TOTK_OPERATION_MODE", [controls[1] indexOfSelectedItem] ? "handheld" : "docked", 1);
    int language = (int)[controls[2] indexOfSelectedItem];
    const char *locales[] = {NULL, "zh-Hans-CN", "zh-Hant-TW", "en-US", "ja-JP"};
    if (language) setenv("TOTK_LOCALE", locales[language], 1); else unsetenv("TOTK_LOCALE");
    setenv("ACNH_USER", [controls[4] indexOfSelectedItem] ? "1" : "0", 1);
    BOOL up = [controls[3] indexOfSelectedItem] != 0;
    setenv("TOTK_MFX_UPSCALE", up ? "1" : "0", 1); setenv("TOTK_MFX_OUTPUT", up ? "1" : "0", 1);
    return YES;
}

static NSDictionary<NSString *, NSString *> *kv_read(NSString *path) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [line substringToIndex:eq.location];
        if (!out[key]) out[key] = [line substringFromIndex:eq.location + 1];
    }
    return out;
}

static uint64_t pid_start(pid_t pid) {
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi || bi.pbi_status == SZOMB)
        return 0;
    return bi.pbi_start_tvsec;
}

static pid_t running_game(NSString *slot) {
    int fd = open([slot stringByAppendingPathComponent:@"lock"].fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    if (!held) return 0;
    NSDictionary *h = kv_read([slot stringByAppendingPathComponent:@"holder"]);
    const char *requestOwner = getenv("TOTK_SLOT_OWNER");
    if (![h[@"owner"] isEqualToString:@"user"] &&
        (!requestOwner || !*requestOwner || !strcmp(requestOwner, "user"))) return 0;
    pid_t pid = [h[@"game_pid"] intValue];
    return pid > 0 && pid_start(pid) == (uint64_t)[h[@"game_start"] longLongValue] ? pid : 0;
}

static BOOL make_dir(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
}

static int required_present(NSString *root, NSString *romfs) {
    NSArray<NSString *> *need = @[
        @"build/acnh-native-boot",
        @"rewritten/main.patched.bin",
        @"rewritten/sdk.patched.bin",
        @"rewritten/subsdk0.patched.bin",
        @"shared-fonts/0-standard.bfttf",
        @"ngc/ac_common_not_b_nx",
        @"build/shader-service/totk_xlate",
        @"build/spirv-cross/spirv-cross",
    ];
    for (NSString *rel in need) {
        NSString *path = [root stringByAppendingPathComponent:rel];
        if (access(path.fileSystemRepresentation, R_OK) != 0) return 0;
    }
    for (NSString *name in @[@"romfs.bin", @"runtime-index.bin"]) {
        if (access([romfs stringByAppendingPathComponent:name].fileSystemRepresentation, R_OK) != 0) return 0;
    }
    NSString *binary = [root stringByAppendingPathComponent:@"build/acnh-native-boot"];
    return access(binary.fileSystemRepresentation, X_OK) == 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"acnh"];
        NSString *binary = [root stringByAppendingPathComponent:@"build/acnh-native-boot"];
        NSString *data = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/" GAME_FOLDER];
        NSString *romfs = [data stringByAppendingPathComponent:@"romfs"];
        const char *override = getenv("ACNH_RESOURCE_DIR");
        if (override && *override) romfs = @(override);
        NSString *bundled = [root stringByAppendingPathComponent:@"romfs"];
        if (!override && access([bundled stringByAppendingPathComponent:@"romfs.bin"].fileSystemRepresentation, R_OK) == 0) romfs = bundled;
        if (argc > 1 && !strcmp(argv[1], "--check")) {
            NSString *declared = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            int ok = required_present(root, romfs) && [declared isEqualToString:@TOTK_GAME_VERSION];
            printf("%s: ACNH %s\n", ok ? "ok" : "missing files or version mismatch", TOTK_GAME_VERSION);
            return ok ? 0 : 1;
        }
        if (!required_present(root, romfs)) {
            make_dir(data);
            show_error(@"未找到游戏资源。请按照安装教程，将资源包内的 romfs 文件夹放到 ~/Library/Application Support/MacNativeGames/ACNH/ 中。");
            return 1;
        }
        NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        if (!launch_options()) return 0;
        NSString *(^env_or)(const char *, NSString *) = ^NSString *(const char *name, NSString *fallback) {
            const char *v = getenv(name);
            return v && *v ? @(v) : fallback;
        };
        NSString *profile = env_or("TOTK_PROFILE_DIR", [data stringByAppendingPathComponent:@"profile"]);
        NSString *slot = env_or("TOTK_GAME_SLOT_DIR", [data stringByAppendingPathComponent:@"game-slot"]);
        NSString *cache = env_or("TOTK_SHADER_CACHE", [data stringByAppendingPathComponent:@"shader-cache"]);
        if (!make_dir(profile) || !make_dir(slot) || !make_dir(cache) || !make_dir(logs)) {
            show_error([NSString stringWithFormat:@"无法创建 %@", data]);
            return 1;
        }
        pid_t running = running_game(slot);
        if (running) {
            [[NSRunningApplication runningApplicationWithProcessIdentifier:running]
                activateWithOptions:NSApplicationActivateAllWindows];
            return 0;
        }
        if (chdir(root.fileSystemRepresentation) != 0) {
            show_error(@"无法进入 App 内的游戏目录。");
            return 1;
        }
        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_CACHE", cache.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS", romfs.fileSystemRepresentation, 1);
        setenv("TOTK_SHARED_FONTS", [root stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation, 1);
        setenv("TOTK_NGC_DIR", [root stringByAppendingPathComponent:@"ngc"].fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_SEED_DIR", [root stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PATCHES", "0", 1);
        setenv("TOTK_ROMFS_MODS", "0", 1);
        setenv("TOTK_UNCAPPED", "0", 1);
        setenv("DM_PRESENT_HZ", "60", 1);
        setenv("DM_PRESENT_LINK", "0", 1);
        setenv("DM_PRESENT_INTERVAL", "game", 1);
        setenv("TOTK_MFX", "0", 1);
        setenv("TOTK_GPU_KEEPALIVE", "0", 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        setenv("TOTK_SLOT_OWNER", "user", 0);
        setenv("TOTK_SLOT_PURPOSE", "player", 0);
        unsetenv("TOTK_AUTOLOAD");
        NSString *preferred = NSLocale.preferredLanguages.firstObject;
        if (preferred && !getenv("TOTK_LOCALE")) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
        NSString *cmds = env_or("TOTK_INPUT_FILE", [data stringByAppendingPathComponent:@"input.txt"]);
        FILE *cf = fopen(cmds.fileSystemRepresentation, "w");
        if (cf) fclose(cf);
        setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1);

        NSString *log = [logs stringByAppendingPathComponent:@GAME_LOG ".log"];
        [NSFileManager.defaultManager removeItemAtPath:[logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"] error:nil];
        [NSFileManager.defaultManager moveItemAtPath:log
                                              toPath:[logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"]
                                               error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten", "boot", NULL};
        execv(arguments[0], arguments);
        fprintf(stderr, "[acnh-launcher] execv failed: %s\n", strerror(errno));
        show_error([NSString stringWithFormat:@"启动失败。日志：%@", log]);
        return 1;
    }
}
