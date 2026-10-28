/* Host side of applet, telemetry, NFC and error-display services that main reaches through IPC.
   Client-side SDK code (PlayReport encoding, keyboard presets, diag logging) runs unchanged; only the
   calls that would talk to system services are taken here. */
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- nn::oe ---------- */
static float balance[2] = {1.0f, 1.0f};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t message_cv = PTHREAD_COND_INITIALIZER;
static uint32_t messages[32]; static unsigned head, tail;
static uint64_t start_ns;
#ifdef GAME_ACNH
static unsigned exit_section_depth;
static int exit_close_requested;
static int exit_close_queued;
static int exit_close_delivered;
static int exit_close_deferred;
static int exit_close_started;
static unsigned char inactivity_extended_enabled;
#endif
__attribute__((constructor)) static void started(void) { start_ns = runtime_now_ns(); }

static int push_locked(uint32_t message) {
    if (tail - head >= 32) return 0;
    messages[tail++ % 32] = message;
    pthread_cond_broadcast(&message_cv);
    return 1;
}
#ifdef GAME_ACNH
static int queue_exit_close_locked(void) {
    if (!exit_close_requested || exit_close_queued || exit_close_delivered) return 1;
    if (!push_locked(4)) { exit_close_deferred = 1; return 0; }
    exit_close_queued = 1;
    if (exit_close_deferred) {
        exit_close_deferred = 0;
        fprintf(stderr, "[totk] ACNH notification queue has space; queued pending exit message 4\n");
    }
    return 1;
}
static uint32_t pop_locked(void) {
    uint32_t message = messages[head++ % 32];
    if (message == 4 && exit_close_queued) {
        exit_close_queued = 0;
        exit_close_delivered = 1;
    }
    (void)queue_exit_close_locked();
    return message;
}
#endif
void runtime_oe_push_message(uint32_t message) {
    pthread_mutex_lock(&lock);
    (void)push_locked(message);
    pthread_mutex_unlock(&lock);
}
static void get_balance(float *l, float *r) { pthread_mutex_lock(&lock); if (l) *l = balance[0]; if (r) *r = balance[1]; pthread_mutex_unlock(&lock); }
static void set_balance(float l, float r) {
    if (l < 0 || l > 1 || r < 0 || r > 1) runtime_fail("volume balance out of range");
    pthread_mutex_lock(&lock); balance[0] = l; balance[1] = r; pthread_mutex_unlock(&lock);
}
static int64_t total_active_time(void) { return (int64_t)(runtime_now_ns() - start_ns); }   /* TimeSpan in ns */
#ifdef GAME_ACNH
/* SDK的嵌套退出保护区；消息4只在首次请求时入队。 */
void runtime_oe_enter_exit_request_section(void) {
    pthread_mutex_lock(&lock);
    if (exit_section_depth == UINT32_MAX) { pthread_mutex_unlock(&lock); runtime_fail("oe exit section depth overflow"); }
    exit_section_depth++;
    pthread_mutex_unlock(&lock);
}
void runtime_oe_leave_exit_request_section(void) {
    pthread_mutex_lock(&lock);
    if (!exit_section_depth) { pthread_mutex_unlock(&lock); runtime_fail("oe exit section depth underflow"); }
    int released = --exit_section_depth == 0 && exit_close_requested;
    pthread_mutex_unlock(&lock);
    if (released) fprintf(stderr, "[totk] ACNH exit request handling section released; the process ends once save data is idle\n");
}
/* 与 Switch 关闭软件一致：游戏在退出请求处理区内（存档时）就等它离开，不在区内则直接结束，不运行客体收尾。
   结束前另等宿主存档空闲，最多 15 秒；设备存档按槽提交，放弃时只丢失尚未提交的这一次。 */
static void *exit_when_released(void *unused) {
    (void)unused;
    uint64_t busy_since = 0;
    int busy;
    /* 决定结束后一直持锁到 _exit，游戏不能再进入退出请求处理区。 */
    for (;;) {
        pthread_mutex_lock(&lock);
        busy = 0;
        if (exit_section_depth) busy_since = 0;
        else if (!(busy = runtime_fs_save_busy())) break;
        else if (!busy_since) busy_since = runtime_now_ns();
        else if (runtime_now_ns() - busy_since >= 15000000000ull) break;
        pthread_mutex_unlock(&lock);
        usleep(20000);
    }
    fprintf(stderr, "[totk] ACNH exit: save data %s; process ends\n",
            busy ? "still being written after 15 s (the uncommitted save is lost)" : "idle");
    _exit(0);
}
int runtime_oe_request_exit_notification(void) {
    pthread_mutex_lock(&lock);
    exit_close_requested = 1;
    int queued = queue_exit_close_locked();
    int start = !exit_close_started;
    exit_close_started = 1;
    pthread_mutex_unlock(&lock);
    if (start) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, exit_when_released, NULL)) runtime_fail("start ACNH exit thread");
        pthread_detach(thread);
    }
    return queued;
}
static void inactivity_extended(unsigned char on) {
    pthread_mutex_lock(&lock); inactivity_extended_enabled = !!on; pthread_mutex_unlock(&lock);
}
static unsigned char inactivity_extended_get(void) {
    pthread_mutex_lock(&lock); unsigned char on = inactivity_extended_enabled; pthread_mutex_unlock(&lock); return on;
}
/* GetPseudoDeviceId 先调用 GetApplicationFunctions。应用接口全局尚未建立时该指针为空，
   原 SDK 接着读取虚表 +0xe8 并崩溃。这里只提供持久的 16 字节本地标识，不创建 AM 对象。 */
static unsigned char pseudo_device_id[16];
static int pseudo_device_ready;
static pthread_mutex_t pseudo_device_lock = PTHREAD_MUTEX_INITIALIZER;
static void pseudo_device_fail(int fd, const char *message) {
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&pseudo_device_lock);
    runtime_fail(message);
}
static void load_pseudo_device_id(void) {
    pthread_mutex_lock(&pseudo_device_lock);
    if (pseudo_device_ready) { pthread_mutex_unlock(&pseudo_device_lock); return; }
    const char *dir = getenv("TOTK_PROFILE_DIR");
    if (!dir || !dir[0]) dir = "user-data";
    if (mkdir(dir, 0700) && errno != EEXIST) pseudo_device_fail(-1, "create pseudo device id directory");
    char path[4096];
    if (snprintf(path, sizeof path, "%s/pseudo-device-id", dir) >= (int)sizeof path)
        pseudo_device_fail(-1, "pseudo device id path length");
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) pseudo_device_fail(-1, "open pseudo device id");
    if (flock(fd, LOCK_EX)) pseudo_device_fail(fd, "lock pseudo device id");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) pseudo_device_fail(fd, "invalid pseudo device id file");
    if (st.st_size == 0) {
        arc4random_buf(pseudo_device_id, sizeof pseudo_device_id);
        pseudo_device_id[6] = (unsigned char)((pseudo_device_id[6] & 0x0f) | 0x40);
        pseudo_device_id[8] = (unsigned char)((pseudo_device_id[8] & 0x3f) | 0x80);
        if (write(fd, pseudo_device_id, sizeof pseudo_device_id) != (ssize_t)sizeof pseudo_device_id || fsync(fd))
            pseudo_device_fail(fd, "persist pseudo device id");
    } else if (st.st_size != (off_t)sizeof pseudo_device_id ||
               pread(fd, pseudo_device_id, sizeof pseudo_device_id, 0) != (ssize_t)sizeof pseudo_device_id) {
        pseudo_device_fail(fd, "corrupt pseudo device id");
    } else {
        int nonzero = 0;
        for (size_t i = 0; i < sizeof pseudo_device_id; i++) if (pseudo_device_id[i]) nonzero = 1;
        if (!nonzero) pseudo_device_fail(fd, "corrupt pseudo device id");
    }
    close(fd);
    pseudo_device_ready = 1;
    pthread_mutex_unlock(&pseudo_device_lock);
    fprintf(stderr, "[acnh-oe] persistent local pseudo device id loaded\n");
}
static void get_pseudo_device_id(unsigned char *out) {
    if (!out) runtime_fail("ACNH GetPseudoDeviceId output is null");
    load_pseudo_device_id();
    pthread_mutex_lock(&pseudo_device_lock);
    memcpy(out, pseudo_device_id, sizeof pseudo_device_id);
    pthread_mutex_unlock(&pseudo_device_lock);
}
#else
/* 宿主不执行主机系统的自动休眠策略。 */
static void inactivity_extended(unsigned char on) { (void)on; }
#endif
/* No system startup logo is drawn on the host, so there is nothing to dismiss. */
static void finish_startup_logo(void) { fprintf(stderr, "[totk] game finished startup logo\n"); }
/* The single native executable was launched directly; it has no predecessor
 * in an applet ExecuteProgram chain. -1 keeps the game's normal user-open path. */
static int previous_program_index(void) { return -1; }
/* A stable, nonzero resource id for this process's one application. Host input
 * and audio share that application; there is no separate system applet client. */
static uint64_t applet_resource_user_id(void) { return 1; }
static unsigned char try_pop_launch_parameter(uint64_t *size,void *buffer,uint64_t capacity) {
    (void)buffer;(void)capacity;if(size)*size=0;return 0;
}
static unsigned char try_pop(uint32_t *out) {
    pthread_mutex_lock(&lock); int ok = head != tail;
    if (ok) {
#ifdef GAME_ACNH
        uint32_t message = pop_locked();
#else
        uint32_t message = messages[head++ % 32];
#endif
        if (out) *out = message;
    }
    pthread_mutex_unlock(&lock); return (unsigned char)ok;
}
int runtime_core_away(void); void runtime_core_back(int *away);
static uint32_t pop(void) {
    __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away();   /* blocks like a Horizon wait */
    pthread_mutex_lock(&lock); while (head == tail) pthread_cond_wait(&message_cv, &lock);
#ifdef GAME_ACNH
    uint32_t m = pop_locked();
#else
    uint32_t m = messages[head++ % 32];
#endif
    pthread_mutex_unlock(&lock); return m;
}

/* ---------- nn::prepo: play reports are not transmitted (no Nintendo servers) ---------- */
static uint64_t reports;
static unsigned report_save(const void *report) { (void)report; if (!(reports++ % 100)) fprintf(stderr, "[totk] play report kept local (not transmitted), count %llu\n", (unsigned long long)reports); return 0; }
static unsigned report_save_uid(const void *report, const void *uid) { (void)uid; return report_save(report); }
static unsigned immediate_transmission(void) { return 0; }
static unsigned connection_status(void *report, const char *a, const char *b, const char *c) { (void)report; (void)a; (void)b; (void)c; return 0; }

/* ---------- nn::nfp: the host has no NFC reader ---------- */
#define NFP_DEVICE_NOT_FOUND 0x8073u   /* nfp module 115, description 64 */
static int nfp_state;
static unsigned nfp_initialize(void) { nfp_state = 1; return 0; }
static unsigned nfp_finalize(void) { nfp_state = 0; return 0; }
static int nfp_get_state(void) { return nfp_state; }
static unsigned nfp_list(void *handles, int *count, int max) { (void)handles; (void)max; if (count) *count = 0; return NFP_DEVICE_NOT_FOUND; }
static unsigned nfp_attach_availability(void *event) { if (!event) runtime_fail("nfp availability event"); hle_bind_system_event(event, 0); return 0; }
static unsigned nfp_no_device(void) { return NFP_DEVICE_NOT_FOUND; }
static int nfp_device_state(const void *h) { (void)h; return 0; }

/* ---------- nn::err (nn::swkbd is in runtime/swkbd_host.c) ---------- */
static void show_error(uint32_t result) {
    fprintf(stderr, "[totk] game error dialog: %04u-%04u (result %#x)\n", 2000 + (result & 0x1ff), (result >> 9) & 0x1fff, result);
}

#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md). Exit requests are handled by the window (the section brackets change nothing);
   video recording has no Mac counterpart; the DLC shop and the DLC-version error screen are console applets. */
static void exit_section(void) {}
static void enable_recording(void) {}
static void unacceptable_aoc_version(void) { fprintf(stderr, "[botw] the game reported a DLC version it cannot use\n"); }
static uint32_t show_aoc_shop(uint64_t application, const void *user) { (void)application; (void)user; fprintf(stderr, "[botw] eShop DLC page: not available\n"); return 0; }
#endif
void *runtime_applet_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn6applet23GetPreviousProgramIndexEv", previous_program_index},
        {"_ZN2nn6applet23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2am23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2oe21TryPopLaunchParameterEPmPvm", try_pop_launch_parameter},
        {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", get_balance}, {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", set_balance},
        {"_ZN2nn2oe25GetProgramTotalActiveTimeEv", total_active_time}, {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", inactivity_extended},
        {"_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", inactivity_extended},
        {"_ZN2nn2oe17FinishStartupLogoEv", finish_startup_logo}, {"_ZN2nn2oe25TryPopNotificationMessageEPj", try_pop},
        {"_ZN2nn2oe22PopNotificationMessageEv", pop},
        {"_ZN2nn5prepo10PlayReport4SaveEv", report_save}, {"_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", report_save_uid},
        {"_ZN2nn5prepo28RequestImmediateTransmissionEv", immediate_transmission},
        {"_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", connection_status},
        {"_ZN2nn3nfp10InitializeEv", nfp_initialize}, {"_ZN2nn3nfp8FinalizeEv", nfp_finalize}, {"_ZN2nn3nfp8GetStateEv", nfp_get_state},
        {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list},
        {"_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", nfp_attach_availability},
        {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", nfp_device_state},
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_no_device},
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_no_device}, {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3err9ShowErrorENS_6ResultE", show_error},
#ifdef GAME_BOTW
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", exit_section}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", exit_section},
        {"_ZN2nn2oe15EnableRecordingEv", enable_recording},
        {"_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", unacceptable_aoc_version},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", show_aoc_shop},
#endif
#ifdef GAME_ACNH
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", runtime_oe_enter_exit_request_section},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", runtime_oe_leave_exit_request_section},
        {"_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", inactivity_extended_get},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", get_pseudo_device_id},
#endif
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
