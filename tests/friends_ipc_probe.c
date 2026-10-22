#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/macnativegames/games/totk/runtime/ipc.h"
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

pthread_key_t runtime_keys[8];
static void *handles[4096];
static uint32_t next_handle = 0x1000;
static int child_failure_mode;

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    if (child_failure_mode) exit(42);
    abort();
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

static void make_control(uint32_t *message, uint32_t command) {
    make_cmif(message, 7, command, NULL, 0);
}

static void make_domain(uint32_t *message, uint32_t object_id, uint32_t command,
                        const void *args, unsigned args_size) {
    memset(message, 0, 0x100);
    unsigned domain_size = 16 + args_size;
    unsigned actual = 16 + 16 + domain_size;
    message[0] = 6;
    message[1] = (actual + 3) / 4;
    uint8_t domain_header[16] = {1, 0};
    uint16_t size = (uint16_t)domain_size;
    memcpy(domain_header + 2, &size, sizeof size);
    memcpy(domain_header + 4, &object_id, sizeof object_id);
    memcpy((uint8_t *)message + 16, domain_header, sizeof domain_header);
    uint32_t cmif_header[4] = {0x49434653u, 0, command, 0};
    memcpy((uint8_t *)message + 32, cmif_header, sizeof cmif_header);
    if (args_size) memcpy((uint8_t *)message + 48, args, args_size);
}

static void make_domain_close(uint32_t *message, uint32_t object_id) {
    memset(message, 0, 0x100);
    message[0] = 4;
    message[1] = 8;
    uint8_t header[16] = {2, 0};
    memcpy(header + 4, &object_id, sizeof object_id);
    memcpy((uint8_t *)message + 16, header, sizeof header);
}

static uint32_t domain_result(uint32_t *message) {
    return ((uint32_t *)((uint8_t *)message + 16))[6];
}

static uint32_t open_friend_service(uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    const char service_name[8] = {'f','r','i','e','n','d',':','u'};
    make_cmif(message, 4, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

int main(int argc, char **argv) {
    child_failure_mode = argc > 1 &&
        (strcmp(argv[1], "child-fail") == 0 || strcmp(argv[1], "malformed-root") == 0);
    int malformed_root_mode = argc > 1 && strcmp(argv[1], "malformed-root") == 0;
    uint32_t message[64];
    uint32_t service = open_friend_service(message);

    make_control(message, 0);
    send_frame(service, message);
    assert(((uint32_t *)((uint8_t *)message + 16))[2] == 0);
    assert(*(uint32_t *)((uint8_t *)message + 32) == 1);

    make_domain(message, 1, 0, NULL, 0);
    send_frame(service, message);
    assert(domain_result(message) == 0);
    uint32_t object_id = *(uint32_t *)((uint8_t *)message + 48);
    assert(object_id == 2);

    if (malformed_root_mode) {
        uint32_t unexpected_arg = 1;
        make_domain(message, 1, 0, &unexpected_arg, sizeof unexpected_arg);
        send_frame(service, message);
        assert(!"CreateFriendService with unexpected data must fail");
    }

    if (child_failure_mode) {
        make_domain(message, object_id, 0, NULL, 0);
        send_frame(service, message);
        assert(!"unimplemented IFriendService command must fail");
    }

    make_domain_close(message, object_id);
    send_frame(service, message);
    assert(domain_result(message) == 0);
    make_domain(message, object_id, 0, NULL, 0);
    send_frame(service, message);
    assert(domain_result(message) == 0xf402u);

    runtime_ipc_session_closed(runtime_handle_get(service, 1));
    return 0;
}
