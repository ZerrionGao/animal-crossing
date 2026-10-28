#include "../engine/runtime/ipc.h"
#include "../engine/runtime/runtime.h"
#include <assert.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }
void runtime_ipc_session_retain(void *session) { (void)session; }
uint64_t runtime_now_ns(void) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (uint64_t)((__uint128_t)mach_absolute_time() * timebase.numer / timebase.denom);
}

static void need(int condition, const char *message) { if (!condition) runtime_fail(message); }
static void svc(unsigned number, uint64_t *regs) { need(svc_thread((int)number, regs) == 1, "SVC dispatch"); }
static uint64_t wait_one(uint32_t handle) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)&handle;
    regs[2] = 1;
    regs[3] = 0;
    svc(0x18, regs);
    return regs[0];
}
static void close_readable(uint32_t handle) {
    uint64_t regs[31] = {handle};
    svc(0x16, regs);
    need(regs[0] == 0, "close readable handle");
}

int main(void) {
    (void)svc_thread_main_init();
    uint32_t writable = runtime_event_new();
    uint32_t read_a = runtime_event_readable(writable);
    uint32_t read_b = runtime_event_readable(writable);
    need(read_a != read_b, "read handles are independent handles");
    need(wait_one(read_a) == 0xea01, "new event begins unsignaled");

    runtime_event_signal(writable);
    need(wait_one(read_a) == 0 && wait_one(read_b) == 0, "read handles observe the same signal");
    uint64_t regs[31] = {read_a};
    svc(0x17, regs);
    need(regs[0] == 0, "reset event through readable handle");
    need(wait_one(read_b) == 0xea01, "reset clears shared event state");

    runtime_event_close_writable(writable);
    need(wait_one(read_a) == 0xea01 && wait_one(read_b) == 0xea01, "read ends survive service write-end close");
    close_readable(read_a);
    need(wait_one(read_a) == 0xe401, "closed readable handle is invalid");
    need(wait_one(read_b) == 0xea01, "other readable handle remains valid");
    close_readable(read_b);

    writable = runtime_event_new();
    read_a = runtime_event_readable(writable);
    close_readable(read_a);
    need(wait_one(read_a) == 0xe401, "read end closes while service end stays open");
    read_b = runtime_event_readable(writable);
    runtime_event_close_writable(writable);
    need(wait_one(read_b) == 0xea01, "new read end survives reverse-order close");
    close_readable(read_b);

    for (unsigned i = 0; i < 5000; i++) {
        writable = runtime_event_new();
        read_a = runtime_event_readable(writable);
        read_b = runtime_event_readable(writable);
        close_readable(read_a);
        close_readable(read_b);
        runtime_event_close_writable(writable);
    }
    puts("{\"status\":\"pass\",\"event_cycles\":5000,\"scope\":\"production event handle allocation, wait, signal, reset and both close orders\"}");
    return 0;
}
