/* ACNH pctl：登机影片结束后 nn::pctl::IsFreeCommunicationAvailable 会打开家长控制服务。
   SDK 依次尝试 pctl、pctl:s、pctl:a、pctl:r。工厂命令 0/1 创建 IParentalControlService
   （1 是 CreateServiceWithoutInitialize）。进岛后会再调服务命令 1（Initialize）：
   type=6、0 字节输入、无缓冲区和句柄，结果 0、无输出。
   服务命令 1018 的结果为 0 时，SDK 只做 cmp w0,#0，不读输出布尔，空闲通信视为可用。
   随后账号检查走离线。把这条命令改成中止进程，影片一结束窗口就会关掉。
   1001（CheckFreeCommunicationPermission）同样只看结果，按允许处理。其余服务命令明确失败。不记录用户数据。 */
#include "ipc.h"
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>

#ifdef GAME_ACNH
static void note(const char *name, const IpcRequest *rq) {
    static uint32_t seen[48];
    static unsigned nseen;
    static pthread_mutex_t note_lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t key = ((uint32_t)(unsigned char)name[0] << 16) | rq->cmd;
    pthread_mutex_lock(&note_lock);
    for (unsigned i = 0; i < nseen; i++) {
        if (seen[i] == key) {
            pthread_mutex_unlock(&note_lock);
            return;
        }
    }
    if (nseen < sizeof seen / sizeof seen[0]) seen[nseen++] = key;
    fprintf(stderr,
            "[acnh-pctl] %s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            name, rq->cmd, rq->type, rq->domain, rq->object_id, rq->domain_token,
            rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc,
            rq->has_pid, rq->ncopy, rq->nmove);
    pthread_mutex_unlock(&note_lock);
}

static void pctl_service(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    note("service", rq);
    if ((rq->cmd != 1 && rq->cmd != 1001 && rq->cmd != 1018) || rq->type != 6 ||
        rq->domain_token || rq->data_size || rq->has_pid || rq->ncopy || rq->nmove ||
        rq->nx || rq->na || rq->nb || rq->nw || rq->nc)
        ipc_unimplemented(session, rq);
}

static void pctl_factory(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    note("factory", rq);
    if (rq->cmd > 1) ipc_unimplemented(session, rq);
    ipc_out_object(rs, "IParentalControlService", pctl_service, NULL, NULL);
}

__attribute__((constructor)) static void register_pctl(void) {
    static const char *const names[] = {"pctl", "pctl:s", "pctl:a", "pctl:r"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        runtime_ipc_register(names[i], pctl_factory, NULL, NULL);
}
#endif
