/* ACNH olsc:u 只支持本地离线初始化和查询未启用；未知命令失败，日志不输出请求正文。 */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_account_uid(uint64_t out[2]);

#ifdef GAME_ACNH
typedef struct { atomic_int initialized; } OlscSession;

static void *olsc_open(void) {
    OlscSession *state = calloc(1, sizeof *state);
    if (!state) runtime_fail("ACNH olsc session allocation");
    atomic_init(&state->initialized, 0);
    return state;
}

static void olsc_close(void *state) {
    free(state);
}

static void log_request(const IpcSession *session, const IpcRequest *request) {
    fprintf(stderr,
            "[acnh-olsc] request service=%s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            session->name, request->cmd, request->type, request->domain, request->object_id,
            request->domain_token, request->data_size, request->nx, request->na, request->nb,
            request->nw, request->nc, request->has_pid, request->ncopy, request->nmove);
}

static int root_domain_request(const IpcRequest *request, int has_pid, uint32_t data_size) {
    return request->type == 6 && request->domain && request->object_id == 1 &&
           request->domain_token == 0 && request->has_pid == has_pid &&
           request->data_size == data_size && request->data &&
           request->nx == 0 && request->na == 0 && request->nb == 0 &&
           request->nw == 0 && request->nc == 0 &&
           request->ncopy == 0 && request->nmove == 0;
}

static void olsc_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    log_request(session, request);
    OlscSession *state = session->state;
    if (!state) runtime_fail("ACNH olsc session state missing");
    switch (request->cmd) {
    case 0: {
        if (!root_domain_request(request, 1, 8))
            runtime_fail("ACNH olsc Initialize request shape is not supported");
        uint64_t argument;
        memcpy(&argument, request->data, sizeof argument);
        if (argument != 0) runtime_fail("ACNH olsc Initialize argument is not supported");
        atomic_store(&state->initialized, 1);
        return;
    }
    case 13: {
        if (!atomic_load(&state->initialized)) runtime_fail("ACNH olsc GetSetting before Initialize");
        if (!root_domain_request(request, 0, 16))
            runtime_fail("ACNH olsc GetSetting request shape is not supported");
        uint64_t requested_uid[2], local_uid[2];
        memcpy(requested_uid, request->data, sizeof requested_uid);
        runtime_account_uid(local_uid);
        if (requested_uid[0] != local_uid[0] || requested_uid[1] != local_uid[1])
            runtime_fail("ACNH olsc GetSetting UID is not the local user");
        uint8_t disabled = 0;
        ipc_out(response, &disabled, sizeof disabled);
        return;
    }
    default:
        runtime_fail("ACNH olsc:u command is not implemented");
    }
}

__attribute__((constructor)) static void register_olsc_offline(void) {
    runtime_ipc_register("olsc:u", olsc_handler, olsc_open, olsc_close);
}
#endif
