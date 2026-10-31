#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/runtime/runtime.h"
#include "../engine/runtime/fs_host.h"

typedef struct { uint64_t lo, hi; } ProbeUid;
typedef uint32_t (*ListUsersFn)(int *, ProbeUid *, int);

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    exit(42);
}

static const char *symbol = "_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi";

#ifdef GAME_ACNH
static void check_canaries(const ProbeUid values[8]) {
    for (int i = 1; i < 8; i++)
        assert(values[i].lo == 0xa1a1a1a1a1a1a1a1ULL && values[i].hi == 0xb2b2b2b2b2b2b2b2ULL);
}

static void check_persistent_uid(const ProbeUid *listed) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    assert(profile);
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/account.uid", profile);
    assert(n >= 0 && (size_t)n < sizeof path);
    FILE *file = fopen(path, "rb");
    assert(file);
    ProbeUid persisted;
    assert(fread(&persisted, 1, sizeof persisted, file) == sizeof persisted);
    assert(fgetc(file) == EOF);
    fclose(file);
    assert(listed->lo == persisted.lo && listed->hi == persisted.hi);
}
#endif

int main(void) {
    ListUsersFn list = (ListUsersFn)runtime_account_override(symbol);
#ifdef GAME_ACNH
    assert(list);
    if (getenv("TEST_TWO_RESIDENTS")) {
        ProbeUid users[8]; int count = 0;
        assert(list(&count, users, 8) == 0 && count == 2);
        assert((users[0].lo || users[0].hi) && (users[1].lo || users[1].hi));
        assert(users[0].lo != users[1].lo || users[0].hi != users[1].hi);
        for (int i = 2; i < 8; i++) assert(!users[i].lo && !users[i].hi);
        check_persistent_uid(&users[0]);
        uint64_t selected[2]; runtime_account_uid(selected);
        int which = getenv("ACNH_USER") && !strcmp(getenv("ACNH_USER"), "1");
        assert(selected[0] == users[which].lo && selected[1] == users[which].hi);
        return 0;
    }
    ProbeUid values[8];
    for (int i = 0; i < 8; i++) values[i] = (ProbeUid){0xa1a1a1a1a1a1a1a1ULL, 0xb2b2b2b2b2b2b2b2ULL};
    int count = -7;
    assert(list(&count, NULL, 0) == 0 && count == 1);
    count = -7;
    assert(list(&count, values, 0) == 0 && count == 1);
    assert(values[0].lo == 0xa1a1a1a1a1a1a1a1ULL && values[0].hi == 0xb2b2b2b2b2b2b2b2ULL);
    check_canaries(values);
    count = -7;
    assert(list(&count, NULL, -1) == 0 && count == 1);

    count = -7;
    assert(list(&count, values, 1) == 0 && count == 1);
    check_canaries(values);
    ProbeUid listed = values[0];
    assert(listed.lo || listed.hi);
    check_persistent_uid(&listed);

    /* 读档时游戏用未清零的 8 项数组接收，空槽必须是无效 Uid（全 0）。 */
    for (int i = 0; i < 8; i++) values[i] = (ProbeUid){0xa1a1a1a1a1a1a1a1ULL, 0xb2b2b2b2b2b2b2b2ULL};
    count = -7;
    assert(list(&count, values, 8) == 0 && count == 1);
    assert(values[0].lo == listed.lo && values[0].hi == listed.hi);
    for (int i = 1; i < 8; i++) assert(!values[i].lo && !values[i].hi);

    for (int i = 0; i < 8; i++) values[i] = (ProbeUid){0xa1a1a1a1a1a1a1a1ULL, 0xb2b2b2b2b2b2b2b2ULL};
    count = -7;
    assert(list(&count, values, 3) == 0 && count == 1);
    assert(values[0].lo == listed.lo && values[0].hi == listed.hi);
    assert(!values[1].lo && !values[1].hi && !values[2].lo && !values[2].hi);
    for (int i = 3; i < 8; i++)
        assert(values[i].lo == 0xa1a1a1a1a1a1a1a1ULL && values[i].hi == 0xb2b2b2b2b2b2b2b2ULL);

    count = -7;
    assert(list(NULL, values, 8) != 0);
    assert(list(&count, NULL, 1) != 0 && count == -7);
#elif defined(GAME_BOTW)
    assert(list);
    ProbeUid values[8];
    memset(values, 0xa5, sizeof values);
    int count = -7;
    assert(list(&count, NULL, 0) == 0 && count == 0);
    assert(list(&count, values, 8) == 0 && count == 1);
    assert(values[0].lo || values[0].hi);
#else
    assert(!list);
#endif
    return 0;
}
