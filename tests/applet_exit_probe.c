#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

static jmp_buf fail_env;
static int fail_armed;
static _Atomic int busy_polls;
static _Atomic uint64_t fake_ns = 1, busy_step_ns;
uint64_t runtime_now_ns(void) { return fake_ns; }
_Noreturn void runtime_fail(const char *message) { (void)message; if (fail_armed) longjmp(fail_env, 1); abort(); }
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }
int runtime_fs_save_busy(void) {
    if (busy_polls <= 0) return 0;
    busy_polls--;
    fake_ns += busy_step_ns;
    return 1;
}
static void *enter_race(void *unused) {
    (void)unused;
    runtime_oe_enter_exit_request_section();
    return NULL;
}
static void expect_exit_zero(pid_t child) {
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    void (*enter)(void) = runtime_applet_override("_ZN2nn2oe31EnterExitRequestHandlingSectionEv");
    void (*leave)(void) = runtime_applet_override("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv");
    unsigned char (*pop)(uint32_t *) = runtime_applet_override("_ZN2nn2oe25TryPopNotificationMessageEPj");
    int (*request_exit)(void) = runtime_oe_request_exit_notification;
    void (*set_extended)(unsigned char) = runtime_applet_override("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb");
    unsigned char (*is_extended)(void) = runtime_applet_override("_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv");
    assert(enter && leave && pop && request_exit && set_extended && is_extended);

    set_extended(1); assert(is_extended());
    set_extended(0); assert(!is_extended());

    /* 处理区内关闭：队列满时消息 4 在消费后补投；进程留到最后一层 Leave，再等存档空闲后以 0 结束。 */
    fflush(NULL);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        enter(); enter();
        for (uint32_t i = 0; i < 32; i++) runtime_oe_push_message(100 + i);
        if (request_exit()) _exit(10);
        if (request_exit()) _exit(11);
        uint32_t message = 0;
        if (!pop(&message) || message != 100) _exit(12);
        for (uint32_t i = 101; i < 132; i++) if (!pop(&message) || message != i) _exit(13);
        if (!pop(&message) || message != 4) _exit(14);
        if (!request_exit()) _exit(15);
        if (pop(&message)) _exit(16);
        busy_polls = 5;
        usleep(200000);
        leave();
        usleep(200000);
        if (busy_polls != 5) _exit(17);
        leave();
        sleep(5);
        _exit(18);
    }
    expect_exit_zero(child);

    /* 处理区外关闭：不等游戏处理消息，存档空闲后直接结束。 */
    fflush(NULL);
    child = fork();
    assert(child >= 0);
    if (!child) {
        busy_polls = 3;
        if (!request_exit()) _exit(20);
        sleep(5);
        _exit(21);
    }
    expect_exit_zero(child);

    /* 存档一直忙：每次查询推进 1 秒，15 秒后放弃等待并结束。 */
    fflush(NULL);
    child = fork();
    assert(child >= 0);
    if (!child) {
        busy_step_ns = 1000000000ull;
        busy_polls = 1000;
        if (!request_exit()) _exit(30);
        sleep(5);
        _exit(31);
    }
    expect_exit_zero(child);

    enter(); enter();
    leave(); leave();

    /* 先完成并发 Enter，再检查关闭；锁内计数保证不能越过活动保护区。 */
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, enter_race, NULL));
    assert(!pthread_join(thread, NULL));
    leave();

    fail_armed = 1;
    if (!setjmp(fail_env)) { leave(); assert(!"underflow must fail"); }
    fail_armed = 0;
    puts("ACNH applet exit probe passed");
    return 0;
}
