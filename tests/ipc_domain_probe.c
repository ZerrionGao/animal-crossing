#include <assert.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/runtime/ipc.h"
#include "../engine/runtime/runtime.h"

pthread_key_t runtime_keys[8];
static void *handles[256];
static uint32_t next_handle = 0x1000;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int handler_entered, handler_continue, close_count;
static int root_close_count, child_close_count;
static int require_pid;
static jmp_buf fail_jump;
static int catch_runtime_fail;
typedef struct { int used; int signaled; unsigned refs; int writer_open; unsigned signal_count; } FakeEvent;
typedef struct { uint32_t handle; FakeEvent *event; int writer; int used; } FakeEventHandle;
static FakeEvent fake_events[32];
static FakeEventHandle fake_event_handles[256];
static uint32_t next_event_handle = 0x5000;
static unsigned fake_event_creates, fake_event_readables, fake_event_frees, fake_event_signals;
static FakeEventHandle *fake_event_lookup(uint32_t handle);

_Noreturn void runtime_fail(const char *message) { (void)message; if (catch_runtime_fail) longjmp(fail_jump, 1); abort(); }
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }
uint32_t runtime_handle_new(int session, void *object) { (void)session; uint32_t h = next_handle++; handles[h & 255] = object; return h; }
void *runtime_handle_get(uint32_t handle, int session) { (void)session; return handles[handle & 255]; }
void *runtime_handle_acquire_session(uint32_t handle) {
    IpcSession *s = runtime_handle_get(handle, 1);
    if (s) runtime_ipc_session_retain(s);
    return s;
}
void runtime_event_signal(uint32_t handle) {
    FakeEventHandle *writer = fake_event_lookup(handle);
    assert(writer && writer->writer && writer->event->writer_open);
    writer->event->signaled = 1;
    writer->event->signal_count++;
    fake_event_signals++;
}
static FakeEventHandle *fake_event_lookup(uint32_t handle) {
    for (unsigned i = 0; i < sizeof fake_event_handles / sizeof fake_event_handles[0]; i++)
        if (fake_event_handles[i].used && fake_event_handles[i].handle == handle) return &fake_event_handles[i];
    return NULL;
}
static void fake_event_release(FakeEvent *event) {
    assert(event->refs > 0);
    if (--event->refs == 0) { event->used = 0; fake_event_frees++; }
}
uint32_t runtime_event_new(void) {
    for (unsigned i = 0; i < sizeof fake_events / sizeof fake_events[0]; i++) if (!fake_events[i].used) {
        FakeEvent *event = &fake_events[i]; *event = (FakeEvent){.used = 1, .refs = 1, .writer_open = 1};
        for (unsigned j = 0; j < sizeof fake_event_handles / sizeof fake_event_handles[0]; j++) if (!fake_event_handles[j].used) {
            uint32_t handle = next_event_handle++;
            fake_event_handles[j] = (FakeEventHandle){handle, event, 1, 1};
            fake_event_creates++;
            return handle;
        }
        abort();
    }
    abort();
}
uint32_t runtime_event_readable(uint32_t handle) {
    FakeEventHandle *writer = fake_event_lookup(handle);
    assert(writer && writer->writer && writer->event->writer_open);
    for (unsigned j = 0; j < sizeof fake_event_handles / sizeof fake_event_handles[0]; j++) if (!fake_event_handles[j].used) {
        uint32_t readable = next_event_handle++;
        writer->event->refs++;
        fake_event_handles[j] = (FakeEventHandle){readable, writer->event, 0, 1};
        fake_event_readables++;
        return readable;
    }
    abort();
}
void runtime_event_close_writable(uint32_t handle) {
    FakeEventHandle *writer = fake_event_lookup(handle);
    assert(writer && writer->writer && writer->event->writer_open);
    FakeEvent *event = writer->event;
    event->writer_open = 0;
    writer->used = 0;
    fake_event_release(event);
}
static void fake_event_close_readable(uint32_t handle) {
    FakeEventHandle *readable = fake_event_lookup(handle);
    assert(readable && !readable->writer);
    FakeEvent *event = readable->event;
    readable->used = 0;
    fake_event_release(event);
}
static FakeEvent *fake_event_get_readable(uint32_t handle) {
    FakeEventHandle *readable = fake_event_lookup(handle);
    return readable && !readable->writer ? readable->event : NULL;
}
#ifdef GAME_ACNH
static void send_frame(uint32_t handle, uint32_t *m);
static void make_domain(uint32_t *m, uint32_t object_id, uint32_t cmd, const void *args, unsigned arg_size);
static uint8_t *response_payload(uint32_t *m);
static uint32_t response_result_with_handles(uint32_t *m, int domain);
static void nifm_domain_empty(uint32_t *m, uint32_t handle, uint32_t object, uint32_t cmd) {
    make_domain(m, object, cmd, NULL, 0);
    m[0] = 6;
    send_frame(handle, m);
}
static uint32_t nifm_create_request(uint32_t *m, uint32_t handle, uint32_t general, int32_t preset) {
    make_domain(m, general, 4, &preset, sizeof preset);
    m[0] = 6;
    send_frame(handle, m);
    assert(response_result_with_handles(m, 1) == 0);
    assert(((uint32_t *)response_payload(m))[0] == 1);
    return *(uint32_t *)(response_payload(m) + 32);
}
static uint32_t nifm_request_state(uint32_t *m, uint32_t handle, uint32_t request) {
    nifm_domain_empty(m, handle, request, 0);
    assert(response_result_with_handles(m, 1) == 0);
    return *(uint32_t *)(response_payload(m) + 32);
}
static uint32_t nifm_request_result(uint32_t *m, uint32_t handle, uint32_t request) {
    nifm_domain_empty(m, handle, request, 1);
    return response_result_with_handles(m, 1);
}
#endif

static uint32_t open_service(const char *name) {
    uint64_t r[31] = {0};
    r[1] = (uintptr_t)name;
    assert(runtime_ipc_svc(0x1f, r));
    return (uint32_t)r[1];
}

static void send_frame(uint32_t handle, uint32_t *m) {
    uint64_t r[31] = {0};
    r[0] = (uintptr_t)m; r[1] = 0x100; r[2] = handle;
    assert(runtime_ipc_svc(0x22, r));
    assert(r[0] == 0);
}

static void make_cmif(uint32_t *m, unsigned type, uint32_t cmd, const void *args, unsigned arg_size) {
    memset(m, 0, 0x100);
    unsigned actual = 16 + 16 + arg_size;
    unsigned words = (actual + 3) / 4;
    m[0] = type; m[1] = words;
    uint32_t h[4] = {0x49434653u, 0, cmd, 0};
    memcpy((uint8_t *)m + 16, h, sizeof h);
    if (arg_size) memcpy((uint8_t *)m + 32, args, arg_size);
}
static void make_cmif_xdesc(uint32_t *m, uint32_t cmd, const void *args, unsigned arg_size) {
    make_cmif(m, 4, cmd, args, arg_size);
    m[0] |= 1u << 16;
}
static void make_cmif_pid(uint32_t *m, uint32_t cmd, const void *args, unsigned arg_size) {
    uint8_t payload[0x100];
    make_cmif(m, 4, cmd, args, arg_size);
    memcpy(payload, (uint8_t *)m + 16, 16 + arg_size);
    memset((uint8_t *)m + 16, 0, 0x100 - 16);
    m[1] |= 1u << 31;
    m[2] = 1; /* special header 含 PID */
    *(uint64_t *)((uint8_t *)m + 12) = 0x1122334455667788ull;
    memcpy((uint8_t *)m + 32, payload, 16 + arg_size);
}

static void make_control(uint32_t *m, uint32_t cmd) { make_cmif(m, 7, cmd, NULL, 0); }

static void make_domain(uint32_t *m, uint32_t object_id, uint32_t cmd, const void *args, unsigned arg_size) {
    memset(m, 0, 0x100);
    unsigned domain_data_size = 16 + arg_size;
    unsigned actual = 16 + 16 + domain_data_size;
    m[0] = 4; m[1] = (actual + 3) / 4;
    uint8_t dh[16] = {1, 0};
    uint16_t ds = (uint16_t)domain_data_size;
    memcpy(dh + 2, &ds, 2); memcpy(dh + 4, &object_id, 4);
    memcpy((uint8_t *)m + 16, dh, 16);
    uint32_t h[4] = {0x49434653u, 0, cmd, 0};
    memcpy((uint8_t *)m + 32, h, sizeof h);
    if (arg_size) memcpy((uint8_t *)m + 48, args, arg_size);
}

static void make_domain_special(uint32_t *m, uint32_t object_id, uint32_t cmd) {
    make_domain(m, object_id, cmd, NULL, 0);
    m[1] |= 1u << 31;
    m[2] = 0; /* 合法的空 special header；raw data 因此从偏移 12 开始。 */
}
#ifdef GAME_ACNH
static void make_domain_pid(uint32_t *m, uint32_t object_id, uint32_t cmd, const void *args, unsigned arg_size) {
    make_domain(m, object_id, cmd, args, arg_size);
    uint8_t payload[0x100];
    unsigned domain_data_size = 16 + arg_size;
    memcpy(payload, (uint8_t *)m + 16, 16 + domain_data_size);
    memset((uint8_t *)m + 16, 0, 0x100 - 16);
    m[1] |= 1u << 31;
    m[2] = 1;
    *(uint64_t *)((uint8_t *)m + 12) = 0x1122334455667788ull;
    memcpy((uint8_t *)m + 32, payload, 16 + domain_data_size);
}
#endif
static void make_domain_xdesc(uint32_t *m, uint32_t object_id, uint32_t cmd) {
    make_domain(m, object_id, cmd, NULL, 0);
    m[0] |= 1u << 16; /* 一个零长度 X descriptor，raw data 从偏移 16 开始。 */
    m[2] = 0; m[3] = 0;
}

#ifdef GAME_ACNH
static void make_domain_bdesc(uint32_t *m, uint32_t object_id, uint32_t cmd, const void *args, unsigned arg_size) {
    make_domain(m, object_id, cmd, args, arg_size);
    uint8_t payload[0x100];
    unsigned payload_size = 16 + 16 + arg_size;
    memcpy(payload, (uint8_t *)m + 16, payload_size);
    memset((uint8_t *)m + 8, 0, 0x100 - 8);
    m[0] |= 1u << 24; /* 一个零长度 B descriptor，raw data 从偏移 20 开始，载荷对齐到 32。 */
    m[1] = (16u + payload_size + 3u) / 4u;
    memcpy((uint8_t *)m + 32, payload, payload_size);
}
#endif
static void make_domain_close(uint32_t *m, uint32_t object_id) {
    memset(m, 0, 0x100);
    m[0] = 4; m[1] = 8;
    uint8_t dh[16] = {2, 0}; memcpy(dh + 4, &object_id, 4);
    memcpy((uint8_t *)m + 16, dh, sizeof dh);
}
static void make_domain_close_xdesc(uint32_t *m, uint32_t object_id) {
    make_domain_close(m, object_id);
    m[0] |= 1u << 16;
    m[2] = 0; m[3] = 0;
}
static uint8_t *response_payload(uint32_t *m) { return (uint8_t *)m + 16; }
static uint32_t response_result(uint32_t *m, int domain) {
    return ((uint32_t *)response_payload(m))[domain ? 6 : 2];
}
#ifdef GAME_ACNH
static uint8_t *response_payload_with_handles(uint32_t *m) {
    size_t offset = 8;
    if (m[1] >> 31) {
        uint32_t special = m[2];
        offset += 4u + 4u * (((special >> 1) & 0xfu) + ((special >> 5) & 0xfu));
    }
    offset += ((m[0] >> 16) & 0xfu) * 8u;
    offset = (offset + 15u) & ~(size_t)15u;
    return (uint8_t *)m + offset;
}
static uint32_t response_result_with_handles(uint32_t *m, int domain) {
    uint32_t *cmif = (uint32_t *)(response_payload_with_handles(m) + (domain ? 16 : 0));
    assert(cmif[0] == 0x4f434653u);
    return cmif[2];
}
static void test_response_result_after_copy_handles(void) {
    uint32_t m[64] = {0};
    m[1] = 1u << 31;
    m[2] = 4; /* special header with two copied handles */
    uint32_t *domain = (uint32_t *)((uint8_t *)m + 32);
    uint32_t cmif[4] = {0x4f434653u, 0, 0x12345678u, 0};
    memcpy(domain + 4, cmif, sizeof cmif);
    assert(response_result(m, 1) == 0); /* padding that the old fixed-offset parser mistook for Result */
    assert(response_result_with_handles(m, 1) == 0x12345678u);
}
#endif

static void slow_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)session; (void)response;
    assert(request->cmd == 9);
    pthread_mutex_lock(&gate);
    handler_entered = 1; pthread_cond_broadcast(&changed);
    while (!handler_continue) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
static void owner_close(void *state) { (void)state; __atomic_add_fetch(&close_count, 1, __ATOMIC_SEQ_CST); }
static void root_close(void *state) { (void)state; root_close_count++; }
static void child_close(void *state) { (void)state; child_close_count++; }
typedef struct { uint32_t writable[2]; } FakeEventOwner;
static void fake_event_owner_close(void *opaque) {
    FakeEventOwner *state = opaque;
    runtime_event_close_writable(state->writable[0]);
    runtime_event_close_writable(state->writable[1]);
    free(state);
}
static void empty_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)session; (void)response;
    if (request->cmd == 5) ipc_out_object(response, "probe-child", slow_handler, NULL, child_close);
    else if (request->cmd != 9) abort();
}
static void argument_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)session; (void)response;
    assert(request->cmd == 7);
    assert(request->has_pid == require_pid);
    assert(IPC_ARG(request, uint32_t, 0) == 0x12345678u);
}
static void *slow_call(void *arg) {
    uint32_t m[64]; make_cmif(m, 4, 9, NULL, 0); send_frame((uint32_t)(uintptr_t)arg, m); return NULL;
}

int main(int argc, char **argv) {
    uint32_t m[64];
#ifdef GAME_ACNH
    if (argc == 2 && strncmp(argv[1], "factory-", 8) == 0) {
        test_response_result_after_copy_handles();
        uint32_t sm = open_service("sm:");
        make_cmif(m, 4, 1, "nifm:u", 8); send_frame(sm, m);
        uint32_t static_handle = ((uint32_t *)m)[3];
        assert(static_handle != 0);
        make_control(m, 3); send_frame(static_handle, m);
        assert(response_result(m, 0) == 0);
        assert(*(uint16_t *)(response_payload(m) + 16) == 0x8000);
        make_control(m, 0); send_frame(static_handle, m);
        assert(response_result(m, 0) == 0);
        make_domain(m, 1, 5, NULL, 0); m[0] = 6; send_frame(static_handle, m);
        assert(response_result(m, 1) == 0);
        assert(((uint32_t *)response_payload(m))[0] == 1);
        uint32_t general_id = *(uint32_t *)(response_payload(m) + 32);
        assert(general_id == 2);

        int32_t preset = 2;
        if (strcmp(argv[1], "factory-preset") == 0) preset = 3;
        if (strcmp(argv[1], "factory-shape") == 0) {
            make_domain_pid(m, general_id, 4, &preset, sizeof preset);
            m[0] = 6;
            send_frame(static_handle, m);
            abort();
        }
        if (strcmp(argv[1], "factory-child-shape") == 0) {
            make_domain(m, general_id, 4, &preset, sizeof preset); m[0] = 6; send_frame(static_handle, m);
            assert(response_result(m, 1) == 0);
            uint32_t child_id = *(uint32_t *)(response_payload(m) + 32);
            assert(child_id == 3 && fake_event_creates == 2);
            uint32_t extra = 0;
            make_domain(m, child_id, 2, &extra, sizeof extra); m[0] = 6;
            catch_runtime_fail = 1;
            if (!setjmp(fail_jump)) { send_frame(static_handle, m); abort(); }
            catch_runtime_fail = 0;
            assert(fake_event_creates == 2);
            return 0;
        }
        if (strcmp(argv[1], "factory-unknown") == 0 || strcmp(argv[1], "factory-valid") == 0 ||
            strcmp(argv[1], "factory-submit") == 0 || strcmp(argv[1], "factory-submit-option") == 0 ||
            strncmp(argv[1], "factory-applet-info", 19) == 0 ||
            strncmp(argv[1], "factory-lifecycle-shape-", 24) == 0 ||
            strncmp(argv[1], "factory-option-", 15) == 0) {
            make_domain(m, general_id, 4, &preset, sizeof preset); m[0] = 6; send_frame(static_handle, m);
            assert(response_result(m, 1) == 0);
            assert(((uint32_t *)response_payload(m))[0] == 1);
            uint32_t child_id = *(uint32_t *)(response_payload(m) + 32);
            assert(child_id == 3);
            if (strcmp(argv[1], "factory-submit") == 0 || strcmp(argv[1], "factory-submit-option") == 0) {
                if (strcmp(argv[1], "factory-submit-option") == 0) {
                    uint8_t option = 1;
                    make_domain(m, child_id, 11, &option, sizeof option); m[0] = 6; send_frame(static_handle, m);
                    assert(response_result_with_handles(m, 1) == 0);
                    nifm_domain_empty(m, static_handle, child_id, 4);
                    abort();
                }
                uint32_t second_id = nifm_create_request(m, static_handle, general_id, 2);
                assert(second_id == 4 && fake_event_creates == 4);
                assert(nifm_request_state(m, static_handle, child_id) == 1);
                assert(nifm_request_state(m, static_handle, second_id) == 1);
                assert(nifm_request_result(m, static_handle, child_id) == 0x26e6eu);
                assert(nifm_request_result(m, static_handle, second_id) == 0x26e6eu);
                nifm_domain_empty(m, static_handle, child_id, 2);
                assert(response_result_with_handles(m, 1) == 0 && ((uint32_t *)m)[2] == 4);
                uint32_t first_read = ((uint32_t *)m)[3], second_read = ((uint32_t *)m)[4];
                nifm_domain_empty(m, static_handle, second_id, 2);
                assert(response_result_with_handles(m, 1) == 0 && ((uint32_t *)m)[2] == 4);
                uint32_t other_first_read = ((uint32_t *)m)[3], other_second_read = ((uint32_t *)m)[4];
                FakeEvent *first_event = fake_event_get_readable(first_read);
                FakeEvent *second_event = fake_event_get_readable(second_read);
                FakeEvent *other_first_event = fake_event_get_readable(other_first_read);
                FakeEvent *other_second_event = fake_event_get_readable(other_second_read);
                assert(first_event && second_event && other_first_event && other_second_event);
                assert(!first_event->signaled && !second_event->signaled &&
                       !other_first_event->signaled && !other_second_event->signaled);
                uint8_t option = 4;
                make_domain(m, child_id, 11, &option, sizeof option); m[0] = 6; send_frame(static_handle, m);
                assert(response_result_with_handles(m, 1) == 0);
                assert(fake_event_signals == 0);
                nifm_domain_empty(m, static_handle, child_id, 4);
                assert(response_result_with_handles(m, 1) == 0x8986eu);
                assert(fake_event_signals == 1 && fake_event_creates == 4);
                assert(first_event->signaled && first_event->signal_count == 1 && !second_event->signaled);
                assert(!other_first_event->signaled && !other_second_event->signaled);
                assert(nifm_request_state(m, static_handle, child_id) == 1);
                assert(nifm_request_result(m, static_handle, child_id) == 0x8986eu);
                assert(nifm_request_result(m, static_handle, second_id) == 0x26e6eu);
                nifm_domain_empty(m, static_handle, child_id, 2);
                assert(response_result_with_handles(m, 1) == 0 && ((uint32_t *)m)[2] == 4);
                first_read = ((uint32_t *)m)[3]; second_read = ((uint32_t *)m)[4];
                first_event = fake_event_get_readable(first_read);
                second_event = fake_event_get_readable(second_read);
                assert(first_event && second_event && first_event != second_event);
                assert(first_event->signaled && first_event->signal_count == 1 && !second_event->signaled);
                assert(fake_event_creates == 4);
                nifm_domain_empty(m, static_handle, child_id, 4);
                assert(response_result_with_handles(m, 1) == 0x8986eu);
                assert(fake_event_signals == 2 && first_event->signal_count == 2 && second_event->signal_count == 0);
                nifm_domain_empty(m, static_handle, child_id, 2);
                assert(response_result_with_handles(m, 1) == 0 && ((uint32_t *)m)[2] == 4);
                first_read = ((uint32_t *)m)[3]; second_read = ((uint32_t *)m)[4];
                first_event = fake_event_get_readable(first_read);
                second_event = fake_event_get_readable(second_read);
                assert(first_event && second_event && first_event->signal_count == 2 && second_event->signal_count == 0);
                return 0;
            }
            if (strncmp(argv[1], "factory-lifecycle-shape-", 24) == 0) {
                uint32_t cmd = (uint32_t)(argv[1][24] - '0');
                if (cmd == 0) make_domain(m, child_id, cmd, NULL, 0);
                else if (cmd == 1) make_domain(m, child_id, cmd, NULL, 0);
                else make_domain(m, child_id, cmd, NULL, 0);
                m[0] = 6;
                if (argv[1][25] == 'x') { m[0] |= 1u << 16; m[2] = 0; m[3] = 0; }
                if (argv[1][25] == 'p') make_domain_pid(m, child_id, cmd, NULL, 0), m[0] = 6;
                catch_runtime_fail = 1;
                if (!setjmp(fail_jump)) { send_frame(static_handle, m); abort(); }
                catch_runtime_fail = 0;
                return 0;
            }
            if (strcmp(argv[1], "factory-unknown") == 0) {
                make_domain(m, child_id, 3, NULL, 0); m[0] = 6; send_frame(static_handle, m);
                abort();
            }
            if (strcmp(argv[1], "factory-applet-info") == 0) {
                uint32_t theme = 0;
                make_domain_bdesc(m, child_id, 21, &theme, sizeof theme); m[0] = (m[0] & ~0xffffu) | 6;
                send_frame(static_handle, m);
                assert(response_result_with_handles(m, 1) == 0x26e6eu);
                uint8_t option = 4;
                make_domain(m, child_id, 11, &option, sizeof option); m[0] = 6; send_frame(static_handle, m);
                assert(response_result_with_handles(m, 1) == 0);
                nifm_domain_empty(m, static_handle, child_id, 4);
                assert(response_result_with_handles(m, 1) == 0x8986eu);
                make_domain_bdesc(m, child_id, 21, &theme, sizeof theme); m[0] = (m[0] & ~0xffffu) | 6;
                send_frame(static_handle, m);
                assert(response_result_with_handles(m, 1) == 0x8986eu);
                return 0;
            }
            if (strcmp(argv[1], "factory-applet-info-shape") == 0) {
                uint32_t theme = 0;
                make_domain(m, child_id, 21, &theme, sizeof theme); m[0] = 6;
                catch_runtime_fail = 1;
                if (!setjmp(fail_jump)) { send_frame(static_handle, m); abort(); }
                catch_runtime_fail = 0;
                return 0;
            }
            if (strncmp(argv[1], "factory-option-", 15) == 0) {
                uint8_t option = 4;
                if (strcmp(argv[1], "factory-option-invalid-0") == 0) option = 0;
                if (strcmp(argv[1], "factory-option-invalid-6") == 0) option = 6;
                if (strcmp(argv[1], "factory-option-invalid-255") == 0) option = 255;
                make_domain(m, child_id, 11, &option, sizeof option); m[0] = 6;
                if (strcmp(argv[1], "factory-option-type") == 0) m[0] = 4;
                if (strcmp(argv[1], "factory-option-data") == 0) {
                    uint16_t data = 4;
                    make_domain(m, child_id, 11, &data, sizeof data); m[0] = 6;
                }
                if (strcmp(argv[1], "factory-option-token") == 0) *(uint32_t *)((uint8_t *)m + 28) = 1;
                if (strcmp(argv[1], "factory-option-pid") == 0) {
                    make_domain_pid(m, child_id, 11, &option, sizeof option); m[0] = 6;
                }
                if (strcmp(argv[1], "factory-option-desc") == 0) {
                    m[0] |= 1u << 16;
                    m[2] = 0;
                    m[3] = 0;
                }
                catch_runtime_fail = 1;
                if (!setjmp(fail_jump)) { send_frame(static_handle, m); abort(); }
                catch_runtime_fail = 0;
                assert(fake_event_creates == 2 && fake_event_readables == 0 && fake_event_signals == 0);
                return 0;
            }
            make_domain(m, child_id, 2, NULL, 0); m[0] = 6; send_frame(static_handle, m);
            assert(response_result_with_handles(m, 1) == 0 && ((uint32_t *)m)[2] == 4);
            uint32_t read0 = ((uint32_t *)m)[3], read1 = ((uint32_t *)m)[4];
            assert(read0 && read1 && read0 != read1);
            make_domain(m, child_id, 2, NULL, 0); m[0] = 6; send_frame(static_handle, m);
            assert(response_result_with_handles(m, 1) == 0 && ((uint32_t *)m)[2] == 4);
            uint32_t read2 = ((uint32_t *)m)[3], read3 = ((uint32_t *)m)[4];
            assert(read2 && read3 && read2 != read3 && read0 != read2 && read1 != read3);
            assert(fake_event_get_readable(read0) == fake_event_get_readable(read2));
            assert(fake_event_get_readable(read1) == fake_event_get_readable(read3));
            assert(fake_event_get_readable(read0) != fake_event_get_readable(read1));
            assert(fake_event_signals == 0 && fake_event_creates == 2);
            unsigned created_before_options = fake_event_creates;
            unsigned readable_before_options = fake_event_readables;
            const uint8_t options[] = {1, 2, 3, 4, 5, 4};
            for (unsigned i = 0; i < sizeof options; i++) {
                make_domain(m, child_id, 11, &options[i], sizeof options[i]); m[0] = 6;
                send_frame(static_handle, m);
                assert(response_result(m, 1) == 0);
                assert(fake_event_creates == created_before_options && fake_event_readables == readable_before_options && fake_event_signals == 0);
            }
            make_domain_close(m, child_id); send_frame(static_handle, m);
            assert(response_result(m, 1) == 0);
            uint32_t reads[4] = {read0, read1, read2, read3};
            assert(fake_event_get_readable(read0) && fake_event_get_readable(read1));
            assert(fake_event_get_readable(read0)->writer_open == 0 && fake_event_get_readable(read1)->writer_open == 0);
            make_domain(m, child_id, 2, NULL, 0); m[0] = 6; send_frame(static_handle, m);
            assert(response_result(m, 1) == 0xf402u);
            for (unsigned i = 0; i < sizeof reads / sizeof reads[0]; i++) fake_event_close_readable(reads[i]);
            assert(fake_event_frees == 2);
            make_domain_close(m, general_id); send_frame(static_handle, m);
            make_domain_close(m, 1); send_frame(static_handle, m);
            runtime_ipc_session_closed(runtime_handle_get(static_handle, 1));
            runtime_ipc_session_closed(runtime_handle_get(sm, 1));
            return 0;
        }
        make_domain(m, general_id, 4, &preset, sizeof preset); m[0] = 6;
        if (strcmp(argv[1], "factory-type") == 0) m[0] = 4;
        if (strcmp(argv[1], "factory-data") == 0) {
            uint64_t long_args = 2;
            make_domain(m, general_id, 4, &long_args, sizeof long_args); m[0] = 6;
        }
        if (strcmp(argv[1], "factory-token") == 0) *(uint32_t *)((uint8_t *)m + 28) = 1;
        if (strcmp(argv[1], "factory-desc") == 0) {
            m[0] |= 1u << 16;
            m[2] = 0;
            m[3] = 0;
        }
        send_frame(static_handle, m);
        abort();
    }
#else
    (void)argc;
    (void)argv;
#endif
    uint32_t nifm = open_service("sm:");
    make_cmif(m, 4, 1, "nifm:u", 8); send_frame(nifm, m);
    uint32_t nifm_handle = ((uint32_t *)m)[3];
    assert(nifm_handle != 0);

    /* 普通 CMIF 的 cmd 5 仍返回独立 session move handle。 */
    make_cmif(m, 4, 5, NULL, 0); send_frame(nifm_handle, m);
    uint32_t general_handle = ((uint32_t *)m)[3];
    assert(general_handle != 0);
    make_cmif(m, 4, 18, NULL, 0); send_frame(general_handle, m);
    assert(response_result(m, 0) == 0 && response_payload(m)[32] == 0 && response_payload(m)[33] == 0 && response_payload(m)[34] == 0);
    runtime_ipc_session_closed(runtime_handle_get(general_handle, 1));

    /* CMIF 普通参数长度是读取上界；带 PID / X descriptor 的四字节参数完整可读。 */
    uint32_t args = 0x12345678u;
    uint32_t arg_handle = runtime_ipc_new_session("arg", argument_handler, NULL, NULL);
    require_pid = 0; make_cmif_xdesc(m, 7, &args, sizeof args); send_frame(arg_handle, m);
    require_pid = 1; make_cmif_pid(m, 7, &args, sizeof args); send_frame(arg_handle, m);
    runtime_ipc_session_closed(runtime_handle_get(arg_handle, 1));

    /* 转 domain、cmd 5 建 object、查询、关闭子对象与 root。 */
    make_control(m, 0); send_frame(nifm_handle, m);
    assert(response_result(m, 0) == 0 && *(uint32_t *)(response_payload(m) + 16) == 1);
    make_domain_special(m, 1, 5); send_frame(nifm_handle, m);
    assert(response_result(m, 1) == 0 && ((uint32_t *)response_payload(m))[0] == 1);
    assert(*(uint32_t *)(response_payload(m) + 32) == 2);
    make_domain_xdesc(m, 2, 18); send_frame(nifm_handle, m);
    assert(response_result(m, 1) == 0 && response_payload(m)[32] == 0 && response_payload(m)[33] == 0 && response_payload(m)[34] == 0);
    make_domain_close_xdesc(m, 2); send_frame(nifm_handle, m);
    assert(response_result(m, 1) == 0);
    make_domain(m, 2, 18, NULL, 0); send_frame(nifm_handle, m);
    assert(response_result(m, 1) == 0xf402u);
    make_domain_close(m, 1); m[1] |= 1u << 31; m[2] = 0; send_frame(nifm_handle, m);
    assert(response_result(m, 1) == 0);
    runtime_ipc_session_closed(runtime_handle_get(nifm_handle, 1));

    /* 畸形 domain 长度、未知 ID 与未知类型均返回错误，不越界访问。 */
    nifm = runtime_ipc_new_session("nifm:u", empty_handler, NULL, NULL);
    make_control(m, 0); send_frame(nifm, m);
    make_domain(m, 1, 5, NULL, 0); *(uint16_t *)((uint8_t *)m + 18) = 0xffff;
    send_frame(nifm, m); assert(response_result(m, 1) == 0xf401u);
    make_domain(m, 99, 18, NULL, 0); send_frame(nifm, m); assert(response_result(m, 1) == 0xf402u);
    make_domain(m, 1, 5, NULL, 0); *((uint8_t *)m + 16) = 3;
    send_frame(nifm, m); assert(response_result(m, 1) == 0xf401u);
    make_domain(m, 1, 5, NULL, 0); *((uint8_t *)m + 17) = 1;
    send_frame(nifm, m); assert(response_result(m, 1) == 0xf401u);
    make_domain(m, 1, 5, NULL, 0); *((uint32_t *)((uint8_t *)m + 24)) = 1;
    send_frame(nifm, m); assert(response_result(m, 1) == 0xf401u);
    runtime_ipc_session_closed(runtime_handle_get(nifm, 1));

    /* domain 子对象 close 与 session/root close 各释放自己的状态且恰好一次。 */
    root_close_count = child_close_count = 0;
    uint32_t domain_owner = runtime_ipc_new_session("probe", empty_handler, NULL, root_close);
    make_control(m, 0); send_frame(domain_owner, m);
    make_domain(m, 1, 5, NULL, 0); send_frame(domain_owner, m);
    assert(response_result(m, 1) == 0 && *(uint32_t *)(response_payload(m) + 32) == 2);
    make_domain_close(m, 2); send_frame(domain_owner, m);
    assert(child_close_count == 1 && root_close_count == 0);
    make_domain_close(m, 1); send_frame(domain_owner, m);
    assert(root_close_count == 0);
    runtime_ipc_session_closed(runtime_handle_get(domain_owner, 1));
    assert(root_close_count == 1 && child_close_count == 1);

    /* 普通 Clone/CloneEx 保留同一状态，最后一个 session close 才运行析构。 */
    close_count = 0;
    uint32_t clone_owner = runtime_ipc_new_session("probe", empty_handler, NULL, owner_close);
    make_control(m, 2); send_frame(clone_owner, m); uint32_t clone2 = ((uint32_t *)m)[3];
    make_control(m, 4); send_frame(clone2, m); uint32_t clone4 = ((uint32_t *)m)[3];
    assert(clone2 && clone4);
    runtime_ipc_session_closed(runtime_handle_get(clone_owner, 1));
    runtime_ipc_session_closed(runtime_handle_get(clone2, 1));
    assert(close_count == 0);
    runtime_ipc_session_closed(runtime_handle_get(clone4, 1));
    assert(close_count == 1);

    /* Clones share one owner: service event write ends close only after the final session reference. */
    FakeEventOwner *event_owner = calloc(1, sizeof *event_owner); assert(event_owner);
    event_owner->writable[0] = runtime_event_new(); event_owner->writable[1] = runtime_event_new();
    uint32_t event_read0 = runtime_event_readable(event_owner->writable[0]);
    uint32_t event_read1 = runtime_event_readable(event_owner->writable[1]);
    uint32_t event_session = runtime_ipc_new_session("event-owner", empty_handler, event_owner, fake_event_owner_close);
    make_control(m, 2); send_frame(event_session, m); uint32_t event_clone2 = ((uint32_t *)m)[3];
    make_control(m, 4); send_frame(event_clone2, m); uint32_t event_clone4 = ((uint32_t *)m)[3];
    runtime_ipc_session_closed(runtime_handle_get(event_session, 1));
    runtime_ipc_session_closed(runtime_handle_get(event_clone2, 1));
    assert(fake_event_get_readable(event_read0)->writer_open && fake_event_get_readable(event_read1)->writer_open);
    runtime_ipc_session_closed(runtime_handle_get(event_clone4, 1));
    assert(!fake_event_get_readable(event_read0)->writer_open && !fake_event_get_readable(event_read1)->writer_open);
    assert(fake_event_get_readable(event_read0) && fake_event_get_readable(event_read1));
    fake_event_close_readable(event_read0); fake_event_close_readable(event_read1);

    /* 并发 close 只释放拥有句柄；in-flight dispatch 的 pin 保持 session 存活。 */
    close_count = 0; handler_entered = handler_continue = 0;
    uint32_t slow = runtime_ipc_new_session("probe", slow_handler, NULL, owner_close);
    pthread_t thread; assert(!pthread_create(&thread, NULL, slow_call, (void *)(uintptr_t)slow));
    pthread_mutex_lock(&gate);
    while (!handler_entered) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    runtime_ipc_session_closed(runtime_handle_get(slow, 1));
    assert(close_count == 0);
    pthread_mutex_lock(&gate); handler_continue = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    assert(!pthread_join(thread, NULL));
    assert(close_count == 1);
    return 0;
}
