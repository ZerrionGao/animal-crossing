#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

static jmp_buf fail_env;
static int fail_armed;
uint64_t runtime_now_ns(void) { return 1; }
_Noreturn void runtime_fail(const char *message) { (void)message; if (fail_armed) longjmp(fail_env, 1); abort(); }
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }
static void *enter_race(void *unused) {
    (void)unused;
    runtime_oe_enter_exit_request_section();
    return NULL;
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
    enter(); enter();
    /* A full queue leaves the window open; consuming one message makes room for
       the one pending close notification without terminating the process. */
    for (uint32_t i = 0; i < 32; i++) runtime_oe_push_message(100 + i);
    assert(!request_exit());
    assert(!request_exit());
    uint32_t message = 0;
    assert(pop(&message) && message == 100);
    for (uint32_t i = 101; i < 132; i++) assert(pop(&message) && message == i);
    assert(pop(&message) && message == 4);
    assert(request_exit());
    assert(!pop(&message));
    leave(); leave();

    /* 先完成并发 Enter，再检查关闭；锁内计数保证不能越过活动保护区。 */
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, enter_race, NULL));
    assert(!pthread_join(thread, NULL));
    leave();

    fail_armed = 1;
    if (!setjmp(fail_env)) { leave(); assert(!"underflow must fail"); }
    fail_armed = 0;
    return 0;
}
