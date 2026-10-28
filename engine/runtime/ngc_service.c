/* ACNH ngc:u Mask（cmd 2）。词库来自本机 NgWord2，用 Aho-Corasick 做子串替换。
   不读取、不打印名字正文。其他命令仍失败。 */
#include "ipc.h"
#include "ngc_aho.h"
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GAME_ACNH
enum {
    NGC_MASK_EFFECTIVE_BYTES = 4u + 5u * 4u,
    NGC_MASK_PADDING_BYTES = 16u,
    NGC_MASK_INLINE_BYTES = NGC_MASK_EFFECTIVE_BYTES + NGC_MASK_PADDING_BYTES,
    NGC_REGIONS = 16,
    NGC_MAX_TEXT = 0x800,
    NGC_MAX_RANGES = 64,
};

typedef struct { int start, end; } NgcRange;
typedef struct { NgcRange ranges[NGC_MAX_RANGES]; int n, words; } NgcHits;

static pthread_mutex_t ngc_lock = PTHREAD_MUTEX_INITIALIZER;
static int ngc_ready, ngc_failed;
static NgcAho *ngc_common;
static NgcAho *ngc_common_b1;
static NgcAho *ngc_region[NGC_REGIONS];
static NgcAho *ngc_region_b1[NGC_REGIONS];

static int collect_hit(int start, int end, void *ctx) {
    NgcHits *hits = ctx;
    if (start < 0 || end <= start) return 1;
    hits->words++;
    if (hits->n < NGC_MAX_RANGES) hits->ranges[hits->n++] = (NgcRange){start, end};
    return 1;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static NgcAho *load_named(const char *dir, const char *name) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t n = 0;
    uint8_t *gzip = read_file(path, &n);
    if (!gzip) return NULL;
    NgcAho *ac = ngc_aho_load_gzip(gzip, n);
    free(gzip);
    return ac;
}

static const char *ngc_dir(void) {
    const char *env = getenv("TOTK_NGC_DIR");
    if (env && env[0]) return env;
    static const char *const fallbacks[] = {
        "../../work/acnh/ngc-private",
        "../../work/research/ngword2-from-xci-20261023/romfs",
        "work/acnh/ngc-private",
        "work/research/ngword2-from-xci-20261023/romfs",
        NULL,
    };
    for (int i = 0; fallbacks[i]; i++) {
        char probe[4096];
        snprintf(probe, sizeof probe, "%s/ac_common_not_b_nx", fallbacks[i]);
        FILE *f = fopen(probe, "rb");
        if (f) { fclose(f); return fallbacks[i]; }
    }
    return NULL;
}

static void ngc_load(void) {
    const char *dir = ngc_dir();
    if (!dir) {
        fprintf(stderr, "[acnh-ngc] NgWord2 目录未找到，Mask 原文照写\n");
        ngc_failed = 1;
        return;
    }
    ngc_common = load_named(dir, "ac_common_not_b_nx");
    ngc_common_b1 = load_named(dir, "ac_common_b1_nx");
    if (!ngc_common) {
        fprintf(stderr, "[acnh-ngc] 无法加载 common 词库\n");
        ngc_failed = 1;
        return;
    }
    int regions = 0;
    for (int i = 0; i < NGC_REGIONS; i++) {
        char name[32];
        snprintf(name, sizeof name, "ac_%d_not_b_nx", i);
        ngc_region[i] = load_named(dir, name);
        snprintf(name, sizeof name, "ac_%d_b1_nx", i);
        ngc_region_b1[i] = load_named(dir, name);
        if (ngc_region[i] || ngc_region_b1[i]) regions++;
    }
    ngc_ready = 1;
    fprintf(stderr, "[acnh-ngc] NgWord2 已加载（common not_b/b1 + %d 个地区），来源目录已核验\n", regions);
}

static void ensure_loaded(void) {
    pthread_mutex_lock(&ngc_lock);
    if (!ngc_ready && !ngc_failed) ngc_load();
    pthread_mutex_unlock(&ngc_lock);
}

static void ascii_lower(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
    }
}

static int32_t mask_text(uint8_t *text, size_t n, uint32_t region_mask, uint32_t system_mask) {
    size_t len = n;
    for (size_t i = 0; i < n; i++) if (!text[i]) { len = i; break; }
    if (!len) return 0;
    if (len > NGC_MAX_TEXT) len = NGC_MAX_TEXT;
    uint8_t lowered[NGC_MAX_TEXT];
    ascii_lower(lowered, text, len);
    NgcHits hits = {0};
    ngc_aho_match(ngc_common, lowered, len, collect_hit, &hits);
    if (ngc_common_b1) ngc_aho_match(ngc_common_b1, lowered, len, collect_hit, &hits);
    uint32_t use = region_mask | system_mask;
    for (int i = 0; i < NGC_REGIONS; i++) {
        if (!(use & (1u << i))) continue;
        if (ngc_region[i]) ngc_aho_match(ngc_region[i], lowered, len, collect_hit, &hits);
        if (ngc_region_b1[i]) ngc_aho_match(ngc_region_b1[i], lowered, len, collect_hit, &hits);
    }
    for (int i = 0; i < hits.n; i++) {
        int start = hits.ranges[i].start, end = hits.ranges[i].end;
        if (start < 0) start = 0;
        if (end > (int)len) end = (int)len;
        for (int p = start; p < end; p++) text[p] = '*';
    }
    return hits.words;
}

static void ngc_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    fprintf(stderr,
            "[acnh-ngc] request service=%s cmd=%u type=%u domain=%d object=%u token=%u data=%u "
            "buffers=x%u/a%u/b%u/w%u/c%u handles=pid:%d,copy:%u,move:%u\n",
            session->name, request->cmd, request->type, request->domain, request->object_id,
            request->domain_token, request->data_size, request->nx, request->na, request->nb,
            request->nw, request->nc, request->has_pid, request->ncopy, request->nmove);
    int mask = request->cmd == 2 && !request->domain && request->data_size == NGC_MASK_INLINE_BYTES &&
               !request->nx && request->na == 1 && request->nb == 1 && !request->nw && !request->nc &&
               !request->has_pid && !request->ncopy && !request->nmove;
    if (!mask) runtime_fail("ACNH ngc:u command is not implemented");
    if (!request->a[0].size || request->b[0].size < request->a[0].size)
        runtime_fail("ACNH ngc Mask buffer is not supported");
    const uint8_t *src = ipc_guest(request->a[0].addr, request->a[0].size);
    uint8_t *dst = ipc_guest(request->b[0].addr, request->b[0].size);
    memcpy(dst, src, request->a[0].size);
    uint32_t region_mask = 0, option[5] = {0};
    memcpy(&region_mask, request->data, 4);
    memcpy(option, request->data + 4, sizeof option);
    ensure_loaded();
    int32_t count = 0;
    if (ngc_ready) {
        pthread_mutex_lock(&ngc_lock);
        count = mask_text(dst, request->a[0].size, region_mask, option[3]);
        pthread_mutex_unlock(&ngc_lock);
        fprintf(stderr, "[acnh-ngc] Mask 使用 NgWord2，替换 %d 处（%llu 字节）\n",
                count, (unsigned long long)request->a[0].size);
    } else {
        fprintf(stderr, "[acnh-ngc] Mask 无词库，原文照写（%llu 字节），未过滤\n",
                (unsigned long long)request->a[0].size);
    }
    IPC_OUT(response, count);
}

__attribute__((constructor)) static void register_ngc_service(void) {
    runtime_ipc_register("ngc:u", ngc_handler, NULL, NULL);
}
#endif
