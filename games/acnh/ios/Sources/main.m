#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "runtime.h"
#include "dmsh_chain.h"
#import "touch_pad.h"

static NSString *const kACNHDockedModeKey = @"ACNHDockedMode";
static NSString *ACNHRomfsPath(void) {
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData/romfs"];
    if ([NSFileManager.defaultManager fileExistsAtPath:[bundled stringByAppendingPathComponent:@"romfs.bin"]]) return bundled;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [docs stringByAppendingPathComponent:@"ACNHResources/romfs"];
}
static BOOL ACNHRomfsAvailable(void) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *path = ACNHRomfsPath();
    for (NSString *name in @[@"romfs.bin", @"runtime-index.bin"]) {
        NSDictionary *attrs = [fm attributesOfItemAtPath:[path stringByAppendingPathComponent:name] error:nil];
        if (![attrs[NSFileType] isEqualToString:NSFileTypeRegular] || [attrs[NSFileSize] unsignedLongLongValue] == 0) return NO;
    }
    return YES;
}
void runtime_ios_install_boot_overlay(UIView *root);

int acnh_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
void runtime_ios_set_active(int active);
/* 温控状态：0 正常、1 轻度、2 严重、3 紧急；这是系统热状态枚举，不是摄氏度。 */
int dm_thermal_now(void) { return (int)NSProcessInfo.processInfo.thermalState; }
static char *host_argv[3];
static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)acnh_host_main(3, host_argv); }
/* The desktop keyboard-overlay shortcut has no counterpart on iOS. */
void runtime_shortcut_help_toggle(void) {}

@interface TOTKMetalView : UIView @end
@implementation TOTKMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
- (void)syncDrawable {
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    CGFloat scale = self.window.screen.scale;
    if (scale < 1) scale = UIScreen.mainScreen.nativeScale;
    if (scale < 1) scale = 1;
    if (!layer.device) layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.contentsScale = scale;
    CGSize px = CGSizeMake(round(self.bounds.size.width * scale), round(self.bounds.size.height * scale));
    if (px.width < 1 || px.height < 1 || CGSizeEqualToSize(layer.drawableSize, px)) return;
    layer.drawableSize = px;
    fprintf(stderr, "[totk] window drawable %.0fx%.0f (scale %.0f)\n", px.width, px.height, scale);
}
- (void)didMoveToWindow { [super didMoveToWindow]; [self syncDrawable]; }
- (void)layoutSubviews { [super layoutSubviews]; [self syncDrawable]; }
@end

@interface TOTKGameController : UIViewController @property(nonatomic,strong) TOTKMetalView *gameView; @end
@implementation TOTKGameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[TOTKMetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    TOTKTouchPad *pad=[[TOTKTouchPad alloc] initWithFrame:self.view.bounds];pad.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:pad];
    runtime_ios_install_boot_overlay(self.view);
    [self.gameView syncDrawable];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    /* Playback category (PR #97): plays with the ring / silent switch off and routes to the speaker. Without an active
       session the game's CoreAudio output stays on a null sink on iOS 27 ("no device sink in use"); activating it in
       didFinishLaunching blocked the main thread before the log file was opened. Interruptions: audio_observe. */
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    NSError *catErr = nil, *actErr = nil;
    [audio setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&catErr];
    BOOL activated = [audio setActive:YES error:&actErr];
    fprintf(stderr, "[totk-audio] audio session category=%s active=%d route=%s err=%s%s\n",
            audio.category.UTF8String ?: "?", (int)activated, audio.currentRoute.outputs.firstObject.portType.UTF8String ?: "?",
            catErr ? catErr.localizedDescription.UTF8String : "none", actErr ? actErr.localizedDescription.UTF8String : "");
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread; pthread_create(&thread, &attr, run_host, NULL);
    pthread_detach(thread); pthread_attr_destroy(&attr);
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

/* 用轻淡的三角草纹呼应游戏画面，装饰层不参与触摸。 */
@interface ACNHLaunchBackground : UIView @end
@implementation ACNHLaunchBackground
- (void)drawRect:(CGRect)rect {
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    NSArray *colors = @[(id)[UIColor colorWithRed:.77 green:.89 blue:.79 alpha:1].CGColor,
                        (id)[UIColor colorWithRed:.94 green:.94 blue:.80 alpha:1].CGColor,
                        (id)[UIColor colorWithRed:.83 green:.91 blue:.75 alpha:1].CGColor];
    CGGradientRef gradient = CGGradientCreateWithColors(space, (__bridge CFArrayRef)colors, NULL);
    CGContextDrawLinearGradient(c, gradient, CGPointZero, CGPointMake(CGRectGetMaxX(self.bounds), CGRectGetMaxY(self.bounds)), 0);
    CGGradientRelease(gradient); CGColorSpaceRelease(space);
    CGContextSetFillColorWithColor(c, [UIColor colorWithRed:0.54 green:0.69 blue:0.38 alpha:0.12].CGColor);
    for (CGFloat y = 12; y < self.bounds.size.height; y += 38) {
        for (CGFloat x = 15 + ((int)(y / 38) % 2) * 24; x < self.bounds.size.width; x += 48) {
            CGContextMoveToPoint(c, x, y); CGContextAddLineToPoint(c, x + 10, y + 4);
            CGContextAddLineToPoint(c, x + 3, y + 11); CGContextClosePath(c); CGContextFillPath(c);
        }
    }
}
@end

@interface ACNHLaunchController : UIViewController
@property(nonatomic,strong) UISegmentedControl *mode;
@end
@implementation ACNHLaunchController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.91 green:0.94 blue:0.78 alpha:1];
    ACNHLaunchBackground *background = [[ACNHLaunchBackground alloc] initWithFrame:self.view.bounds];
    background.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    background.userInteractionEnabled = NO;
    [self.view addSubview:background];
    UILabel *title = [UILabel new];
    title.text = @"集合啦！动物森友会";
    title.font = [UIFont systemFontOfSize:28 weight:UIFontWeightBold];
    title.textColor = [UIColor colorWithRed:0.25 green:0.30 blue:0.20 alpha:1];
    title.textAlignment = NSTextAlignmentCenter;
    UILabel *hint = [UILabel new];
    hint.text = @"先选画面再开始。主机模式是游戏自己的 1920×1080，更清晰，发热也会更高。";
    hint.font = [UIFont systemFontOfSize:15];
    hint.textColor = [UIColor colorWithRed:0.35 green:0.38 blue:0.29 alpha:1];
    hint.textAlignment = NSTextAlignmentCenter;
    hint.numberOfLines = 0;
    self.mode = [[UISegmentedControl alloc] initWithItems:@[@"主机 1920×1080", @"掌机 1280×720"]];
    BOOL docked = YES;
    if ([NSUserDefaults.standardUserDefaults objectForKey:kACNHDockedModeKey])
        docked = [NSUserDefaults.standardUserDefaults boolForKey:kACNHDockedModeKey];
    self.mode.selectedSegmentIndex = docked ? 0 : 1;
    self.mode.backgroundColor = [UIColor colorWithRed:0.42 green:0.57 blue:0.42 alpha:0.18];
    self.mode.layer.cornerRadius = 12;
    self.mode.layer.borderWidth = 1;
    self.mode.layer.borderColor = [UIColor colorWithRed:1 green:1 blue:.94 alpha:.65].CGColor;
    self.mode.selectedSegmentTintColor = [UIColor colorWithRed:0.99 green:0.99 blue:0.93 alpha:0.88];
    NSDictionary *modeText = @{NSForegroundColorAttributeName: [UIColor colorWithRed:0.25 green:0.30 blue:0.20 alpha:1], NSFontAttributeName: [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold]};
    [self.mode setTitleTextAttributes:modeText forState:UIControlStateNormal];
    [self.mode setTitleTextAttributes:modeText forState:UIControlStateSelected];
    UIButton *start = [UIButton buttonWithType:UIButtonTypeSystem];
    [start setTitle:@"开始游戏" forState:UIControlStateNormal];
    start.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
    [start setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    start.backgroundColor = [UIColor colorWithRed:0.16 green:0.44 blue:0.32 alpha:0.90];
    start.layer.cornerRadius = 26;
    start.layer.borderWidth = 1.5;
    start.layer.borderColor = [UIColor colorWithRed:.91 green:.96 blue:.77 alpha:.75].CGColor;
    start.layer.shadowColor = [UIColor colorWithRed:.15 green:.37 blue:.26 alpha:1].CGColor;
    start.layer.shadowOpacity = .20; start.layer.shadowRadius = 7; start.layer.shadowOffset = CGSizeMake(0, 3);
    [start addTarget:self action:@selector(startGame) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, hint, self.mode, start]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentFill;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *card = [UIView new];
    card.backgroundColor = UIColor.clearColor;
    card.layer.cornerRadius = 28;
    card.layer.shadowColor = [UIColor colorWithRed:0.30 green:0.38 blue:0.19 alpha:1].CGColor;
    card.layer.shadowOpacity = 0.14; card.layer.shadowRadius = 14; card.layer.shadowOffset = CGSizeMake(0, 5);
    card.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:card];
    UIVisualEffectView *glass = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialLight]];
    glass.translatesAutoresizingMaskIntoConstraints = NO;
    glass.layer.cornerRadius = 28; glass.layer.masksToBounds = YES;
    glass.layer.borderWidth = 1.5;
    glass.layer.borderColor = [UIColor colorWithRed:1 green:1 blue:.95 alpha:.75].CGColor;
    [card addSubview:glass];
    [glass.contentView addSubview:stack];
    NSLayoutConstraint *preferredWidth = [card.widthAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor multiplier:0.80];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [card.centerXAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerXAnchor],
        [card.centerYAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerYAnchor],
        preferredWidth,
        [card.widthAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor constant:-24],
        [glass.leadingAnchor constraintEqualToAnchor:card.leadingAnchor],
        [glass.trailingAnchor constraintEqualToAnchor:card.trailingAnchor],
        [glass.topAnchor constraintEqualToAnchor:card.topAnchor],
        [glass.bottomAnchor constraintEqualToAnchor:card.bottomAnchor],
        [card.widthAnchor constraintLessThanOrEqualToConstant:680],
        [stack.leadingAnchor constraintEqualToAnchor:glass.contentView.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:glass.contentView.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:glass.contentView.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:glass.contentView.bottomAnchor constant:-20],
        [start.heightAnchor constraintEqualToConstant:52],
        [self.mode.heightAnchor constraintEqualToConstant:44],
    ]];
}
- (void)startGame {
    if (!ACNHRomfsAvailable()) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"请先导入游戏资源" message:@"请将资源包中的 ACNHResources 文件夹放入“文件”App 中本游戏的文件夹，然后重新点击开始游戏。" preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"知道了" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil]; return;
    }
    BOOL docked = self.mode.selectedSegmentIndex == 0;
    [NSUserDefaults.standardUserDefaults setBool:docked forKey:kACNHDockedModeKey];
    setenv("TOTK_OPERATION_MODE", docked ? "docked" : "handheld", 1);
    self.view.window.rootViewController = [TOTKGameController new];
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

/* iOS 27 ends apps linked against its SDK that keep the window in the app delegate (UIKit traps in
   _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption at launch), so the window belongs to a window scene. */
@interface ACNHSceneDelegate : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation ACNHSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [ACNHLaunchController new]; [self.window makeKeyAndVisible];
}
@end

/* Game audio after interruptions (from nxnative b301431). A call, Siri or an alarm takes the audio session and the
   system stops the game's output unit; a media services reset invalidates it. Nothing started it again, so the game
   stayed silent. When an interruption ends the session is made active again and the output started
   (runtime_audio_output_restart); a reset rebuilds the output. Becoming active does the same, for an interruption
   whose "ended" never came. Becoming active also turns the idle timer off again. */
static BOOL g_audio_interrupted;
static UIBackgroundTaskIdentifier g_save_background_task;
static uint64_t g_save_background_task_epoch;
static uint64_t g_save_background_generation;

static void wait_for_save_before_suspend(void) {
    UIApplication *app = UIApplication.sharedApplication;
    if (g_save_background_task == UIBackgroundTaskInvalid) {
        uint64_t taskEpoch = ++g_save_background_task_epoch;
        __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
        task = [app beginBackgroundTaskWithName:@"ACNH save commit" expirationHandler:^{
            fprintf(stderr, "[acnh-lifecycle] background time expired; save_busy=%d\n", runtime_fs_save_busy());
            dispatch_async(dispatch_get_main_queue(), ^{
                if (g_save_background_task == task && g_save_background_task_epoch == taskEpoch) {
                    [app endBackgroundTask:task];
                    g_save_background_task = UIBackgroundTaskInvalid;
                }
            });
        }];
        g_save_background_task = task;
        if (g_save_background_task == UIBackgroundTaskInvalid) {
            fprintf(stderr, "[acnh-lifecycle] could not begin background task; save_busy=%d\n", runtime_fs_save_busy());
            return;
        }
    }
    uint64_t generation = ++g_save_background_generation;
    UIBackgroundTaskIdentifier task = g_save_background_task;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSTimeInterval deadline = [NSDate timeIntervalSinceReferenceDate] + 15.0;
        while (runtime_front_focus() && [NSDate timeIntervalSinceReferenceDate] < deadline) usleep(20 * 1000);
        NSTimeInterval settleUntil = [NSDate timeIntervalSinceReferenceDate] + 2.0;
        while (([NSDate timeIntervalSinceReferenceDate] < settleUntil || runtime_fs_save_busy()) &&
               [NSDate timeIntervalSinceReferenceDate] < deadline) usleep(20 * 1000);
        BOOL busy = runtime_fs_save_busy();
        BOOL focusLost = !runtime_front_focus();
        fprintf(stderr, "[acnh-lifecycle] background save wait %s focus_lost=%d\n",
                busy || !focusLost ? "timed out" : "idle", focusLost);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_save_background_task == task && g_save_background_generation == generation) {
                [app endBackgroundTask:task];
                g_save_background_task = UIBackgroundTaskInvalid;
            }
        });
    });
}

static void audio_resume(int recreate, const char *why) {
    NSError *err = nil;
    if (![AVAudioSession.sharedInstance setActive:YES error:&err]) fprintf(stderr,"[totk-audio] audio session not active again (%s): %s\n",why,err.localizedDescription.UTF8String);
    g_audio_interrupted = NO; runtime_audio_output_restart(recreate);
}
static void audio_observe(void) {
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter; NSOperationQueue *main = NSOperationQueue.mainQueue;
    [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; runtime_ios_set_active(0); wait_for_save_before_suspend();
    }];
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        fprintf(stderr,"[totk-audio] audio session interruption %s\n",type == AVAudioSessionInterruptionTypeBegan ? "began" : "ended");
        if (type == AVAudioSessionInterruptionTypeBegan) g_audio_interrupted = YES; else audio_resume(0,"interruption ended");
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; fprintf(stderr,"[totk-audio] media services were reset\n"); audio_resume(1,"media services reset");
    }];
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; runtime_ios_set_active(1); UIApplication.sharedApplication.idleTimerDisabled = YES;
        if (g_audio_interrupted) audio_resume(0,"became active during an interruption");
    }];
}

@interface TOTKApp : UIResponder <UIApplicationDelegate> @end
@implementation TOTKApp
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = ACNHSceneDelegate.class; return c;
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)options;
    // Controller input does not reset the idle timer; auto-lock would suspend the game mid-play or mid shader import.
    application.idleTimerDisabled = YES;
    g_save_background_task = UIBackgroundTaskInvalid;
    runtime_ios_set_active(1);
    audio_observe();
    /* Keep the preferred locale for the shared runtime's language selection. */
    NSString *preferred = NSLocale.preferredLanguages.firstObject;
    if (preferred.length) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSURL *supportURL = [fm URLForDirectory:NSApplicationSupportDirectory inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
    NSString *data = [supportURL.path stringByAppendingPathComponent:@"ACNH"];
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData"];
    if (!supportURL || ![fm createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil]) {
        fprintf(stderr, "[acnh] cannot create private Application Support directory\n"); return NO;
    }
    for (NSString *name in @[@"save", @"temp", @"shader-cache", @"game-slot"]) [fm createDirectoryAtPath:[data stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *log = [docs stringByAppendingPathComponent:@"log.txt"];
    if (freopen(log.fileSystemRepresentation,"w",stderr)) setvbuf(stderr,NULL,_IOLBF,0);
    NSString *ngc = [res stringByAppendingPathComponent:@"ngc"];
    if (![fm fileExistsAtPath:[ngc stringByAppendingPathComponent:@"ac_common_not_b_nx"]]) {
        fprintf(stderr, "[acnh] missing bundled NgWord2 common table\n"); return NO;
    }
    setenv("TOTK_ROMFS",ACNHRomfsPath().fileSystemRepresentation,1);
    setenv("TOTK_NGC_DIR",ngc.fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PATCHES",[res stringByAppendingPathComponent:@"romfs-patches"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PATCHES_OUT",[res stringByAppendingPathComponent:@"romfs-patches-out"].fileSystemRepresentation,1);
    setenv("TOTK_SHARED_FONTS",[res stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation,1);
    setenv("TOTK_SHADER_SEED_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    /* One folder per translation chain, as on the Mac: the runtime refuses translations of another chain, and the seed
       import keeps files that are already there. Builds before the v5 chain (2026-10-20) kept them directly in
       shader-cache; those, and other chains' folders, are only cache and are removed in the background. */
    NSString *cacheRoot = [data stringByAppendingPathComponent:@"shader-cache"], *chainDir = @"rt-" DMSH_CHAIN;
    NSString *cache = [cacheRoot stringByAppendingPathComponent:chainDir];
    /* Test launches (devicectl --environment-variables '{"TOTK_SHADER_CACHE":"cache-test/1"}') can name another folder
       in Application Support, e.g. an empty one to time a first start; it lies outside shader-cache, so the cleanup leaves it. */
    const char *testCache = getenv("TOTK_SHADER_CACHE");
    if (testCache && *testCache && !strstr(testCache, "..")) cache = [data stringByAppendingPathComponent:@(testCache)];
    [fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("TOTK_SHADER_CACHE",cache.fileSystemRepresentation,1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        NSFileManager *files = [NSFileManager new]; NSUInteger removed = 0;
        for (NSString *name in [files contentsOfDirectoryAtPath:cacheRoot error:nil] ?: @[]) {
            if ([name isEqualToString:chainDir]) continue;
            if ([files removeItemAtPath:[cacheRoot stringByAppendingPathComponent:name] error:nil]) removed++;
        }
        if (removed) fprintf(stderr,"[shader-cache] removed %lu entries of earlier translation chains\n",(unsigned long)removed);
    });
    // Shaders the shared list lacks (totk_shader_contrib.h), in the Files app for players who want to send them; only the Mac asks.
    setenv("TOTK_SHADER_CONTRIB",[docs stringByAppendingPathComponent:@"shader-list-contribution.txt"].fileSystemRepresentation,1);
    setenv("DM_NVN_SHMAP",[res stringByAppendingPathComponent:@"shader-seed/shader_map.tsv"].fileSystemRepresentation,1);
    setenv("DM_MSL_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    NSString *font = [res stringByAppendingPathComponent:@"font"]; setenv("TOTK_FONT_DIR",font.fileSystemRepresentation,1);
    // Save transactions and runtime state remain in private Application Support; Documents exposes only log/export files.
    if (chdir(data.fileSystemRepresentation)) {
        fprintf(stderr, "[acnh] cannot enter private Application Support directory\n"); return NO;
    }
    setenv("TOTK_PROFILE_DIR","save",1);
    setenv("TOTK_GAME_SLOT_DIR",[data stringByAppendingPathComponent:@"game-slot"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PROOF_DIR",data.fileSystemRepresentation,1);   // hash the 17 GB RomFS once, not on every launch
    // 移动端默认禁用无业务 GPU 保活以减少空转耗电；不改渲染器、帧率或画质，显式环境变量仍可覆盖。
    setenv("TOTK_GPU_KEEPALIVE","0",0);
    for (NSString *name in @[@"TOTK_GPU_ALLOW_SUBMIT",@"TOTK_GPU_WIDEN",@"TOTK_GPU_BLOCKRT",@"TOTK_GPU_MIDCLEAR"]) setenv(name.UTF8String,"1",1);
    setenv("TOTK_SLOT_OWNER","user",0); setenv("TOTK_SLOT_PURPOSE","player",0);
    unsetenv("TOTK_AUTOLOAD");
    host_argv[0] = strdup("TOTK"); host_argv[1] = strdup(res.fileSystemRepresentation); host_argv[2] = strdup("boot");
    return YES;   // the window is made when the window scene connects (ACNHSceneDelegate)
}
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(TOTKApp.class)); } }
