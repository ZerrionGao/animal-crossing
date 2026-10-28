/* ACNH friend:u 只实现已观测的 CreateFriendService；IFriendService 命令仍明确失败。
   可安全判定为空的本地列表查询返回空列表；在线会话、游玩历史和密钥相关动作统一返回
   nn::friends::ResultInternetRequestNotAccepted（0xc79），不制造状态或密钥内容。 */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#ifdef GAME_ACNH
enum {
    FRIEND_RESULT_INVALID_ARGUMENT = 0x1c79u,
    FRIEND_RESULT_INTERNET_REQUEST_NOT_ACCEPTED = 0xc79u,
};

static void note_offline(void) {
    static atomic_int said;
    if (atomic_exchange_explicit(&said, 1, memory_order_relaxed) == 0)
        fprintf(stderr, "[acnh-friend] 离线，好友名单为空\n");
}

static int valid_empty_query(int *count, const void *out, const void *uid, int offset, int limit) {
    if (!count || !uid || offset < 0 || limit < 0 || limit > 0x10000 || (limit && !out)) return 0;
    return 1;
}

static uint32_t empty_count(int *count, const void *out, const void *uid, int offset, int limit) {
    note_offline();
    if (!valid_empty_query(count, out, uid, offset, limit)) return FRIEND_RESULT_INVALID_ARGUMENT;
    *count = 0;
    return 0;
}
static uint32_t blocked_users(int *count, void *out, const void *uid, int offset, int limit) {
    return empty_count(count, out, uid, offset, limit);
}
static uint32_t friend_list(int *count, void *out, const void *uid, int offset, int limit, const void *filter) {
    if (!filter) return FRIEND_RESULT_INVALID_ARGUMENT;
    return empty_count(count, out, uid, offset, limit);
}
static uint32_t friend_unavailable(void) {
    note_offline();
    return FRIEND_RESULT_INTERNET_REQUEST_NOT_ACCEPTED;
}
static uint32_t play_history_key(unsigned char *out, const void *uid, int local_only) {
    (void)out; (void)uid; (void)local_only;
    note_offline();
    return FRIEND_RESULT_INTERNET_REQUEST_NOT_ACCEPTED;
}
void *runtime_friends_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", blocked_users},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", friend_list},
        {"_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", friend_unavailable},
        {"_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", friend_unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friend_unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", play_history_key},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", friend_unavailable},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", friend_unavailable},
        {"_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", friend_unavailable},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", friend_unavailable},
    };
    for (unsigned i = 0; i < sizeof entries / sizeof entries[0]; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

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
