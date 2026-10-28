#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/runtime/ipc.h"
#include "../engine/runtime/runtime.h"

pthread_key_t runtime_keys[8];
static void *handles[4096];
static uint32_t next_handle = 0x1000;

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    exit(42);
}
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }
uint32_t runtime_handle_new(int session, void *object) {
    (void)session;
    uint32_t handle = next_handle++;
    handles[handle & 4095] = object;
    return handle;
}
void *runtime_handle_get(uint32_t handle, int session) { (void)session; return handles[handle & 4095]; }
void *runtime_handle_acquire_session(uint32_t handle) {
    IpcSession *session = runtime_handle_get(handle, 1);
    if (session) runtime_ipc_session_retain(session);
    return session;
}
void runtime_event_signal(uint32_t handle) { (void)handle; }
uint32_t runtime_event_new(void) { return 0; }
uint32_t runtime_event_readable(uint32_t handle) { return handle; }

static void send_frame(uint32_t handle, uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[0] = (uintptr_t)message;
    regs[1] = 0x100;
    regs[2] = handle;
    assert(runtime_ipc_svc(0x22, regs));
    assert(regs[0] == 0);
}

static void make_cmif(uint32_t *message, unsigned type, uint32_t command,
                      const void *args, unsigned args_size) {
    memset(message, 0, 0x100);
    unsigned actual = 16 + 16 + args_size;
    message[0] = type;
    message[1] = (actual + 3) / 4;
    uint32_t header[4] = {0x49434653u, 0, command, 0};
    memcpy((uint8_t *)message + 16, header, sizeof header);
    if (args_size) memcpy((uint8_t *)message + 32, args, args_size);
}

static void make_observed_pctl(uint32_t *message, uint32_t command,
                               const void *args, unsigned args_size) {
    memset(message, 0, 0x100);
    message[0] = 6;
    message[1] = (8u + 16u + args_size + 3u) / 4u;
    uint32_t header[4] = {0x49434653u, 0, command, 0};
    memcpy((uint8_t *)message + 16, header, sizeof header);
    if (args_size) memcpy((uint8_t *)message + 32, args, args_size);
}

static uint32_t open_pctl(uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    const char service_name[8] = {'p','c','t','l',0,0,0,0};
    make_cmif(message, 4, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

static uint32_t open_named(uint32_t *message, const char *name) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    char service_name[8] = {0};
    memcpy(service_name, name, strlen(name));
    make_cmif(message, 4, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

int main(int argc, char **argv) {
#ifdef GAME_ACNH
    const char *mode = argc > 1 ? argv[1] : "service";
    uint32_t message[64];
    if (strcmp(mode, "special") == 0) {
        uint32_t special = open_named(message, "pctl:s");
        runtime_ipc_session_closed(runtime_handle_get(special, 1));
        return 0;
    }
    uint32_t pctl = open_pctl(message);
    if (strcmp(mode, "factory-unknown") == 0) {
        make_observed_pctl(message, 2, NULL, 0);
        send_frame(pctl, message);
        assert(!"unknown pctl factory request must fail");
    }
    uint64_t application_id = 0x01006f8002326000ULL;
    make_observed_pctl(message, 1, &application_id, sizeof application_id);
    send_frame(pctl, message);
    uint32_t service = ((uint32_t *)message)[3];
    assert(service != 0);
    runtime_ipc_session_closed(runtime_handle_get(pctl, 1));
    if (strcmp(mode, "factory-only") == 0) {
        runtime_ipc_session_closed(runtime_handle_get(service, 1));
        return 0;
    }
    if (strcmp(mode, "service-unknown") == 0) {
        make_observed_pctl(message, 1031, NULL, 0);
        send_frame(service, message);
        assert(!"unknown pctl service request must fail");
    }
    if (strcmp(mode, "service-bad-shape") == 0) {
        uint32_t extra = 0;
        make_observed_pctl(message, 1018, &extra, sizeof extra);
        send_frame(service, message);
        assert(!"malformed pctl service request must fail");
    }
    make_observed_pctl(message, 1, NULL, 0);
    send_frame(service, message);
    assert(message[6] == 0);
    make_observed_pctl(message, 1001, NULL, 0);
    send_frame(service, message);
    assert(message[6] == 0);
    make_observed_pctl(message, 1018, NULL, 0);
    send_frame(service, message);
    assert(message[6] == 0);
    runtime_ipc_session_closed(runtime_handle_get(service, 1));
    return 0;
#else
    (void)argc;
    (void)argv;
#endif
    return 0;
}
