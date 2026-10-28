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
void runtime_account_uid(uint64_t out[2]) {
    out[0] = 0x1122334455667788ULL;
    out[1] = 0x8877665544332211ULL;
}

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

static void make_control(uint32_t *message, uint32_t command) {
    make_cmif(message, 7, command, NULL, 0);
}

static void make_domain(uint32_t *message, uint32_t command, const void *args,
                        unsigned args_size, int has_pid, unsigned x_descriptors, unsigned copy_handles) {
    memset(message, 0, 0x100);
    unsigned domain_data_size = 16 + args_size;
    unsigned data_words = (16 + 16 + domain_data_size + 3) / 4;
    message[0] = 6 | (x_descriptors << 16);
    message[1] = data_words;
    size_t raw_offset = 8;
    if (has_pid || copy_handles) {
        message[1] |= 1u << 31;
        message[2] = (has_pid ? 1u : 0u) | (copy_handles << 1);
        if (has_pid) *(uint64_t *)((uint8_t *)message + 12) = 0x1122334455667788ULL;
        if (copy_handles) message[3] = 0xfedcba98u;
        raw_offset += 4 + (has_pid ? 8 : 0) + copy_handles * 4;
    }
    raw_offset += x_descriptors * 8;
    size_t payload_offset = (raw_offset + 15) & ~(size_t)15;
    uint8_t domain_header[16] = {1, 0};
    uint16_t data_size = (uint16_t)domain_data_size;
    uint32_t object_id = 1;
    memcpy(domain_header + 2, &data_size, sizeof data_size);
    memcpy(domain_header + 4, &object_id, sizeof object_id);
    memcpy((uint8_t *)message + payload_offset, domain_header, sizeof domain_header);
    uint32_t cmif_header[4] = {0x49434653u, 0, command, 0};
    memcpy((uint8_t *)message + payload_offset + 16, cmif_header, sizeof cmif_header);
    if (args_size) memcpy((uint8_t *)message + payload_offset + 32, args, args_size);
}

static uint32_t response_result(uint32_t *message, int domain) {
    return ((uint32_t *)((uint8_t *)message + 16))[domain ? 6 : 2];
}

static uint32_t open_olsc(uint32_t *message);

static uint32_t open_and_convert(uint32_t *message) {
    uint32_t service = open_olsc(message);
    make_control(message, 0);
    send_frame(service, message);
    assert(response_result(message, 0) == 0);
    assert(*(uint32_t *)((uint8_t *)message + 32) == 1);
    return service;
}

static void send_init(uint32_t service, uint32_t *message, uint64_t argument, int has_pid) {
    make_domain(message, 0, &argument, sizeof argument, has_pid, 0, 0);
    send_frame(service, message);
}

static void send_get_setting(uint32_t service, uint32_t *message,
                             const uint64_t uid[2], int has_pid,
                             unsigned x_descriptors, unsigned copy_handles) {
    make_domain(message, 13, uid, 16, has_pid, x_descriptors, copy_handles);
    send_frame(service, message);
}

static void send_set_setting(uint32_t service, uint32_t *message, uint8_t enabled, const uint64_t uid[2]) {
    uint8_t args[24] = {0};
    args[0] = enabled;
    memcpy(args + 8, uid, 16);
    make_domain(message, 14, args, sizeof args, 0, 0, 0);
    send_frame(service, message);
}

static uint32_t open_olsc(uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    const char service_name[8] = {'o','l','s','c',':','u',0,0};
    make_cmif(message, 4, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

int main(int argc, char **argv) {
#ifdef GAME_ACNH
    uint32_t message[64];
    const char *mode = argc > 1 ? argv[1] : "valid";
    const uint64_t uid[2] = {0x1122334455667788ULL, 0x8877665544332211ULL};
    if (!strcmp(mode, "valid")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        send_init(olsc, message, 0, 1); /* 本地幂等重复初始化 */
        send_get_setting(olsc, message, uid, 0, 0, 0);
        assert(response_result(message, 1) == 0);
        assert(*((uint8_t *)message + 48) == 0);
        runtime_ipc_session_closed(runtime_handle_get(olsc, 1));
        return 0;
    }
    if (!strcmp(mode, "uninitialized")) {
        uint32_t olsc = open_and_convert(message);
        send_get_setting(olsc, message, uid, 0, 0, 0);
        assert(!"GetSetting before Initialize must fail");
    }
    if (!strcmp(mode, "reopened-session")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        runtime_ipc_session_closed(runtime_handle_get(olsc, 1));
        olsc = open_and_convert(message);
        send_get_setting(olsc, message, uid, 0, 0, 0);
        assert(!"new session must start uninitialized");
    }
    if (!strcmp(mode, "wrong-uid")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        const uint64_t other_uid[2] = {0x53454e54494e454cULL, 0x5052495641544500ULL};
        send_get_setting(olsc, message, other_uid, 0, 0, 0);
        assert(!"nonlocal UID must fail");
    }
    if (!strcmp(mode, "bad-init-argument")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 1, 1);
        assert(!"nonzero Initialize argument must fail");
    }
    if (!strcmp(mode, "bad-init-shape")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 0);
        assert(!"Initialize without PID must fail");
    }
    if (!strcmp(mode, "get-setting-with-pid")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        send_get_setting(olsc, message, uid, 1, 0, 0);
        assert(!"GetSetting with PID must fail");
    }
    if (!strcmp(mode, "get-setting-with-buffer")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        send_get_setting(olsc, message, uid, 0, 1, 0);
        assert(!"GetSetting with a buffer must fail");
    }
    if (!strcmp(mode, "get-setting-with-handle")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        send_get_setting(olsc, message, uid, 0, 0, 1);
        assert(!"GetSetting with a handle must fail");
    }
    if (!strcmp(mode, "set-setting")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        send_set_setting(olsc, message, 0, uid);
        assert(response_result(message, 1) == 0);
        send_get_setting(olsc, message, uid, 0, 0, 0);
        assert(response_result(message, 1) == 0 && *((uint8_t *)message + 48) == 0);
        send_set_setting(olsc, message, 1, uid);
        assert(response_result(message, 1) == 0);
        send_get_setting(olsc, message, uid, 0, 0, 0);
        assert(response_result(message, 1) == 0 && *((uint8_t *)message + 48) == 1);
        runtime_ipc_session_closed(runtime_handle_get(olsc, 1));
        return 0;
    }
    if (!strcmp(mode, "set-setting-wrong-uid")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        const uint64_t other_uid[2] = {0x53454e54494e454cULL, 0x5052495641544500ULL};
        send_set_setting(olsc, message, 0, other_uid);
        assert(!"nonlocal UID must fail");
    }
    if (!strcmp(mode, "set-setting-bad-value")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        send_set_setting(olsc, message, 2, uid);
        assert(!"non-bool value must fail");
    }
    if (!strcmp(mode, "set-setting-uninitialized")) {
        uint32_t olsc = open_and_convert(message);
        send_set_setting(olsc, message, 0, uid);
        assert(!"SetSettingEnabled before Initialize must fail");
    }
    if (!strcmp(mode, "set-setting-bad-shape")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        make_domain(message, 14, NULL, 0, 0, 0, 0);
        send_frame(olsc, message);
        assert(!"SetSettingEnabled without arguments must fail");
    }
    if (!strcmp(mode, "unknown-command")) {
        uint32_t olsc = open_and_convert(message);
        send_init(olsc, message, 0, 1);
        make_domain(message, 15, NULL, 0, 0, 0, 0);
        send_frame(olsc, message);
        assert(!"unimplemented command must fail");
    }
    assert(!"unknown probe mode");
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
    return 0;
}
