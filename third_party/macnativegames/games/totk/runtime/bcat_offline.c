/* ACNH bcat:u 仅接入已观测的服务工厂与缓存缺失返回；IBcatService 命令仍记录元数据并拒绝。 */
#include "ipc.h"
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef GAME_ACNH
typedef struct { uint32_t marker; } BcatSessionState;
static const BcatSessionState bcat_session_state = {0x42434154u};
static atomic_flag mount_delivery_cache_logged = ATOMIC_FLAG_INIT;

static unsigned mount_delivery_cache_storage(void) {
    if (!atomic_flag_test_and_set_explicit(&mount_delivery_cache_logged, memory_order_relaxed)) {
        fprintf(stderr,
                "[acnh-bcat] MountDeliveryCacheStorage: local delivery cache unavailable; result=0x47a\n");
    }
    return 0x47au;
}

void *runtime_bcat_override(const char *name) {
    if (name && !strcmp(name, "_ZN2nn4bcat25MountDeliveryCacheStorageEv"))
        return mount_delivery_cache_storage;
    return NULL;
}

static void *bcat_open(void) { return (void *)&bcat_session_state; }
static void bcat_close(void *state) { (void)state; }

static void log_request(const IpcSession *session, const IpcRequest *request) {
    fprintf(stderr,
            "[acnh-bcat] request service=%s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            session->name, request->cmd, request->type, request->domain, request->object_id,
            request->domain_token, request->data_size, request->nx, request->na, request->nb,
            request->nw, request->nc, request->has_pid, request->ncopy, request->nmove);
}

static void bcat_service_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)response;
    log_request(session, request);
    runtime_fail("ACNH bcat:u command is not implemented");
}

static void bcat_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    log_request(session, request);
    if (request->cmd != 0) runtime_fail("ACNH bcat:u service-creator command is not implemented");
    if (request->type != 6 || !request->domain || request->object_id != 1 || request->domain_token != 0 ||
        request->data_size != 8 || request->nx || request->na || request->nb || request->nw || request->nc ||
        !request->has_pid || request->ncopy || request->nmove) {
        runtime_fail("ACNH bcat:u service-creator request shape is not supported");
    }
    ipc_out_object(response, "IBcatService", bcat_service_handler,
                   (void *)&bcat_session_state, bcat_close);
}

__attribute__((constructor)) static void register_bcat_offline(void) {
    runtime_ipc_register("bcat:u", bcat_handler, bcat_open, bcat_close);
}
#endif
