/* nn::settings "set" 服务：BOTW 保留语言相关实现。ACNH 已核实地区查询 GetRegionCode(cmd 4)，
   以及确认名字时实际到达的 GetLanguageCode(cmd 0，普通会话、8 字节、无缓冲区)。 */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#ifndef GAME_ACNH
static const char *const CODES[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
enum { NCODES = sizeof CODES / sizeof CODES[0], NCODES_V1 = 15 };   /* commands 1 / 3 list the first 15 (pre-4.0 firmware) */

static uint64_t code(unsigned i) { uint64_t v = 0; memcpy(&v, CODES[i], strlen(CODES[i])); return v; }
static uint64_t current(void) {
    uint64_t (*desired)(void) = (uint64_t (*)(void))runtime_override("_ZN2nn2oe18GetDesiredLanguageEv");
    return desired ? desired() : code(1);
}

/* Fill an out buffer with up to `limit` codes; a C buffer ("pointer") is also reported back to the client. */
static int32_t put_codes(const IpcRequest *rq, IpcResponse *rs, unsigned limit) {
    int pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    IpcBuffer b = pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
    unsigned n = 0;
    if (b.size) {
        uint64_t *dst = ipc_guest(b.addr, b.size);
        for (; n < limit && (n + 1) * 8 <= b.size; n++) dst[n] = code(n);
        if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, n * 8, b.index};
    }
    return (int32_t)n;
}
#endif

#ifdef GAME_ACNH
static int acnh_settings_no_payload(const IpcRequest *rq) {
    return (rq->domain ? rq->data_size == 0 : rq->data_size <= 8) &&
           !rq->nx && !rq->na && !rq->nb && !rq->nw && !rq->nc && !rq->has_pid && !rq->ncopy && !rq->nmove;
}

static uint64_t acnh_language_code(void) {
    uint64_t (*desired)(void) = (uint64_t (*)(void))runtime_override("_ZN2nn2oe18GetDesiredLanguageEv");
    if (desired) return desired();
    uint64_t value = 0;
    memcpy(&value, "zh-Hans", 7);
    return value;
}
#endif

static void settings_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
#ifdef GAME_ACNH
    fprintf(stderr,
            "[acnh-settings] request service=%s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            s->name, rq->cmd, rq->type, rq->domain, rq->object_id, rq->domain_token, rq->data_size,
            rq->nx, rq->na, rq->nb, rq->nw, rq->nc, rq->has_pid, rq->ncopy, rq->nmove);
    if (rq->cmd == 0) {
        if (!acnh_settings_no_payload(rq)) runtime_fail("ACNH GetLanguageCode request shape is not supported");
        uint64_t language = acnh_language_code();
        IPC_OUT(rs, language);
        return;
    }
    if (rq->cmd != 4) runtime_fail("ACNH nn::settings command is not implemented");
    if (!acnh_settings_no_payload(rq)) runtime_fail("ACNH GetRegionCode request shape is not supported");
    int32_t region = 1;
    IPC_OUT(rs, region);
    return;
#else
    switch (rq->cmd) {
    case 0: { uint64_t v = current(); IPC_OUT(rs, v); return; }                         /* GetLanguageCode */
    case 1: { int32_t n = put_codes(rq, rs, NCODES_V1); IPC_OUT(rs, n); return; }       /* GetAvailableLanguageCodes */
    case 2: {                                                                            /* MakeLanguageCode(Language) */
        int32_t language = IPC_ARG(rq, int32_t, 0);
        if (language < 0 || language >= NCODES) { rs->result = 0x3c69u; return; }     /* settings: invalid language */
        uint64_t v = code((unsigned)language); IPC_OUT(rs, v); return;
    }
    case 3: { int32_t n = NCODES_V1; IPC_OUT(rs, n); return; }                          /* GetAvailableLanguageCodeCount */
    case 4: { int32_t region = 1; IPC_OUT(rs, region); return; }                        /* GetRegionCode: USA */
    case 5: { int32_t n = put_codes(rq, rs, NCODES); IPC_OUT(rs, n); return; }          /* GetAvailableLanguageCodes2 */
    case 6: { int32_t n = NCODES; IPC_OUT(rs, n); return; }                             /* GetAvailableLanguageCodeCount2 */
    case 8: { uint8_t quest = 0; IPC_OUT(rs, quest); return; }                          /* GetQuestFlag: not a kiosk unit */
    default: ipc_unimplemented(s, rq);
    }
#endif
}

__attribute__((constructor)) static void register_settings(void) { runtime_ipc_register("set", settings_handler, NULL, NULL); }
