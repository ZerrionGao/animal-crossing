/* ACNH audout:u — nn::audio::detail::IAudioOutManager / IAudioOut.
   登机影片的 VP9 画面已经在 movie_host 上解码。音轨是 Vorbis（movie::DecoderType 5），
   原库在开始播放时打开 audout:u。这里按 SDK 的 OpenAudioOut / AppendAudioOutBuffer /
   GetReleasedAudioOutBuffer 把 PCM 送到单独的 AudioQueue，和已有的 audren 输出叠在一起。
   采样格式固定回 PcmInt16，和主机 audout 的缺省一致；客户端按返回值组缓冲。 */
#include "ipc.h"
#include "runtime.h"
#include <AudioToolbox/AudioQueue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef GAME_ACNH
enum { PCM_INT16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, NAME_BYTES = 0x100, QCAP = 64, RELCAP = 128, SLICE = 960 };
static const char device_name[] = "AudioTvOutput";

typedef struct {
    uint64_t tag, data, size, offset, capacity;
} Pcm;

typedef struct Out {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    int alive, busy, state;
    uint32_t event_w, rate;
    unsigned channels;
    float volume;
    uint64_t played, inflight;
    int has_inflight;
    AudioQueueRef queue;
    AudioQueueBufferRef slice;
    int slice_done, queue_ok;
    Pcm q[QCAP];
    int qh, nq;
    uint64_t rel[RELCAP];
    int nr;
} Out;

static void queue_done(void *user, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)queue; (void)buffer;
    Out *o = user;
    pthread_mutex_lock(&o->mu);
    o->slice_done = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

static void sleep_slice(Out *o, uint64_t frames) {
    uint64_t ns = frames * 1000000000ull / (o->rate ? o->rate : 48000u);
    struct timespec step = {0, 20000000};
    while (ns) {
        pthread_mutex_lock(&o->mu);
        int stop = !o->alive || o->state != STATE_STARTED;
        pthread_mutex_unlock(&o->mu);
        if (stop) return;
        if (ns < 20000000ull) step.tv_nsec = (long)ns;
        nanosleep(&step, NULL);
        ns -= ns < 20000000ull ? ns : 20000000ull;
    }
}

static int valid_pcm(const Out *o, const Pcm *pcm) {
    unsigned ch = o->channels ? o->channels : 2;
    uint64_t frame_bytes = (uint64_t)ch * 2u;
    if (!pcm->data || pcm->offset > pcm->capacity || pcm->size > pcm->capacity - pcm->offset ||
        pcm->offset % frame_bytes || pcm->size % frame_bytes || pcm->data > UINT64_MAX - pcm->offset) return 0;
    uint64_t source = pcm->data + pcm->offset;
    return source <= UINT64_MAX - pcm->size;
}

static uint64_t play_pcm(Out *o, const Pcm *pcm) {
    unsigned ch = o->channels ? o->channels : 2;
    uint64_t frame_bytes = (uint64_t)ch * 2u;
    if (!valid_pcm(o, pcm)) return 0;
    uint64_t frames = pcm->size / frame_bytes;
    const int16_t *src = (const int16_t *)ipc_guest(pcm->data + pcm->offset, pcm->size);
    uint64_t done = 0;
    while (done < frames) {
        pthread_mutex_lock(&o->mu);
        int run = o->alive && o->state == STATE_STARTED;
        float volume = o->volume;
        pthread_mutex_unlock(&o->mu);
        if (!run) break;
        uint64_t n = frames - done;
        if (n > SLICE) n = SLICE;
        if (o->queue_ok) {
            float *dst = o->slice->mAudioData;
            for (uint64_t i = 0; i < n; i++) {
                float l = src[(done + i) * ch] / 32768.0f * volume;
                float r = ch > 1 ? src[(done + i) * ch + 1] / 32768.0f * volume : l;
                dst[2 * i] = l; dst[2 * i + 1] = r;
            }
            o->slice->mAudioDataByteSize = (UInt32)(n * 2u * sizeof(float));
            pthread_mutex_lock(&o->mu);
            o->slice_done = 0;
            pthread_mutex_unlock(&o->mu);
            if (AudioQueueEnqueueBuffer(o->queue, o->slice, 0, NULL) || AudioQueueStart(o->queue, NULL)) {
                o->queue_ok = 0;
                fprintf(stderr, "[acnh-audout] AudioQueue 写入失败，后续缓冲只按时长释放\n");
                sleep_slice(o, n);
            } else {
                pthread_mutex_lock(&o->mu);
                while (!o->slice_done && o->alive && o->state == STATE_STARTED) pthread_cond_wait(&o->cv, &o->mu);
                int finished = o->slice_done;
                pthread_mutex_unlock(&o->mu);
                if (!finished) break;
            }
        } else sleep_slice(o, n);
        done += n;
    }
    return done;
}

static void release_tag(Out *o, uint64_t tag) {
    if (o->nr == RELCAP) {
        runtime_fail("ACNH audout release queue overflow");
    }
    o->rel[o->nr++] = tag;
}

static void *worker(void *arg) {
    Out *o = arg;
    for (;;) {
        Pcm pcm;
        pthread_mutex_lock(&o->mu);
        while (o->alive && (o->state != STATE_STARTED || !o->nq || o->nr == RELCAP)) pthread_cond_wait(&o->cv, &o->mu);
        if (!o->alive) { pthread_mutex_unlock(&o->mu); break; }
        pcm = o->q[o->qh];
        o->qh = (o->qh + 1) % QCAP;
        o->nq--;
        o->inflight = pcm.tag;
        o->has_inflight = 1;
        o->busy = 1;
        pthread_mutex_unlock(&o->mu);
        uint64_t frames = play_pcm(o, &pcm);
        pthread_mutex_lock(&o->mu);
        o->played += frames;
        o->has_inflight = 0;
        release_tag(o, pcm.tag);
        o->busy = 0;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
        runtime_event_signal(o->event_w);
    }
    return NULL;
}

static void out_handler(IpcSession *session, const IpcRequest *rq, IpcResponse *rs);

static void out_close(void *p) {
    Out *o = p;
    pthread_mutex_lock(&o->mu);
    o->alive = 0;
    o->state = STATE_STOPPED;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    if (o->queue) AudioQueueStop(o->queue, true);
    pthread_join(o->thread, NULL);
    if (o->queue) AudioQueueDispose(o->queue, true);
    runtime_event_close_writable(o->event_w);
    pthread_mutex_destroy(&o->mu);
    pthread_cond_destroy(&o->cv);
    free(o);
}

static Out *out_new(uint32_t rate, unsigned channels) {
    Out *o = calloc(1, sizeof *o);
    if (!o) runtime_fail("audout allocation");
    pthread_mutex_init(&o->mu, NULL);
    pthread_cond_init(&o->cv, NULL);
    o->alive = 1;
    o->state = STATE_STOPPED;
    o->rate = rate ? rate : 48000;
    o->channels = channels ? channels : 2;
    o->volume = 1;
    o->event_w = runtime_event_new();
    AudioStreamBasicDescription fmt = {
        .mSampleRate = o->rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8, .mFramesPerPacket = 1, .mBytesPerFrame = 8,
        .mChannelsPerFrame = 2, .mBitsPerChannel = 32,
    };
    if (!AudioQueueNewOutput(&fmt, queue_done, o, NULL, NULL, 0, &o->queue) &&
        !AudioQueueAllocateBuffer(o->queue, SLICE * 2u * sizeof(float), &o->slice)) o->queue_ok = 1;
    else fprintf(stderr, "[acnh-audout] AudioQueue 没有打开，影片音轨按时长释放、没有声音\n");
    if (pthread_create(&o->thread, NULL, worker, o)) runtime_fail("audout thread");
    return o;
}

static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    char *dst = ipc_guest(b->addr, b->size);
    size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size);
    memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = *b;
}

static IpcBuffer take_in(const IpcRequest *rq) {
    if (rq->na && rq->a[0].size) return rq->a[0];
    if (rq->nx && rq->x[0].size) return rq->x[0];
    return (IpcBuffer){0};
}
static IpcBuffer take_out(const IpcRequest *rq, int *pointer) {
    if (rq->nb && rq->b[0].size) { *pointer = 0; return rq->b[0]; }
    if (rq->nc && rq->c[0].size) { *pointer = 1; return rq->c[0]; }
    if (rq->nx && rq->x[0].size && !rq->na) { *pointer = 1; return rq->x[0]; }
    *pointer = 0;
    return (IpcBuffer){0};
}

static void list_outs(const IpcRequest *rq, IpcResponse *rs) {
    int pointer = 0;
    IpcBuffer b = take_out(rq, &pointer);
    uint32_t count = b.size >= NAME_BYTES ? 1u : 0u;
    if (count) put_name(&b, rs, pointer);
    IPC_OUT(rs, count);
}

static void open_out(const IpcRequest *rq, IpcResponse *rs) {
    int pointer = 0;
    IpcBuffer in = take_in(rq);
    IpcBuffer out = take_out(rq, &pointer);
    if (!out.size && rq->nx >= 2 && rq->x[1].size) { pointer = 1; out = rq->x[1]; }
    if (out.size < NAME_BYTES) {
        fprintf(stderr, "[acnh-audout] Open 缓冲不符 x%u a%u b%u c%u data=%u\n", rq->nx, rq->na, rq->nb, rq->nc, rq->data_size);
        runtime_fail("ACNH audout OpenAudioOut buffer is not supported");
    }
    uint32_t rate = 48000;
    unsigned channels = 2;
    if (rq->data_size >= 8) {
        uint32_t raw_rate; uint16_t raw_ch;
        memcpy(&raw_rate, rq->data, 4);
        memcpy(&raw_ch, rq->data + 4, 2);
        if (raw_rate) rate = raw_rate;
        if (raw_ch) channels = raw_ch;
    }
    if (channels > 8) channels = 2;
    (void)in;
    put_name(&out, rs, pointer);
    Out *o = out_new(rate, channels);
    struct { uint32_t rate, channels, format, state; } param = {o->rate, o->channels, PCM_INT16, STATE_STOPPED};
    IPC_OUT(rs, param);
    ipc_out_object(rs, "IAudioOut", out_handler, o, out_close);
    fprintf(stderr, "[acnh-audout] 打开 %u Hz x%u pcm16\n", o->rate, o->channels);
}

static int contains_tag(Out *o, uint64_t tag) {
    if (o->has_inflight && o->inflight == tag) return 1;
    for (int i = 0; i < o->nq; i++) if (o->q[(o->qh + i) % QCAP].tag == tag) return 1;
    return 0;
}

static void out_handler(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    Out *o = session->state;
    switch (rq->cmd) {
    case 0: { uint32_t state; pthread_mutex_lock(&o->mu); state = (uint32_t)o->state; pthread_mutex_unlock(&o->mu); IPC_OUT(rs, state); return; }
    case 1:
        pthread_mutex_lock(&o->mu);
        o->state = STATE_STARTED;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
        return;
    case 2: {
        pthread_mutex_lock(&o->mu);
        o->state = STATE_STOPPED;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
        if (o->queue) AudioQueueStop(o->queue, true);
        pthread_mutex_lock(&o->mu);
        while (o->busy) pthread_cond_wait(&o->cv, &o->mu);
        int released = 0;
        while (o->nq) { release_tag(o, o->q[o->qh].tag); o->qh = (o->qh + 1) % QCAP; o->nq--; released = 1; }
        pthread_mutex_unlock(&o->mu);
        if (released) runtime_event_signal(o->event_w);
        return;
    }
    case 3: case 7: {
        IpcBuffer b = take_in(rq);
        if (b.size < 0x28 || rq->data_size < 8) {
            fprintf(stderr, "[acnh-audout] Append 缓冲不符 size=%llu data=%u x%u a%u\n", (unsigned long long)b.size, rq->data_size, rq->nx, rq->na);
            runtime_fail("ACNH audout AppendAudioOutBuffer is not supported");
        }
        uint64_t tag, data, capacity, size, offset;
        const uint8_t *raw = ipc_guest(b.addr, 0x28);
        memcpy(&tag, rq->data, 8);
        memcpy(&data, raw + 8, 8);
        memcpy(&capacity, raw + 16, 8);
        memcpy(&size, raw + 24, 8);
        memcpy(&offset, raw + 32, 8);
        Pcm pcm = {tag, data, size, offset, capacity};
        if (!valid_pcm(o, &pcm)) {
            fprintf(stderr, "[acnh-audout] Append 数据范围或帧对齐无效 data=%#llx capacity=%#llx size=%#llx offset=%#llx\n",
                    (unsigned long long)data, (unsigned long long)capacity,
                    (unsigned long long)size, (unsigned long long)offset);
            runtime_fail("ACNH audout buffer bounds or frame alignment");
        }
        pthread_mutex_lock(&o->mu);
        if (o->nq == QCAP) { pthread_mutex_unlock(&o->mu); runtime_fail("ACNH audout buffer queue is full"); }
        /* 先限制所有未交还的 tag 总数，Stop/Flush 才不会在静态释放队列满时丢失它们。 */
        uint64_t outstanding = (uint64_t)o->nr + (uint64_t)o->nq + (o->has_inflight ? 1u : 0u);
        if (outstanding >= RELCAP) {
            pthread_mutex_unlock(&o->mu);
            runtime_fail("ACNH audout outstanding buffer limit reached");
        }
        o->q[(o->qh + o->nq) % QCAP] = pcm;
        o->nq++;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
        return;
    }
    case 4:
        rs->copy[rs->ncopy++] = runtime_event_readable(o->event_w);
        return;
    case 5: case 8: {
        int pointer = 0;
        IpcBuffer b = take_out(rq, &pointer);
        uint32_t room = b.size / 8u, count = 0;
        pthread_mutex_lock(&o->mu);
        if (b.size && room) {
            uint64_t *dst = ipc_guest(b.addr, room * 8u);
            while (count < room && o->nr) { dst[count++] = o->rel[0]; o->nr--; memmove(o->rel, o->rel + 1, (size_t)o->nr * sizeof o->rel[0]); }
        }
        int more = o->nr > 0;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
        if (pointer && b.size) rs->xout[rs->nxout++] = b;
        if (more) runtime_event_signal(o->event_w);
        IPC_OUT(rs, count);
        return;
    }
    case 6: {
        uint64_t tag = 0;
        if (rq->data_size >= 8) memcpy(&tag, rq->data, 8);
        pthread_mutex_lock(&o->mu);
        uint8_t yes = (uint8_t)contains_tag(o, tag);
        pthread_mutex_unlock(&o->mu);
        IPC_OUT(rs, yes);
        return;
    }
    case 9: {
        pthread_mutex_lock(&o->mu);
        uint32_t n = (uint32_t)o->nq + (o->busy ? 1u : 0u);
        pthread_mutex_unlock(&o->mu);
        IPC_OUT(rs, n);
        return;
    }
    case 10: { pthread_mutex_lock(&o->mu); uint64_t n = o->played; pthread_mutex_unlock(&o->mu); IPC_OUT(rs, n); return; }
    case 11: {
        pthread_mutex_lock(&o->mu);
        uint8_t pending = (uint8_t)(o->nq || o->busy);
        while (o->nq) { release_tag(o, o->q[o->qh].tag); o->qh = (o->qh + 1) % QCAP; o->nq--; }
        pthread_mutex_unlock(&o->mu);
        if (pending) runtime_event_signal(o->event_w);
        IPC_OUT(rs, pending);
        return;
    }
    case 12:
        if (rq->data_size < 4) runtime_fail("ACNH audout volume is not supported");
        pthread_mutex_lock(&o->mu);
        memcpy(&o->volume, rq->data, 4);
        pthread_mutex_unlock(&o->mu);
        return;
    case 13: { pthread_mutex_lock(&o->mu); float v = o->volume; pthread_mutex_unlock(&o->mu); IPC_OUT(rs, v); return; }
    default: ipc_unimplemented(session, rq);
    }
}

static void manager_handler(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: list_outs(rq, rs); return;
    case 1: case 3: open_out(rq, rs); return;
    default: ipc_unimplemented(session, rq);
    }
}

__attribute__((constructor)) static void register_audout(void) {
    runtime_ipc_register("audout:u", manager_handler, NULL, NULL);
}
#endif
