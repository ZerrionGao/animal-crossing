/* In-process Horizon IPC server. Message framing follows the public HIPC/CMIF/TIPC description
   (libnx sf/hipc.h, cmif.h, tipc.h; ISC licence). The SDK client code runs unchanged: requests are
   read from the calling thread's TLS message buffer and answered in place. Unknown services and
   commands stop the runtime with their name and id instead of returning a fake success. */
#include "runtime.h"
#include "ipc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMIF_IN_MAGIC 0x49434653u   /* "SFCI" */
#define CMIF_OUT_MAGIC 0x4f434653u  /* "SFCO" */
#define MSG_BYTES 0x100
#define POINTER_BUFFER_SIZE 0x8000

enum { T_CLOSE = 2, T_REQUEST = 4, T_CONTROL = 5, T_REQUEST_CTX = 6, T_CONTROL_CTX = 7, T_TIPC_BASE = 16, T_TIPC_CLOSE = 15 };

typedef struct { const char *name; IpcHandler handler; IpcOpen open; void (*close)(void *); } Service;
static Service services[32];
static unsigned nservices;
static __thread uint64_t trace_fp, trace_lr;   /* guest frame of the current request, for failure reports */
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;

typedef struct { atomic_uint refs; void *state; void (*close)(void *); } IpcOwner;
typedef struct { uint32_t id; const char *name; IpcHandler handler; IpcOwner *owner; } IpcDomainObject;
typedef struct { uint32_t next_id; unsigned count; IpcDomainObject objects[32]; } IpcDomain;

static IpcOwner *owner_new(void *state, void (*close)(void *)) {
    IpcOwner *o = calloc(1, sizeof *o);
    if (!o) runtime_fail("IPC owner allocation");
    atomic_init(&o->refs, 1); o->state = state; o->close = close;
    return o;
}
static void owner_retain(IpcOwner *o) { if (o) atomic_fetch_add(&o->refs, 1); }
static void owner_release(IpcOwner *o) {
    if (o && atomic_fetch_sub(&o->refs, 1) == 1) { if (o->close) o->close(o->state); free(o); }
}
static void *owner_state(IpcOwner *o) { return o ? o->state : NULL; }

static void session_init_lock(IpcSession *s) {
    pthread_mutex_t *lock = malloc(sizeof *lock);
    if (!lock || pthread_mutex_init(lock, NULL)) runtime_fail("IPC session mutex allocation");
    s->lock = lock;
}
static void session_lock(IpcSession *s) { pthread_mutex_lock((pthread_mutex_t *)s->lock); }
static void session_unlock(IpcSession *s) { pthread_mutex_unlock((pthread_mutex_t *)s->lock); }

void runtime_ipc_register(const char *name, IpcHandler handler, IpcOpen open, void (*close)(void *)) {
    pthread_mutex_lock(&registry);
    if (nservices == sizeof services / sizeof *services) runtime_fail("IPC service registry full");
    services[nservices++] = (Service){name, handler, open, close};
    pthread_mutex_unlock(&registry);
}

static IpcSession *session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *), IpcOwner *shared_owner) {
    IpcSession *s = calloc(1, sizeof *s);
    if (!s) runtime_fail("IPC session allocation");
    s->name = name; s->handler = handler; s->state = state; s->close = close;
    s->owner = shared_owner ? shared_owner : owner_new(state, close);
    if (shared_owner) owner_retain(shared_owner);
    atomic_init(&s->refs, 1); /* 句柄持有的初始引用 */
    session_init_lock(s);
    return s;
}
uint32_t runtime_ipc_new_session(const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    return runtime_handle_new(1, session_new(name, handler, state, close, NULL));
}
void runtime_ipc_session_retain(void *p) {
    IpcSession *s = p;
    atomic_fetch_add(&s->refs, 1);
}
void runtime_ipc_session_release(void *p) {
    IpcSession *s = p;
    if (atomic_fetch_sub(&s->refs, 1) != 1) return;
    session_lock(s);
    IpcDomain *d = s->domain;
    s->domain = NULL;
    if (d) {
        for (unsigned i = 0; i < d->count; i++) owner_release(d->objects[i].owner);
        free(d);
    }
    owner_release(s->owner);
    session_unlock(s);
    pthread_mutex_destroy((pthread_mutex_t *)s->lock);
    free(s->lock);
    free(s);
}
void runtime_ipc_session_closed(void *p) { runtime_ipc_session_release(p); }
void runtime_ipc_tmem_closed(void *p) { free(p); }

void *ipc_guest(uint64_t addr, uint64_t size) {
    if (!addr && size) runtime_fail("IPC null buffer");
    if (addr + size < addr) runtime_fail("IPC buffer overflow");
    return (void *)(uintptr_t)addr;
}
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n) {
    if (off + n > rq->data_size) { fprintf(stderr, "[totk-ipc] cmd %u argument %u+%u beyond %u bytes\n", rq->cmd, off, n, rq->data_size); runtime_fail("IPC argument out of range"); }
    return rq->data + off;
}
void ipc_out(IpcResponse *rs, const void *p, uint32_t n) {
    if (rs->data_size + n > sizeof rs->data) runtime_fail("IPC response too large");
    memcpy(rs->data + rs->data_size, p, n); rs->data_size += n;
}
void ipc_out_object(IpcResponse *rs, const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    if (rs->nobjects == sizeof rs->objects / sizeof *rs->objects) runtime_fail("IPC too many output objects");
    rs->objects[rs->nobjects++] = (IpcOutObject){name, handler, state, close};
}
_Noreturn void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) {
    fprintf(stderr, "[totk-ipc] %s: command %u not implemented (type %u, data %u bytes, x%u a%u b%u w%u c%u)\n",
            s->name, rq->cmd, rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc);
    runtime_trace_guest(trace_fp, trace_lr);
    runtime_fail("IPC command not implemented");
}

/* ---------- HIPC framing ---------- */
static uint64_t static_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[0] >> 12) & 0xf) << 32) | ((uint64_t)((d[0] >> 6) & 0x3f) << 36); }
static uint64_t buffer_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[2] >> 28) & 0xf) << 32) | ((uint64_t)((d[2] >> 2) & 0x3fffff) << 36); }
static uint64_t buffer_size(const uint32_t *d) { return d[0] | ((uint64_t)((d[2] >> 24) & 0xf) << 32); }

static int parse(const uint32_t *m, IpcRequest *rq, const uint32_t **data_words, unsigned *ndata) {
    memset(rq, 0, sizeof *rq);
    rq->type = m[0] & 0xffff;
    rq->nx = (m[0] >> 16) & 0xf; rq->na = (m[0] >> 20) & 0xf; rq->nb = (m[0] >> 24) & 0xf; rq->nw = (m[0] >> 28) & 0xf;
    *ndata = m[1] & 0x3ff;
    unsigned recv_mode = (m[1] >> 10) & 0xf, special = m[1] >> 31;
    size_t off = 8;
    if (rq->nx > IPC_MAX_BUF || rq->na > IPC_MAX_BUF || rq->nb > IPC_MAX_BUF || rq->nw > IPC_MAX_BUF) return 0;
    if (special) {
        if (off + 4 > MSG_BYTES) return 0;
        uint32_t h; memcpy(&h, (const uint8_t *)m + off, 4); off += 4;
        rq->has_pid = h & 1; rq->ncopy = (h >> 1) & 0xf; rq->nmove = (h >> 5) & 0xf;
        if (rq->ncopy > 8 || rq->nmove > 8) return 0;
        size_t handle_bytes = (rq->has_pid ? 8u : 0u) + 4u * (rq->ncopy + rq->nmove);
        if (off + handle_bytes > MSG_BYTES) return 0;
        if (rq->has_pid) { memcpy(&rq->pid, (const uint8_t *)m + off, 8); off += 8; }
        for (unsigned i = 0; i < rq->ncopy; i++, off += 4) memcpy(&rq->copy[i], (const uint8_t *)m + off, 4);
        for (unsigned i = 0; i < rq->nmove; i++, off += 4) memcpy(&rq->move[i], (const uint8_t *)m + off, 4);
    }
    if (off + rq->nx * 8u + (rq->na + rq->nb + rq->nw) * 12u > MSG_BYTES) return 0;
    for (unsigned i = 0; i < rq->nx; i++, off += 8) { const uint32_t *p = (const uint32_t *)((const uint8_t *)m + off); rq->x[i] = (IpcBuffer){static_addr(p), p[0] >> 16, p[0] & 0x3f}; }
    IpcBuffer *lists[3] = {rq->a, rq->b, rq->w}; unsigned counts[3] = {rq->na, rq->nb, rq->nw};
    for (unsigned k = 0; k < 3; k++) for (unsigned i = 0; i < counts[k]; i++, off += 12) { const uint32_t *p = (const uint32_t *)((const uint8_t *)m + off); lists[k][i] = (IpcBuffer){buffer_addr(p), buffer_size(p), i}; }
    if (off + (size_t)*ndata * 4u > MSG_BYTES) return 0;
    size_t data_off = off, end_off = off + (size_t)*ndata * 4u;
    rq->nc = recv_mode == 2 ? 1 : recv_mode > 2 ? recv_mode - 2 : 0;
    if (rq->nc > IPC_MAX_BUF || end_off + rq->nc * 8u > MSG_BYTES) return 0;
    for (unsigned i = 0; i < rq->nc; i++, end_off += 8) { uint32_t p[2]; memcpy(p, (const uint8_t *)m + end_off, 8); rq->c[i] = (IpcBuffer){p[0] | ((uint64_t)(p[1] & 0xffff) << 32), p[1] >> 16, i}; }
    rq->data_words = *ndata;
    rq->raw_data_offset = (unsigned)data_off;
    *data_words = (const uint32_t *)((const uint8_t *)m + data_off);
    return 1;
}

static void respond(uint32_t *m, int tipc, int domain, const IpcResponse *rs, const uint32_t *object_ids) {
    uint32_t out[MSG_BYTES / 4]; memset(out, 0, sizeof out);
    unsigned special = rs->ncopy || rs->nmove;
    uint32_t *p = out + 2;
    if (special) {
        *p++ = (rs->ncopy << 1) | (rs->nmove << 5);
        for (unsigned i = 0; i < rs->ncopy; i++) *p++ = rs->copy[i];
        for (unsigned i = 0; i < rs->nmove; i++) *p++ = rs->move[i];
    }
    for (unsigned i = 0; i < rs->nxout; i++, p += 2) {
        uint64_t a = rs->xout[i].addr;
        p[0] = (rs->xout[i].index & 0x3f) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) | (uint32_t)(rs->xout[i].size << 16);
        p[1] = (uint32_t)a;
    }
    uint8_t *words = (uint8_t *)p, *payload;
    unsigned header = tipc ? 4 : domain ? 32 : 16;
    if (tipc) payload = words;
    else payload = (uint8_t *)out + (((words - (uint8_t *)out) + 15) & ~15);
    unsigned prefix = domain ? 16 : 0;
    unsigned used = (unsigned)(payload - words) + header + rs->data_size + (domain ? rs->nobjects * 4 : 0);
    unsigned nwords = tipc ? (used + 3) / 4 : (used + 16 + 3) / 4;   /* CMIF keeps alignment slack like libnx */
    if ((size_t)(words - (uint8_t *)out) + nwords * 4 > MSG_BYTES) runtime_fail("IPC response exceeds message buffer");
    if (tipc) memcpy(payload, &rs->result, 4);
    else {
        if (domain) { uint32_t dh[4] = {rs->nobjects, 0, 0, 0}; memcpy(payload, dh, 16); }
        uint8_t *cmif = payload + prefix;
        uint32_t h[4] = {CMIF_OUT_MAGIC, 0, rs->result, 0}; memcpy(cmif, h, 16);
        memcpy(cmif + 16, rs->data, rs->data_size);
        if (domain && rs->nobjects) memcpy(cmif + 16 + rs->data_size, object_ids, rs->nobjects * 4);
    }
    out[0] = (rs->nxout & 0xf) << 16;
    out[1] = (nwords & 0x3ff) | (special ? 1u << 31 : 0);
    memcpy(m, out, MSG_BYTES);
}

/* ---------- Control requests (CMIF) ---------- */
enum { IPC_RESULT_INVALID_REQUEST = 0xf401u, IPC_RESULT_UNKNOWN_DOMAIN_OBJECT = 0xf402u };
static IpcDomainObject *domain_find(IpcDomain *d, uint32_t id, unsigned *index) {
    if (!d) return NULL;
    for (unsigned i = 0; i < d->count; i++) if (d->objects[i].id == id) { if (index) *index = i; return &d->objects[i]; }
    return NULL;
}
static void domain_remove(IpcDomain *d, unsigned i) {
    owner_release(d->objects[i].owner);
    if (i + 1 < d->count) memmove(&d->objects[i], &d->objects[i + 1], (d->count - i - 1) * sizeof d->objects[0]);
    d->count--;
}
static int domain_can_add(const IpcDomain *d, unsigned n) { return d && n <= 32 - d->count; }
static uint32_t domain_add(IpcDomain *d, const IpcOutObject *o) {
    uint32_t id = d->next_id;
    for (unsigned attempt = 0; attempt < 34; attempt++) {
        if (!id) id = 1;
        if (!domain_find(d, id, NULL)) break;
        id++;
        if (attempt == 33) runtime_fail("IPC domain object id space exhausted");
    }
    IpcDomainObject *entry = &d->objects[d->count++];
    entry->id = id;
    d->next_id = id + 1;
    entry->name = o->name; entry->handler = o->handler; entry->owner = owner_new(o->state, o->close);
    return entry->id;
}
static void control(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 3: { uint16_t n = POINTER_BUFFER_SIZE; IPC_OUT(rs, n); return; }   /* 查询指针缓冲区大小 */
    case 2: case 4:                                                           /* 克隆当前对象 */
        if (s->domain) { rs->result = IPC_RESULT_INVALID_REQUEST; return; }
        if (rs->nmove >= sizeof rs->move / sizeof *rs->move) { rs->result = IPC_RESULT_INVALID_REQUEST; return; }
        rs->move[rs->nmove++] = runtime_handle_new(1, session_new(s->name, s->handler, s->state, s->close, s->owner));
        return;
    case 0: { /* 转换为 domain；这条响应仍使用普通 CMIF，返回根对象 ID。 */
        if (rq->data_words != 8 || rq->raw_data_offset != 8 || rq->has_pid || rq->ncopy || rq->nmove ||
            rq->nx || rq->na || rq->nb || rq->nw || rq->nc || s->domain) {
            rs->result = IPC_RESULT_INVALID_REQUEST; return;
        }
        IpcDomain *d = calloc(1, sizeof *d);
        if (!d) runtime_fail("IPC domain allocation");
        d->next_id = 2; d->count = 1;
        d->objects[0] = (IpcDomainObject){1, s->name, s->handler, s->owner};
        owner_retain(s->owner);
        s->domain = d;
        uint32_t root = 1; IPC_OUT(rs, root);
        return;
    }
    default:
        fprintf(stderr, "[totk-ipc] %s: unsupported control command %u\n", s->name, rq->cmd);
        runtime_fail("IPC control command not implemented");
    }
}

static void dispatch(IpcSession *s, uint32_t *m) {
    IpcRequest rq; const uint32_t *words; unsigned ndata;
    if (!parse(m, &rq, &words, &ndata)) { IpcResponse bad = {.result = IPC_RESULT_INVALID_REQUEST}; respond(m, 0, 0, &bad, NULL); return; }
    if (getenv("TOTK_IPC_DUMP") && rq.na + rq.nb) { fprintf(stderr, "[totk-ipc] raw %s:", s->name); for (unsigned i = 0; i < 20; i++) fprintf(stderr, " %08x", m[i]); fputc('\n', stderr); }
    IpcResponse rs; memset(&rs, 0, sizeof rs);
    if (rq.type == T_CLOSE || rq.type == T_TIPC_CLOSE) return;
    int tipc = rq.type >= T_TIPC_BASE;
    size_t raw_off = (size_t)((const uint8_t *)words - (const uint8_t *)m), end_off = raw_off + ndata * 4u;
    size_t payload_off = (raw_off + 15u) & ~(size_t)15u;
    if (tipc) { rq.cmd = rq.type - T_TIPC_BASE; rq.data = (const uint8_t *)words; rq.data_size = ndata * 4u; }
    else if (rq.type == T_REQUEST || rq.type == T_REQUEST_CTX || rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) {
        if (payload_off > end_off || end_off > MSG_BYTES) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 0, &rs, NULL); return; }
        const uint8_t *payload = (const uint8_t *)m + payload_off;
        if (rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) {
            if (end_off - payload_off < 24) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 0, &rs, NULL); return; }
            uint32_t h[4]; memcpy(h, payload, 16);
            if (h[0] != CMIF_IN_MAGIC) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 0, &rs, NULL); return; }
            rq.cmd = h[2]; rq.data = payload + 16; rq.data_size = (uint32_t)(end_off - payload_off - 16);
        } else if (s->domain) {
            if (end_off - payload_off < 16) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 1, &rs, NULL); return; }
            uint8_t dh[16]; memcpy(dh, payload, sizeof dh);
            uint8_t dtype = dh[0], in_count = dh[1]; uint16_t data_size; uint32_t object_id, padding;
            memcpy(&data_size, dh + 2, 2); memcpy(&object_id, dh + 4, 4); memcpy(&padding, dh + 8, 4); memcpy(&rq.domain_token, dh + 12, 4);
            rq.domain = 1; rq.object_id = object_id;
            if (dtype == 2) {
                size_t available = end_off - payload_off;
                if (in_count || data_size || padding || available < 16 || ndata != 8) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 1, &rs, NULL); return; }
                unsigned index;
                if (!domain_find(s->domain, object_id, &index)) rs.result = IPC_RESULT_UNKNOWN_DOMAIN_OBJECT;
                else domain_remove(s->domain, index);
                respond(m, 0, 1, &rs, NULL); return;
            }
            size_t available = end_off - payload_off;
            size_t expected = 16u + (size_t)data_size + (size_t)in_count * 4u;
            size_t actual_size = 16u + expected;
            /* 当前只接收无输出指针大小表的 domain 请求；其余合法扩展暂按长度错误拒绝。 */
            if (dtype != 1 || in_count || padding || data_size < 16 || expected > available || ndata != (actual_size + 3u) / 4u) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 1, &rs, NULL); return; }
            uint32_t h[4]; memcpy(h, payload + 16, 16);
            if (h[0] != CMIF_IN_MAGIC) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 1, &rs, NULL); return; }
            rq.cmd = h[2]; rq.data = payload + 32; rq.data_size = data_size - 16;
        } else {
            if (end_off - payload_off < 16) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 0, &rs, NULL); return; }
            uint32_t h[4]; memcpy(h, payload, 16);
            if (h[0] != CMIF_IN_MAGIC) { rs.result = IPC_RESULT_INVALID_REQUEST; respond(m, 0, 0, &rs, NULL); return; }
            rq.cmd = h[2]; rq.data = payload + 16; rq.data_size = (uint32_t)(end_off - payload_off - 16);
        }
    } else {
        fprintf(stderr, "[totk-ipc] %s: message type %u at %p:", s->name, rq.type, (void *)m);
        for (unsigned i = 0; i < 16; i++) fprintf(stderr, " %08x", m[i]);
        fputc('\n', stderr); runtime_fail("IPC message type not implemented");
    }
    if (getenv("TOTK_NIFM_TRACE") && !strncmp(s->name, "nifm:", 5))
        fprintf(stderr, "[totk-nifm] request service=%s cmd=%u type=%u data=%u buffers=a%u b%u c%u\n",
                s->name, rq.cmd, rq.type, rq.data_size, rq.na, rq.nb, rq.nc);
    int domain_response = rq.domain;
    if (rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) control(s, &rq, &rs);
    else if (rq.domain) {
        IpcDomainObject *o = domain_find(s->domain, rq.object_id, NULL);
        if (!o) rs.result = IPC_RESULT_UNKNOWN_DOMAIN_OBJECT;
        else {
            IpcSession target = {0};
            target.name = o->name; target.handler = o->handler; target.state = owner_state(o->owner); target.owner = o->owner;
            target.domain = s->domain; target.lock = s->lock;
            target.handler(&target, &rq, &rs);
        }
    } else s->handler(s, &rq, &rs);
    uint32_t ids[8] = {0};
    if (rs.nobjects) {
        if (domain_response) {
            if (!domain_can_add(s->domain, rs.nobjects)) {
                rs.result = IPC_RESULT_INVALID_REQUEST;
                for (unsigned i = 0; i < rs.nobjects; i++) if (rs.objects[i].close) rs.objects[i].close(rs.objects[i].state);
                rs.nobjects = 0;
            } else {
                for (unsigned i = 0; i < rs.nobjects; i++) ids[i] = domain_add(s->domain, &rs.objects[i]);
            }
        } else {
            if (rs.nobjects + rs.nmove > sizeof rs.move / sizeof *rs.move) {
                rs.result = IPC_RESULT_INVALID_REQUEST;
                for (unsigned i = 0; i < rs.nobjects; i++) if (rs.objects[i].close) rs.objects[i].close(rs.objects[i].state);
                rs.nobjects = 0;
            }
            else {
                if (rs.nobjects) memmove(rs.move + rs.nobjects, rs.move, rs.nmove * sizeof rs.move[0]);
                for (unsigned i = 0; i < rs.nobjects; i++) rs.move[i] = runtime_ipc_new_session(rs.objects[i].name ? rs.objects[i].name : s->name, rs.objects[i].handler, rs.objects[i].state, rs.objects[i].close);
                rs.nmove += rs.nobjects;
                rs.nobjects = 0;
            }
        }
    }
    respond(m, tipc, domain_response, &rs, ids);
}

/* ---------- sm: service manager ---------- */
static void sm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: return;                                          /* RegisterClient / Initialize */
    case 1: {                                                /* GetServiceHandle(name) */
        char name[9] = {0}; memcpy(name, ipc_arg(rq, 0, 8), 8);
        pthread_mutex_lock(&registry);
        const Service *found = NULL;
        for (unsigned i = 0; i < nservices; i++) if (!strcmp(services[i].name, name)) found = &services[i];
        pthread_mutex_unlock(&registry);
        if (!found) { fprintf(stderr, "[totk-ipc] sm: service \"%s\" not implemented\n", name); runtime_trace_guest(trace_fp, trace_lr); runtime_fail("IPC service not implemented"); }
        rs->move[rs->nmove++] = runtime_ipc_new_session(found->name, found->handler, found->open ? found->open() : NULL, found->close);
        fprintf(stderr, "[totk-ipc] opened service %s\n", found->name);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- SVCs ---------- */
int runtime_ipc_svc(unsigned num, uint64_t *r) {
    switch (num) {
    case 0x1f: {                                             /* ConnectToNamedPort(x1 name) -> w1 */
        char name[13] = {0}; memcpy(name, (const void *)(uintptr_t)r[1], 12);
        if (strcmp(name, "sm:")) { fprintf(stderr, "[totk-ipc] named port \"%s\"\n", name); runtime_fail("IPC named port not implemented"); }
        r[0] = 0; r[1] = runtime_ipc_new_session("sm:", sm, NULL, NULL); return 1;
    }
    case 0x21: case 0x22: {                                  /* SendSyncRequest(w0) / WithUserBuffer(x0, x1, w2) */
        uint32_t h = (uint32_t)(num == 0x21 ? r[0] : r[2]);
        IpcSession *s = runtime_handle_acquire_session(h);
        if (!s) { r[0] = 0xe401; return 1; }
        uint32_t *m = num == 0x21 ? pthread_getspecific(runtime_keys[2]) : (uint32_t *)(uintptr_t)r[0];
        if (!m || (num == 0x22 && r[1] < MSG_BYTES)) runtime_fail("IPC message buffer");
        trace_fp = r[29]; trace_lr = r[30];
        session_lock(s);
        dispatch(s, m);
        session_unlock(s);
        runtime_ipc_session_release(s); r[0] = 0; return 1;
    }
    case 0x15: {                                             /* CreateTransferMemory(x1 addr, x2 size, w3 perm) -> w1 */
        IpcTransferMemory *t = malloc(sizeof *t);
        if (!t) runtime_fail("transfer memory allocation");
        *t = (IpcTransferMemory){r[1], r[2], (uint32_t)r[3]};
        r[0] = 0; r[1] = runtime_handle_new(0, t); return 1;
    }
    default: return 0;
    }
}
