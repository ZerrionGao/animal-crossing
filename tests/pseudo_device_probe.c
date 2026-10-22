#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

static jmp_buf fail_env;
static int fail_armed;

uint64_t runtime_now_ns(void) { return 1; }
_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    if (fail_armed) longjmp(fail_env, 1);
    abort();
}
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }

int main(int argc, char **argv) {
    void (*get_id)(unsigned char *) = runtime_applet_override("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE");
    const char *mode = argc > 1 ? argv[1] : "write";
    if (!strcmp(mode, "absent")) {
        assert(!get_id);
        return 0;
    }
    assert(get_id);
    if (!strcmp(mode, "null")) {
        fail_armed = 1;
        if (!setjmp(fail_env)) {
            get_id(NULL);
            assert(!"null output must fail");
        }
        return 0;
    }
    unsigned char first[16], second[16];
    memset(first, 0xa5, sizeof first);
    get_id(first);
    if (!strcmp(mode, "write")) {
        get_id(second);
        assert(!memcmp(first, second, sizeof first));
        int nonzero = 0;
        for (size_t i = 0; i < sizeof first; i++) if (first[i]) nonzero = 1;
        assert(nonzero);
        assert((first[6] & 0xf0) == 0x40);
        assert((first[8] & 0xc0) == 0x80);
    }
    assert(fwrite(first, 1, sizeof first, stdout) == sizeof first);
    return 0;
}
