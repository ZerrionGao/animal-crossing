#define GAME_ACNH 1

#include "../engine/runtime/audout_host.c"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t last_guest_addr;
static uint64_t last_guest_size;
static unsigned guest_calls;

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    exit(42);
}

void *ipc_guest(uint64_t addr, uint64_t size) {
    last_guest_addr = addr;
    last_guest_size = size;
    guest_calls++;
    return (void *)(uintptr_t)addr;
}

void runtime_ipc_register(const char *name, IpcHandler handler, IpcOpen open, void (*close)(void *)) {
    (void)name;
    (void)handler;
    (void)open;
    (void)close;
}

uint32_t runtime_event_new(void) { return 1; }
uint32_t runtime_event_readable(uint32_t writable) { return writable + 1; }
void runtime_event_signal(uint32_t writable) { (void)writable; }
void runtime_event_close_writable(uint32_t writable) { (void)writable; }

void ipc_out(IpcResponse *response, const void *value, uint32_t size) {
    assert(response->data_size + size <= sizeof response->data);
    memcpy(response->data + response->data_size, value, size);
    response->data_size += size;
}

void ipc_out_object(IpcResponse *response, const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    (void)response;
    (void)name;
    (void)handler;
    (void)state;
    (void)close;
    runtime_fail("unexpected audout object response");
}

_Noreturn void ipc_unimplemented(const IpcSession *session, const IpcRequest *request) {
    (void)session;
    (void)request;
    runtime_fail("unexpected audout command");
}

static void need(int condition, const char *message) {
    if (!condition) runtime_fail(message);
}

static void out_init(Out *out) {
    memset(out, 0, sizeof *out);
    need(pthread_mutex_init(&out->mu, NULL) == 0, "mutex init");
    need(pthread_cond_init(&out->cv, NULL) == 0, "cond init");
    out->alive = 1;
    out->state = STATE_STARTED;
    out->rate = 48000;
    out->channels = 2;
    out->volume = 1.0f;
}

static void out_destroy(Out *out) {
    pthread_mutex_destroy(&out->mu);
    pthread_cond_destroy(&out->cv);
}

static void request_append(IpcRequest *request, uint8_t tag_data[8], uint64_t descriptor_addr) {
    memset(request, 0, sizeof *request);
    memcpy(tag_data, &(uint64_t){0x1122334455667788ull}, sizeof(uint64_t));
    request->type = 6;
    request->cmd = 3;
    request->data = tag_data;
    request->data_size = sizeof(uint64_t);
    request->na = 1;
    request->a[0] = (IpcBuffer){descriptor_addr, 0x28, 0};
}

static void descriptor(uint64_t raw[5], uint64_t data, uint64_t capacity, uint64_t size, uint64_t offset) {
    raw[0] = 0;
    raw[1] = data;
    raw[2] = capacity;
    raw[3] = size;
    raw[4] = offset;
}

static Pcm make_pcm(uint64_t tag, uint64_t data, uint64_t capacity, uint64_t size, uint64_t offset) {
    Pcm pcm = {0};
    pcm.tag = tag;
    pcm.data = data;
    pcm.size = size;
    pcm.offset = offset;
#ifdef ACNH_AUDOUT_TEST_CAPACITY
    pcm.capacity = capacity;
#else
    (void)capacity;
#endif
    return pcm;
}

static void mode_offset(void) {
    uint8_t samples[32] = {0};
    uint64_t raw[5];
    uint8_t tag_data[8];
    Out out;
    IpcSession session = {0};
    IpcRequest request;
    IpcResponse response = {0};
    out_init(&out);
    descriptor(raw, (uint64_t)(uintptr_t)samples, sizeof samples, 8, 4);
    request_append(&request, tag_data, (uint64_t)(uintptr_t)raw);
    session.state = &out;
    out_handler(&session, &request, &response);
    need(out.nq == 1, "valid offset buffer was not queued");
    guest_calls = 0;
    Pcm pcm = out.q[0];
    uint64_t frames = play_pcm(&out, &pcm);
    need(frames == 2, "data_size is the valid length");
    need(guest_calls == 1, "one guest PCM access");
    need(last_guest_addr == (uint64_t)(uintptr_t)(samples + 4), "data_offset advances the sample pointer");
    need(last_guest_size == 8, "data_size is passed as the sample length");
    out_destroy(&out);
}

static void mode_append(const char *kind) {
    uint8_t samples[32] = {0};
    uint64_t raw[5];
    uint8_t tag_data[8];
    Out out;
    IpcSession session = {0};
    IpcRequest request;
    IpcResponse response = {0};
    out_init(&out);
    session.state = &out;

    uint64_t data = (uint64_t)(uintptr_t)samples;
    if (!strcmp(kind, "bad-capacity")) descriptor(raw, data, 8, 8, 4);
    else if (!strcmp(kind, "bad-size-max")) descriptor(raw, data, 16, UINT64_MAX, 0);
    else if (!strcmp(kind, "bad-address-wrap")) descriptor(raw, UINT64_MAX - 3, 8, 8, 0);
    else if (!strcmp(kind, "bad-offset-wrap")) descriptor(raw, 8, UINT64_MAX, 0, UINT64_MAX - 3);
    else if (!strcmp(kind, "bad-frame-size")) descriptor(raw, data, 16, 6, 0);
    else if (!strcmp(kind, "bad-frame-offset")) descriptor(raw, data, 16, 4, 2);
    else if (!strcmp(kind, "full")) {
        descriptor(raw, data, sizeof samples, 8, 0);
        out.nr = RELCAP;
    } else runtime_fail("unknown append mode");

    request_append(&request, tag_data, (uint64_t)(uintptr_t)raw);
    out_handler(&session, &request, &response);
    need(!strcmp(kind, "full") ? 0 : 1, "invalid append was accepted");
    out_destroy(&out);
}

static void mode_flush(int command, int impossible) {
    Out out;
    IpcSession session = {0};
    IpcRequest request = {0};
    IpcResponse response = {0};
    out_init(&out);
    out.state = STATE_STARTED;
    out.nr = impossible ? RELCAP : RELCAP - 1;
    out.nq = 1;
    out.q[0] = make_pcm(0xaabbccddeeff0011ull, 8, sizeof(uint32_t), sizeof(uint32_t), 0);
    session.state = &out;
    request.cmd = (uint32_t)command;
    out_handler(&session, &request, &response);
    if (impossible) need(0, "Stop/Flush silently dropped a released tag");
    need(out.nq == 0 && out.nr == RELCAP, "Stop/Flush releases queued tag");
    need(out.rel[RELCAP - 1] == 0xaabbccddeeff0011ull, "released tag is retained");
    uint64_t released_tags[RELCAP] = {0};
    IpcRequest get = {0};
    IpcResponse get_response = {0};
    get.cmd = 5;
    get.nb = 1;
    get.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)released_tags, sizeof released_tags, 0};
    out_handler(&session, &get, &get_response);
    uint32_t count = 0;
    memcpy(&count, get_response.data, sizeof count);
    need(count == RELCAP && released_tags[RELCAP - 1] == 0xaabbccddeeff0011ull, "released tag is returned by GetReleased");
    out_destroy(&out);
}

int main(int argc, char **argv) {
    need(argc == 2, "one probe mode is required");
    if (!strcmp(argv[1], "offset")) mode_offset();
    else if (!strncmp(argv[1], "append-", 7)) mode_append(argv[1] + 7);
    else if (!strcmp(argv[1], "stop-near-full")) mode_flush(2, 0);
    else if (!strcmp(argv[1], "flush-near-full")) mode_flush(11, 0);
    else if (!strcmp(argv[1], "stop-full")) mode_flush(2, 1);
    else if (!strcmp(argv[1], "flush-full")) mode_flush(11, 1);
    else runtime_fail("unknown probe mode");
    puts("ACNH audout probe passed");
    return 0;
}
