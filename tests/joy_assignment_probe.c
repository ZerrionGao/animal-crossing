#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

static jmp_buf failure;
static char failure_message[128];

_Noreturn void runtime_fail(const char *message) {
    snprintf(failure_message, sizeof failure_message, "%s", message ? message : "");
    longjmp(failure, 1);
}
void *runtime_hid_feedback_override(const char *name) { (void)name; return NULL; }

#ifdef GAME_ACNH
static int expect_failure(int (*call)(const uint32_t *), const uint32_t *id) {
    failure_message[0] = 0;
    if (setjmp(failure) == 0) {
        (void)call(id);
        return 0;
    }
    return 1;
}
#endif

int main(void) {
    const char *symbol = "_ZN2nn3hid20GetNpadJoyAssignmentERKj";
    int (*assignment)(const uint32_t *) = runtime_input_override(symbol);
#ifdef GAME_ACNH
    assert(assignment);
    const uint32_t ids[] = {0, 7, 0x10, 0x20};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) assert(assignment(&ids[i]) == 0);
    assert(assignment(&ids[0]) == 0);
    uint32_t unsupported = 8;
    assert(expect_failure(assignment, &unsupported));
    assert(strstr(failure_message, "not supported"));
    assert(expect_failure(assignment, NULL));
    assert(strstr(failure_message, "null"));

    uint32_t (*dual)(const uint32_t *) = runtime_input_override("_ZN2nn3hid6detail28SetNpadJoyAssignmentModeDualERKj");
    uint32_t (*single)(const uint32_t *) = runtime_input_override("_ZN2nn3hid6detail30SetNpadJoyAssignmentModeSingleERKj");
    uint32_t (*single_device)(const uint32_t *, uint32_t) =
        runtime_input_override("_ZN2nn3hid6detail30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE");
    uint32_t (*merge)(const uint32_t *, const uint32_t *) = runtime_input_override("_ZN2nn3hid6detail23MergeSingleJoyAsDualJoyERKjS3_");
    uint32_t (*swap)(const uint32_t *, const uint32_t *) = runtime_input_override("_ZN2nn3hid6detail18SwapNpadAssignmentERKjS3_");
    uint32_t (*color)(void *, void *, const uint32_t *) =
        runtime_input_override("_ZN2nn3hid6detail22GetNpadControllerColorEPNS0_19NpadControllerColorES3_RKj");
    uint32_t (*led)(uint8_t *, const uint32_t *) = runtime_input_override("_ZN2nn3hid6detail19GetPlayerLedPatternEPhRKj");
    uint32_t (*get_styles)(uint32_t *) =
        runtime_input_override("_ZN2nn3hid6detail24GetSupportedNpadStyleSetEPNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
    void (*set_styles)(uint32_t) = runtime_input_override("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
    assert(dual && single && single_device && merge && swap && color && led && get_styles && set_styles);

    assert(single(&ids[0]) == 0 && assignment(&ids[0]) == 1 && assignment(&ids[1]) == 0);
    assert(dual(&ids[0]) == 0 && assignment(&ids[0]) == 0);
    assert(single_device(&ids[3], 1) == 0 && assignment(&ids[3]) == 1);
    failure_message[0] = 0;
    if (setjmp(failure) == 0) { single_device(&ids[3], 2); assert(0); }
    assert(strstr(failure_message, "device type"));
    assert(merge(&ids[3], &ids[1]) == 0 && assignment(&ids[3]) == 0);
    assert(swap(&ids[1], &ids[0]) == 0);
    failure_message[0] = 0;
    if (setjmp(failure) == 0) { swap(&ids[0], &unsupported); assert(0); }
    assert(strstr(failure_message, "not supported"));

    uint32_t left = 0x11111111, right = 0x22222222;
    assert(color(&left, &right, &ids[0]) == 0x4b8ca);
    assert(left == 0x11111111 && right == 0x22222222);

    static const uint8_t pattern[8] = {0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6};
    for (uint32_t id = 0; id < 8; id++) {
        uint8_t value = 0xff;
        assert(led(&value, &id) == 0 && value == pattern[id]);
    }
    uint8_t handheld_led = 0xff;
    assert(led(&handheld_led, &ids[3]) == 0 && handheld_led == 0);

    uint32_t style_set = 0;
    set_styles(0x7);
    assert(get_styles(&style_set) == 0 && style_set == 0x7);
    puts("ACNH joy assignment probe passed");
#else
    assert(!assignment);
    puts("non-ACNH target leaves joy assignment to the SDK");
#endif
    return 0;
}
