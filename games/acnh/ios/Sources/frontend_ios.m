#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "runtime.h"
#include "totk_ui_lang.h"

void hle_signal_system_event(void *event);

static __weak CAMetalLayer *g_game_layer;
static _Atomic int g_app_active = 1;
static _Atomic uint64_t g_presented_frames;
static _Atomic int g_warm_pending;
static UIView *g_boot_overlay;
static UILabel *g_boot_stage, *g_boot_detail;
static UIProgressView *g_boot_progress;
static UIActivityIndicatorView *g_boot_spin;
static int g_vi_ready;
static unsigned g_display_identity, g_layer_identity;
static pthread_mutex_t g_vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_vsync_events[16];
static unsigned g_vsync_count;
static CADisplayLink *g_display_link;

@interface ACNHDisplayLinkTarget : NSObject
- (void)tick:(CADisplayLink *)link;
@end

@implementation ACNHDisplayLinkTarget
- (void)tick:(CADisplayLink *)link {
    (void)link;
    void *events[16];
    pthread_mutex_lock(&g_vsync_lock);
    unsigned count = g_vsync_count;
    memcpy(events, g_vsync_events, count * sizeof(events[0]));
    pthread_mutex_unlock(&g_vsync_lock);
    for (unsigned i = 0; i < count; i++) hle_signal_system_event(events[i]);
}
@end

static ACNHDisplayLinkTarget *g_display_link_target;

void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host) {
    (void)host;
    g_game_layer = layer;
}

static void boot_hide_if_ready(void) {
    if (atomic_load(&g_warm_pending)) return;
    if (!atomic_load(&g_presented_frames)) return;
    dispatch_async(dispatch_get_main_queue(), ^{ g_boot_overlay.hidden = YES; });
}

void runtime_ios_install_boot_overlay(UIView *root) {
    if (!root || g_boot_overlay) return;
    g_boot_overlay = [[UIView alloc] initWithFrame:root.bounds];
    g_boot_overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    g_boot_overlay.backgroundColor = [UIColor colorWithRed:0.035 green:0.075 blue:0.09 alpha:1];
    g_boot_stage = [UILabel new];
    g_boot_stage.text = @(TOTK_UI("正在启动", "Starting"));
    g_boot_stage.font = [UIFont systemFontOfSize:25 weight:UIFontWeightSemibold];
    g_boot_stage.textColor = UIColor.whiteColor;
    g_boot_stage.textAlignment = NSTextAlignmentCenter;
    g_boot_detail = [UILabel new];
    g_boot_detail.text = @(TOTK_UI("正在准备游戏环境", "Preparing the game environment"));
    g_boot_detail.font = [UIFont systemFontOfSize:14];
    g_boot_detail.textColor = [UIColor colorWithWhite:0.72 alpha:1];
    g_boot_detail.textAlignment = NSTextAlignmentCenter;
    g_boot_detail.numberOfLines = 0;
    g_boot_progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
    g_boot_progress.progressTintColor = [UIColor colorWithRed:0.35 green:0.78 blue:0.82 alpha:1];
    g_boot_progress.trackTintColor = [UIColor colorWithWhite:1 alpha:0.16];
    g_boot_progress.hidden = YES;
    g_boot_spin = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
    g_boot_spin.color = UIColor.whiteColor;
    [g_boot_spin startAnimating];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[g_boot_stage, g_boot_detail, g_boot_spin, g_boot_progress]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [g_boot_overlay addSubview:stack];
    [root addSubview:g_boot_overlay];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:g_boot_overlay.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:g_boot_overlay.centerYAnchor],
        [stack.widthAnchor constraintEqualToAnchor:g_boot_overlay.widthAnchor multiplier:0.72],
        [g_boot_progress.widthAnchor constraintEqualToConstant:280],
    ]];
}

void *front_metal_layer(void) {
    return (__bridge void *)g_game_layer;
}

void runtime_ios_set_active(int active) {
    int is_active = !!active;
    atomic_store_explicit(&g_app_active, is_active, memory_order_release);
    if (NSThread.isMainThread) g_display_link.paused = !is_active;
    else dispatch_async(dispatch_get_main_queue(), ^{ g_display_link.paused = !is_active; });
}

int runtime_front_focus(void) {
    return atomic_load_explicit(&g_app_active, memory_order_acquire);
}

void runtime_front_run(void (*callback)(void)) {
    if (callback) callback();
}

void front_frame_presented(void) {
    if (atomic_fetch_add_explicit(&g_presented_frames, 1, memory_order_relaxed) == 0)
        boot_hide_if_ready();
}

void runtime_front_request_close(void) {}
void runtime_front_text_test(const char *op, const char *text) {
    (void)op;
    (void)text;
}

void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders,
                           uint64_t shader_total, uint64_t pipelines,
                           uint64_t pipeline_total) {
    atomic_store(&g_warm_pending, 1);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_boot_overlay || g_boot_overlay.hidden) return;
        double percent = total ? fmin(100.0, (double)done / (double)total * 100.0) : 100.0;
        g_boot_stage.text = [NSString stringWithFormat:@(TOTK_UI("预热着色器与管线 %.0f%%", "Warming shaders and pipelines %.0f%%")), floor(percent)];
        g_boot_detail.text = [NSString stringWithFormat:@(TOTK_UI("已处理 %llu/%llu 项 · 着色器 %llu/%llu · 管线 %llu/%llu", "%llu/%llu items · shaders %llu/%llu · pipelines %llu/%llu")),
            (unsigned long long)done, (unsigned long long)total, (unsigned long long)shaders, (unsigned long long)shader_total,
            (unsigned long long)pipelines, (unsigned long long)pipeline_total];
        g_boot_spin.hidden = YES; [g_boot_spin stopAnimating];
        g_boot_progress.hidden = NO;
        g_boot_progress.progress = (float)(percent / 100.0);
    });
}

void runtime_warm_done(void) {
    atomic_store(&g_warm_pending, 0);
    boot_hide_if_ready();
}

static void boot_show_stage(NSString *stage, NSString *detail, int determinate, double percent) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_boot_overlay || g_boot_overlay.hidden) return;
        g_boot_stage.text = stage;
        g_boot_detail.text = detail;
        if (determinate) {
            g_boot_spin.hidden = YES; [g_boot_spin stopAnimating];
            g_boot_progress.hidden = NO;
            g_boot_progress.progress = (float)(percent / 100.0);
        } else {
            g_boot_progress.hidden = YES;
            g_boot_spin.hidden = NO; [g_boot_spin startAnimating];
        }
    });
}

void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    NSString *s = [NSString stringWithUTF8String:stage ? stage : TOTK_UI("正在启动", "Starting")];
    if (total) {
        boot_show_stage(s, [NSString stringWithFormat:@"%.1f / %.1f GB", done / 1e9, total / 1e9], 1, (double)done / (double)total * 100.0);
    } else {
        boot_show_stage(s, @(TOTK_UI("正在处理，请稍候", "Working, please wait")), 0, 0);
    }
}

void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) {
    NSString *s = [NSString stringWithUTF8String:stage ? stage : TOTK_UI("正在启动", "Starting")];
    boot_show_stage(s, [NSString stringWithFormat:@"%llu / %llu", (unsigned long long)done, (unsigned long long)total],
                    total ? 1 : 0, total ? (double)done / (double)total * 100.0 : 0);
}

void runtime_boot_status(const char *stage, const char *detail) {
    boot_show_stage([NSString stringWithUTF8String:stage ? stage : TOTK_UI("正在启动", "Starting")],
                    [NSString stringWithUTF8String:detail ? detail : TOTK_UI("正在处理，请稍候", "Working, please wait")],
                    0, 0);
}

static uint32_t vi_initialize(void) {
    if (!g_game_layer) return 0xf601;
    g_vi_ready = 1;
    return 0;
}

static uint32_t vi_open(void **out) {
    if (!g_vi_ready || !out) return 0xf601;
    *out = &g_display_identity;
    return 0;
}

static uint32_t vi_layer(void **out, void *display) {
    if (!out || display != &g_display_identity) return 0xf601;
    *out = &g_layer_identity;
    return 0;
}

static uint32_t vi_window(void **out, void *layer) {
    if (!out || layer != &g_layer_identity || !g_game_layer) return 0xf601;
    *out = (__bridge void *)g_game_layer;
    return 0;
}

static uint32_t vi_vsync(void *event, void *display) {
    if (!event || display != &g_display_identity) return 0xf601;
    hle_bind_system_event(event, 1);
    pthread_mutex_lock(&g_vsync_lock);
    if (g_vsync_count == sizeof(g_vsync_events) / sizeof(g_vsync_events[0])) {
        pthread_mutex_unlock(&g_vsync_lock);
        return 0xf601;
    }
    g_vsync_events[g_vsync_count++] = event;
    pthread_mutex_unlock(&g_vsync_lock);
    if (!g_display_link) {
        g_display_link_target = [ACNHDisplayLinkTarget new];
        g_display_link = [CADisplayLink displayLinkWithTarget:g_display_link_target selector:@selector(tick:)];
        [g_display_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        g_display_link.paused = !runtime_front_focus();
    }
    return 0;
}

void *runtime_front_override(const char *name) {
    static const struct { const char *name; void *function; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", vi_initialize},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync},
    };
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
        if (!strcmp(name, entries[i].name)) return entries[i].function;
    return NULL;
}
