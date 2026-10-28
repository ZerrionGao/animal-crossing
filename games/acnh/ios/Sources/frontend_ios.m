#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdatomic.h>
#include <string.h>
#include "runtime.h"

void hle_signal_system_event(void *event);

static __weak CAMetalLayer *g_game_layer;
static _Atomic int g_app_active = 1;
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

void front_frame_presented(void) {}

void runtime_front_request_close(void) {}
void runtime_front_text_test(const char *op, const char *text) {
    (void)op;
    (void)text;
}

void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders,
                           uint64_t shader_total, uint64_t pipelines,
                           uint64_t pipeline_total) {
    (void)done;
    (void)total;
    (void)shaders;
    (void)shader_total;
    (void)pipelines;
    (void)pipeline_total;
}

void runtime_warm_done(void) {}

void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    (void)stage;
    (void)done;
    (void)total;
}

void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) {
    (void)stage;
    (void)done;
    (void)total;
}

void runtime_boot_status(const char *stage, const char *detail) {
    (void)stage;
    (void)detail;
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
