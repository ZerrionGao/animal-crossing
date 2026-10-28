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
static int expected_failure;

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    if (expected_failure) exit(42);
    abort();
}
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }
void *runtime_override(const char *name) { (void)name; return NULL; }
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

static void make_cmif(uint32_t *message, uint32_t command, const void *args, unsigned args_size) {
    memset(message, 0, 0x100);
    unsigned actual = 16 + 16 + args_size;
    message[0] = 4;
    message[1] = (actual + 3) / 4;
    uint32_t header[4] = {0x49434653u, 0, command, 0};
    memcpy((uint8_t *)message + 16, header, sizeof header);
    if (args_size) memcpy((uint8_t *)message + 32, args, args_size);
}

#ifdef GAME_ACNH
static void make_control(uint32_t *message, uint32_t command) {
    make_cmif(message, command, NULL, 0);
    message[0] = 7;
}

static void make_domain_get_region(uint32_t *message) {
    memset(message, 0, 0x100);
    message[0] = 4;
    message[1] = 12;
    uint8_t domain_header[16] = {1, 0};
    uint16_t domain_size = 16;
    uint32_t object_id = 1;
    memcpy(domain_header + 2, &domain_size, sizeof domain_size);
    memcpy(domain_header + 4, &object_id, sizeof object_id);
    memcpy((uint8_t *)message + 16, domain_header, sizeof domain_header);
    uint32_t cmif_header[4] = {0x49434653u, 0, 4, 0};
    memcpy((uint8_t *)message + 32, cmif_header, sizeof cmif_header);
}
#endif

static uint32_t open_settings(uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    const char service_name[8] = {'s','e','t',0,0,0,0,0};
    make_cmif(message, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

static void check_result(uint32_t *message, int32_t expected) {
    uint8_t *payload = (uint8_t *)message + 16;
    assert(((uint32_t *)payload)[2] == 0);
    assert(*(int32_t *)(payload + 16) == expected);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "success";
    expected_failure = strcmp(mode, "success") != 0;
    uint32_t message[64];
    uint32_t settings = open_settings(message);

#ifdef GAME_ACNH
    if (strcmp(mode, "language") == 0) {
        make_cmif(message, 0, NULL, 0);
        message[0] = 6;
        send_frame(settings, message);
        uint64_t expected_language = 0;
        memcpy(&expected_language, "zh-Hans", 7);
        uint8_t *payload = (uint8_t *)message + 16;
        assert(((uint32_t *)payload)[2] == 0);
        assert(*(uint64_t *)(payload + 16) == expected_language);
        return 0;
    }
    if (strcmp(mode, "unsupported") == 0) {
        make_cmif(message, 1, NULL, 0);
        send_frame(settings, message);
        assert(!"unobserved ACNH settings command must fail");
    }
    if (strcmp(mode, "bad-shape") == 0) {
        uint32_t argument = 0x12345678;
        make_cmif(message, 4, &argument, sizeof argument);
        send_frame(settings, message);
        assert(!"GetRegionCode with an argument must fail");
    }
    if (strcmp(mode, "domain") == 0) {
        make_control(message, 0);
        send_frame(settings, message);
        assert(((uint32_t *)((uint8_t *)message + 16))[2] == 0);
        assert(*(uint32_t *)((uint8_t *)message + 32) == 1);
        make_domain_get_region(message);
        send_frame(settings, message);
        uint8_t *payload = (uint8_t *)message + 16;
        assert(((uint32_t *)payload)[6] == 0);
        assert(*(int32_t *)(payload + 32) == 1);
        return 0;
    }
    if (strcmp(mode, "request-context") == 0) {
        make_cmif(message, 4, NULL, 0);
        message[0] = 6;
        send_frame(settings, message);
        check_result(message, 1);
        return 0;
    }
    make_cmif(message, 4, NULL, 0);
    send_frame(settings, message);
    check_result(message, 1);
#else
    make_cmif(message, 4, NULL, 0);
    send_frame(settings, message);
    check_result(message, 1);
    make_cmif(message, 0, NULL, 0);
    send_frame(settings, message);
    uint64_t expected_language = 0;
    memcpy(&expected_language, "en-US", 5);
    uint8_t *payload = (uint8_t *)message + 16;
    assert(((uint32_t *)payload)[2] == 0);
    assert(*(uint64_t *)(payload + 16) == expected_language);
#endif

    runtime_ipc_session_closed(runtime_handle_get(settings, 1));
    return 0;
}
