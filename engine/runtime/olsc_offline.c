/* ACNH olsc:u 只支持本地离线初始化、读取和设置存档备份开关；未知命令失败，日志不输出用户标识。 */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int runtime_account_index(const uint64_t uid[2]);

#ifdef GAME_ACNH
typedef struct { atomic_int initialized; } OlscSession;
/* SetSaveDataBackupSettingEnabled（命令 14）只在本进程记录，GetSaveDataBackupSetting（命令 13）读回；没有云备份。 */
static atomic_int backup_enabled[2];

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
        uint64_t requested_uid[2];
        memcpy(requested_uid, request->data, sizeof requested_uid);
        int index = runtime_account_index(requested_uid);
        if (index < 0 || index >= 2)
            runtime_fail("ACNH olsc GetSetting UID is not the local user");
        uint8_t enabled = (uint8_t)atomic_load(&backup_enabled[index]);
        ipc_out(response, &enabled, sizeof enabled);
        return;
    }
    case 14: {
        if (!atomic_load(&state->initialized)) runtime_fail("ACNH olsc SetSettingEnabled before Initialize");
        if (!root_domain_request(request, 0, 24))
            runtime_fail("ACNH olsc SetSettingEnabled request shape is not supported");
        uint8_t enabled;
        uint64_t requested_uid[2];
        memcpy(&enabled, request->data, sizeof enabled);
        memcpy(requested_uid, (const uint8_t *)request->data + 8, sizeof requested_uid);
        int index = runtime_account_index(requested_uid);
        if (index < 0 || index >= 2)
            runtime_fail("ACNH olsc SetSettingEnabled UID is not the local user");
        if (enabled > 1) runtime_fail("ACNH olsc SetSettingEnabled value is not a bool");
        atomic_store(&backup_enabled[index], enabled);
        fprintf(stderr, "[acnh-olsc] save data backup setting for the local user: %s (kept in this process only)\n",
                enabled ? "enabled" : "disabled");
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
