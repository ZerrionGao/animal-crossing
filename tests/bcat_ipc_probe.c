#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/runtime/ipc.h"
#include "../engine/runtime/runtime.h"

void *runtime_bcat_override(const char *name);

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

static void make_domain_factory(uint32_t *message, uint32_t object_id, int has_pid, unsigned data_size) {
    memset(message, 0, 0x100);
    uint16_t domain_data_size = (uint16_t)(16 + data_size);
    uint32_t object = object_id;
    uint8_t *domain_header = (uint8_t *)message + (has_pid ? 32 : 16);
    domain_header[0] = 1;
    memcpy(domain_header + 2, &domain_data_size, sizeof domain_data_size);
    memcpy(domain_header + 4, &object, sizeof object);
    uint32_t cmif_header[4] = {0x49434653u, 0, 0, 0};
    memcpy(domain_header + 16, cmif_header, sizeof cmif_header);
    const uint8_t arg[8] = {0x90,0x87,0x76,0x65,0x54,0x43,0x32,0x21};
    if (data_size) memcpy(domain_header + 32, arg, data_size < sizeof arg ? data_size : sizeof arg);

    message[0] = has_pid ? 6 : 4;
    message[1] = (48u + data_size + 3u) / 4u;
    if (has_pid) {
        message[1] |= 1u << 31;
        message[2] = 1; /* special header 含 PID */
        const uint64_t pid_sentinel = 0x1122334455667788ull;
        memcpy((uint8_t *)message + 12, &pid_sentinel, sizeof pid_sentinel);
    }
}

static void make_domain_child_call(uint32_t *message, uint32_t object_id) {
    make_domain_factory(message, object_id, 1, 8);
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

static uint32_t open_bcat(uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    const char service_name[8] = {'b','c','a','t',':','u',0,0};
    make_cmif(message, 4, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "success";
    expected_failure = strcmp(mode, "success") != 0;
    if (strcmp(mode, "mount-result") == 0) {
        typedef unsigned (*MountDeliveryCacheStorage)(void);
        MountDeliveryCacheStorage mount =
            (MountDeliveryCacheStorage)runtime_bcat_override("_ZN2nn4bcat25MountDeliveryCacheStorageEv");
        assert(mount);
        assert(runtime_bcat_override("_ZN2nn4bcat25MountDeliveryCacheStorageEv") == (void *)mount);
        assert(runtime_bcat_override("_ZN2nn4bcat18UnknownCacheMethodEv") == NULL);
        assert(mount() == 0x47au);
        assert(mount() == 0x47au);
        return 0;
    }
    uint32_t message[64];
    uint32_t bcat = open_bcat(message);

    make_control(message, 0);
    send_frame(bcat, message);
    assert(((uint32_t *)((uint8_t *)message + 16))[2] == 0);

    if (strcmp(mode, "malformed-root") == 0) {
        make_domain_factory(message, 1, 0, 8);
        send_frame(bcat, message);
        assert(!"CreateBcatService without its observed PID shape must fail");
    }
    if (strcmp(mode, "malformed-size") == 0) {
        make_domain_factory(message, 1, 1, 4);
        send_frame(bcat, message);
        assert(!"CreateBcatService with a non-eight-byte argument must fail");
    }

    make_domain_factory(message, 1, 1, 8);
    send_frame(bcat, message);
    assert(domain_result(message) == 0);
    uint32_t child_id = *(uint32_t *)((uint8_t *)message + 48);
    assert(child_id == 2);

    if (strcmp(mode, "child-fail") == 0) {
        make_domain_child_call(message, child_id);
        send_frame(bcat, message);
        assert(!"unimplemented IBcatService command must fail");
    }

    make_domain_close(message, child_id);
    send_frame(bcat, message);
    assert(domain_result(message) == 0);
    make_domain_child_call(message, child_id);
    send_frame(bcat, message);
    assert(domain_result(message) == 0xf402u);

    runtime_ipc_session_closed(runtime_handle_get(bcat, 1));
    return 0;
}
