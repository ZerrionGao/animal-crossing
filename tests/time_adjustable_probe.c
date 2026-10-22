#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

#ifdef GAME_ACNH
static _Atomic int64_t fake_wall_ns;
static _Atomic int64_t fake_continuous_ns;

int64_t acnh_time_test_realtime_ns(void) { return atomic_load(&fake_wall_ns); }
int64_t acnh_time_test_continuous_ns(void) { return atomic_load(&fake_continuous_ns); }
#endif

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    exit(42);
}

typedef unsigned (*InitializeFn)(void);
typedef unsigned (*FinalizeFn)(void);
typedef unsigned char (*IsInitializedFn)(void);
typedef void (*AdjustFn)(void);
typedef uint32_t (*GetCurrentTimeFn)(int64_t *);
#ifdef GAME_ACNH
typedef struct {
    InitializeFn initialize;
    FinalizeFn finalize;
    IsInitializedFn is_initialized;
    GetCurrentTimeFn host_time;
    AdjustFn adjust;
    GetCurrentTimeFn adjustable_time;
} RefOps;

static void *exercise_refcounts(void *arg) {
    RefOps *ops = arg;
    for (unsigned i = 0; i < 4000; i++) {
        assert(ops->initialize() == 0);
        assert(ops->is_initialized());
        int64_t host_time = 0;
        assert(ops->host_time(&host_time) == 0 && host_time > 0);
        ops->adjust();
        int64_t adjustable_time = 0;
        assert(ops->adjustable_time(&adjustable_time) == 0 && adjustable_time > 0);
        assert(ops->finalize() == 0);
    }
    return NULL;
}
#endif

int main(int argc, char **argv) {
    AdjustFn adjust = (AdjustFn)runtime_time_override("_ZN2nn4time25AdjustableUserSystemClock6AdjustEv");
    GetCurrentTimeFn get_time = (GetCurrentTimeFn)runtime_time_override(
        "_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
#ifdef GAME_ACNH
    InitializeFn initialize = (InitializeFn)runtime_time_override("_ZN2nn4time10InitializeEv");
    FinalizeFn finalize = (FinalizeFn)runtime_time_override("_ZN2nn4time8FinalizeEv");
    IsInitializedFn is_initialized = (IsInitializedFn)runtime_time_override("_ZN2nn4time13IsInitializedEv");
    GetCurrentTimeFn host_time = (GetCurrentTimeFn)runtime_time_override(
        "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    assert(adjust && get_time && initialize && finalize && is_initialized && host_time);

    if (argc > 1 && strcmp(argv[1], "adjust-before-init") == 0) {
        adjust();
        assert(!"Adjust before Initialize must fail");
    }
    if (argc > 1 && strcmp(argv[1], "reinit-auto-adjust") == 0) {
        atomic_store(&fake_wall_ns, 1700000000900000000LL);
        atomic_store(&fake_continuous_ns, 100000000000LL);
        assert(initialize() == 0);
        int64_t before_finalize = 0;
        assert(get_time(&before_finalize) == 0 && before_finalize == 1700000060LL);
        assert(finalize() == 0);
        atomic_store(&fake_wall_ns, 1800000000900000000LL);
        atomic_fetch_add(&fake_continuous_ns, 3000000000LL);
        assert(initialize() == 0);
        int64_t after_reinitialize = 0;
        assert(get_time(&after_reinitialize) == 0 && after_reinitialize == 1800000060LL);
        assert(finalize() == 0);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "null-output") == 0) {
        assert(initialize() == 0);
        adjust();
        (void)get_time(NULL);
        assert(!"GetCurrentTime with null output must fail");
    }

    const int64_t wall_before = 1700000000900000000LL;
    const int64_t continuous_before = 100000000000LL;
    atomic_store(&fake_wall_ns, wall_before);
    atomic_store(&fake_continuous_ns, continuous_before);

    RefOps ops = {initialize, finalize, is_initialized, host_time, adjust, get_time};
    int64_t value = 0x123456789;
    assert(is_initialized() == 0);
    assert(get_time(&value) == 0x74u && value == 0x123456789);
    assert(initialize() == 0 && is_initialized());
    assert(get_time(&value) == 0 && value == 1700000060LL);

    assert(initialize() == 0 && is_initialized());
    atomic_store(&fake_wall_ns, 2000000000100000000LL);
    atomic_fetch_add(&fake_continuous_ns, 2100000000LL);
    assert(get_time(&value) == 0 && value == 1700000063LL);

    adjust();
    assert(get_time(&value) == 0 && value == 2000000060LL);

    /* 连续时间包含睡眠；模拟经过两小时并跨过秒边界。 */
    atomic_fetch_add(&fake_continuous_ns, 7200200000000LL);
    assert(get_time(&value) == 0 && value == 2000007260LL);

    /* 主机墙钟变化要等 SDK 再次调用 Adjust 才会采用。 */
    atomic_store(&fake_wall_ns, 2100000000100000000LL);
    atomic_fetch_add(&fake_continuous_ns, 500000000LL);
    assert(get_time(&value) == 0 && value == 2000007260LL);
    adjust();
    assert(get_time(&value) == 0 && value == 2100000060LL);
    atomic_fetch_add(&fake_continuous_ns, 1200000000LL);
    assert(get_time(&value) == 0 && value == 2100000061LL);

    pthread_t workers[8];
    for (unsigned i = 0; i < 8; i++) assert(!pthread_create(&workers[i], NULL, exercise_refcounts, &ops));
    for (unsigned i = 0; i < 8; i++) assert(!pthread_join(workers[i], NULL));

    assert(finalize() == 0 && is_initialized());
    assert(get_time(&value) == 0);
    assert(finalize() == 0 && !is_initialized());
    value = 0x123456789;
    assert(get_time(&value) == 0x74u && value == 0x123456789);
    assert(finalize() == 0x74u && !is_initialized());

    assert(!is_initialized());
#else
    (void)argc;
    (void)argv;
    assert(!adjust && !get_time);
    assert(runtime_time_override("_ZN2nn4time10InitializeEv"));
    assert(runtime_time_override("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE"));
#endif
    return 0;
}
