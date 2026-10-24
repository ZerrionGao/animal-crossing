#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/macnativegames/games/totk/runtime/ipc.h"
#include "../third_party/macnativegames/games/totk/runtime/ngc_aho.h"
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

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

static uint32_t open_ngc(uint32_t *message) {
    uint64_t regs[31] = {0};
    regs[1] = (uintptr_t)"sm:";
    assert(runtime_ipc_svc(0x1f, regs));
    uint32_t sm = (uint32_t)regs[1];
    const char service_name[8] = {'n','g','c',':','u',0,0,0};
    make_cmif(message, 4, 1, service_name, sizeof service_name);
    send_frame(sm, message);
    uint32_t handle = ((uint32_t *)message)[3];
    assert(handle != 0);
    runtime_ipc_session_closed(runtime_handle_get(sm, 1));
    return handle;
}

static void put_alias_buffer(uint32_t *descriptor, const void *pointer, uint64_t size) {
    uint64_t address = (uint64_t)(uintptr_t)pointer;
    descriptor[0] = (uint32_t)size;
    descriptor[1] = (uint32_t)address;
    descriptor[2] = (uint32_t)(((size >> 32) & 0xfu) << 24) |
                    (uint32_t)(((address >> 32) & 0xfu) << 28) |
                    (uint32_t)(((address >> 36) & 0x3fffffu) << 2);
}

enum {
    NGC_MASK_EFFECTIVE_BYTES = 24u,
    NGC_MASK_PADDING_BYTES = 16u,
};
typedef struct {
    uint32_t region_mask;
    uint32_t option[5];
} NgcMaskArguments;
_Static_assert(sizeof(NgcMaskArguments) == NGC_MASK_EFFECTIVE_BYTES, "NGC Mask SDK ABI size changed");

static void make_mask(uint32_t *message, char *input, char *output, unsigned output_size,
                      unsigned effective_bytes, unsigned padding_bytes, unsigned noutput,
                      uint32_t region_mask) {
    NgcMaskArguments arguments = {.region_mask = region_mask};
    unsigned data_size = effective_bytes + padding_bytes;
    memset(message, 0, sizeof(uint32_t) * 64);
    message[0] = 6u | (1u << 20) | (noutput << 24);
    message[1] = (16u + data_size + 3u) / 4u;
    put_alias_buffer(message + 2, input, 32);
    if (noutput) put_alias_buffer(message + 5, output, output_size);
    uint32_t header[4] = {0x49434653u, 0, 2, 0};
    memcpy((uint8_t *)message + 32, header, sizeof header);
    if (effective_bytes > sizeof arguments) effective_bytes = sizeof arguments;
    memcpy((uint8_t *)message + 48, &arguments, effective_bytes);
}

int main(int argc, char **argv) {
#ifdef GAME_ACNH
    const char *mode = argc > 1 ? argv[1] : "unsupported";
    uint32_t message[64];
    uint32_t ngc = open_ngc(message);
    if (strcmp(mode, "mask") == 0) {
        char input[32];
        char output[40];
        memset(input, 0, sizeof input);
        memset(output, 0x5a, sizeof output);
        memcpy(input, "PRIVATE_PLAYER_TEXT_SENTINEL", 28);
        make_mask(message, input, output, sizeof output, NGC_MASK_EFFECTIVE_BYTES, NGC_MASK_PADDING_BYTES, 1, 0);
        send_frame(ngc, message);
        assert(memcmp(output, input, sizeof input) == 0);
        assert(output[sizeof input] == 0x5a);
        return 0;
    }
    if (strcmp(mode, "mask-dict") == 0) {
        assert(argc > 2);
        FILE *f = fopen(argv[2], "rb");
        assert(f);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *gzip = malloc((size_t)n);
        assert(gzip && fread(gzip, 1, (size_t)n, f) == (size_t)n);
        fclose(f);
        NgcAho *ac = ngc_aho_load_gzip(gzip, (size_t)n);
        free(gzip);
        assert(ac);
        char input[64];
        char output[80];
        memset(input, 0, sizeof input);
        memset(output, 0x5a, sizeof output);
        int wlen = ngc_aho_first_word(ac, (uint8_t *)input, sizeof input - 1);
        ngc_aho_free(ac);
        assert(wlen > 0);
        make_mask(message, input, output, sizeof output, NGC_MASK_EFFECTIVE_BYTES, NGC_MASK_PADDING_BYTES, 1, 0xffffffffu);
        send_frame(ngc, message);
        for (int i = 0; i < wlen; i++) assert(output[i] == '*');
        assert(output[wlen] == 0);
        return 0;
    }
    if (strcmp(mode, "malformed") == 0) {
        char input[32];
        char output[40];
        memset(input, 0, sizeof input);
        memset(output, 0x5a, sizeof output);
        make_mask(message, input, output, sizeof output, NGC_MASK_EFFECTIVE_BYTES, 12, 1, 0);
        send_frame(ngc, message);
        assert(!"ngc:u malformed Mask request must fail");
    }
    const char text[] = "PRIVATE_PLAYER_TEXT_SENTINEL";
    make_cmif(message, 4, 23, text, sizeof text);
    send_frame(ngc, message);
    assert(!"ngc:u request must fail explicitly");
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
    return 0;
}
