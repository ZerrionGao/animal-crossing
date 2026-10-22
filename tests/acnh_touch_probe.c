#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

static jmp_buf failure;
_Noreturn void runtime_fail(const char *message) {
    (void)message;
    longjmp(failure, 1);
}
void *runtime_hid_feedback_override(const char *name) { (void)name; return NULL; }

#if defined(GAME_ACNH) && TARGET_OS_OSX
typedef struct { int64_t sampling; int32_t count; uint32_t reserved; uint8_t touches[16 * 40]; } Touch16;
typedef struct { int64_t sampling; int32_t count; uint32_t reserved; uint8_t touches[40]; } Touch1;
_Static_assert(sizeof(Touch16) == 656 && sizeof(Touch1) == 56, "Switch touch-state ABI");

static void expect_failure(void (*call)(void)) {
    if (setjmp(failure) == 0) { call(); assert(!"expected runtime_fail"); }
}
static int (*get_many)(Touch16 *, int);
static void (*get_one)(Touch1 *);
static void read_before_init(void) { Touch16 out; (void)get_many(&out, 1); }
static void read_one_before_init(void) { Touch1 out; get_one(&out); }

int main(void) {
    const char *initialize_name = "_ZN2nn3hid21InitializeTouchScreenEv";
    const char *many_name = "_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi";
    const char *one_name = "_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE";
    void (*initialize)(void) = runtime_input_override(initialize_name);
    get_many = runtime_input_override(many_name);
    get_one = runtime_input_override(one_name);
    assert(initialize && get_many && get_one);

    struct { Touch16 states[4]; uint8_t canary[16]; } many;
    memset(&many, 0xa5, sizeof many);
    assert(get_many(NULL, 0) == 0);
    assert(get_many(NULL, -1) == 0);
    assert(many.canary[0] == 0xa5);
    expect_failure(read_before_init);
    expect_failure(read_one_before_init);

    initialize();
    assert(get_many(many.states, 1) == 1);
    assert(many.states[0].sampling > 0 && many.states[0].count == 0 && many.states[0].reserved == 0);
    for (size_t i = 0; i < sizeof many.states[0].touches; i++) assert(many.states[0].touches[i] == 0);
    for (size_t i = 1; i < 4; i++) for (size_t j = 0; j < sizeof many.states[i]; j++) assert(((uint8_t *)&many.states[i])[j] == 0xa5);
    for (size_t i = 0; i < sizeof many.canary; i++) assert(many.canary[i] == 0xa5);

    int64_t previous = many.states[0].sampling;
    memset(&many, 0xa5, sizeof many);
    assert(get_many(many.states, 4) == 1);
    assert(many.states[0].sampling == previous + 1 && many.states[0].count == 0 && many.states[0].reserved == 0);
    for (size_t i = 1; i < 4; i++) for (size_t j = 0; j < sizeof many.states[i]; j++) assert(((uint8_t *)&many.states[i])[j] == 0xa5);
    for (size_t i = 0; i < sizeof many.canary; i++) assert(many.canary[i] == 0xa5);

    struct { Touch1 state; uint8_t canary[16]; } one;
    memset(&one, 0xa5, sizeof one);
    get_one(&one.state);
    assert(one.state.sampling == previous + 2 && one.state.count == 0 && one.state.reserved == 0);
    for (size_t i = 0; i < sizeof one.state.touches; i++) assert(one.state.touches[i] == 0);
    for (size_t i = 0; i < sizeof one.canary; i++) assert(one.canary[i] == 0xa5);
    puts("ACNH touch HLE probe passed");
    return 0;
}
#else
int main(void) {
    assert(runtime_input_override("_ZN2nn3hid21InitializeTouchScreenEv") == NULL);
    assert(runtime_input_override("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi") == NULL);
    assert(runtime_input_override("_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE") == NULL);
    puts("non-ACNH target leaves touch services to the SDK");
    return 0;
}
#endif
