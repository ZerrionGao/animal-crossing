/* ACNH ngc:u 仅记录首个到达请求的结构，不读取参数正文或提供虚假成功响应。 */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>

#ifdef GAME_ACNH
static void ngc_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)response;
    fprintf(stderr,
            "[acnh-ngc] request service=%s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            session->name, request->cmd, request->type, request->domain, request->object_id,
            request->domain_token, request->data_size, request->nx, request->na, request->nb,
            request->nw, request->nc, request->has_pid, request->ncopy, request->nmove);
    runtime_fail("ACNH ngc:u command is not implemented");
}

__attribute__((constructor)) static void register_ngc_service(void) {
    runtime_ipc_register("ngc:u", ngc_handler, NULL, NULL);
}
#endif
