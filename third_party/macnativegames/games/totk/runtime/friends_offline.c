/* ACNH friend:u 只实现已观测的 CreateFriendService；IFriendService 命令仍明确失败。 */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>

#ifdef GAME_ACNH
typedef struct { uint32_t marker; } FriendSessionState;
static const FriendSessionState friend_session_state = {0x46524945u};

static void *friend_open(void) { return (void *)&friend_session_state; }
static void friend_close(void *state) { (void)state; }

static void log_request(const IpcSession *session, const IpcRequest *request) {
    fprintf(stderr,
            "[acnh-friend] request service=%s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            session->name, request->cmd, request->type, request->domain, request->object_id,
            request->domain_token, request->data_size, request->nx, request->na, request->nb,
            request->nw, request->nc, request->has_pid, request->ncopy, request->nmove);
}

static void friend_service_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)response;
    log_request(session, request);
    runtime_fail("ACNH friend:u command is not implemented");
}

static void friend_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    log_request(session, request);
    if (request->cmd != 0) runtime_fail("ACNH friend:u service-creator command is not implemented");
    if (request->type != 6 || !request->domain || request->object_id != 1 || request->domain_token != 0 ||
        request->data_size != 0 || request->nx || request->na || request->nb || request->nw || request->nc ||
        request->has_pid || request->ncopy || request->nmove) {
        runtime_fail("ACNH friend:u service-creator request shape is not supported");
    }
    ipc_out_object(response, "IFriendService", friend_service_handler, (void *)&friend_session_state, friend_close);
}

__attribute__((constructor)) static void register_friends_offline(void) {
    runtime_ipc_register("friend:u", friend_handler, friend_open, friend_close);
}
#endif
