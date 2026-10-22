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
    puts("ACNH joy assignment probe passed");
#else
    assert(!assignment);
    puts("non-ACNH target leaves joy assignment to the SDK");
#endif
    return 0;
}
