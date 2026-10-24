#include <assert.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

static jmp_buf fail_env;
static int fail_armed;
typedef struct { uint32_t type; uint32_t reserved; const void *address; size_t size; } SharedFontData;
uint64_t runtime_now_ns(void) { return 1; }
_Noreturn void runtime_fail(const char *message) {
    (void)message;
    if (getenv("PROBE_EXPECT_FATAL")) exit(70);
    if (fail_armed) longjmp(fail_env, 1);
    abort();
}

int main(int argc, char **argv) {
    int (*load_state)(int) = runtime_font_override("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE");
    const void *(*address)(int) = runtime_font_override("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE");
    size_t (*size)(int) = runtime_font_override("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE");
    void (*request_load)(int) = runtime_font_override("_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE");
    unsigned char (*get_fonts)(int *, SharedFontData *, int, uint64_t) = runtime_font_override("_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE");
    assert(load_state && address && size && request_load && get_fonts);
    assert(sizeof(SharedFontData) == 0x18 && offsetof(SharedFontData, address) == 8 && offsetof(SharedFontData, size) == 16);

    fail_armed = 1;
    if (!setjmp(fail_env)) { request_load(-1); assert(!"negative font type must fail"); }
    if (!setjmp(fail_env)) { request_load(6); assert(!"out-of-range font type must fail"); }
    if (!setjmp(fail_env)) { int count; (void)get_fonts(&count, NULL, 6, 0); assert(!"unverified font language must fail"); }
    if (!setjmp(fail_env)) { int count; (void)get_fonts(&count, NULL, 1, 0x00000053552d6e65ull); assert(!"positive max requires an output buffer"); }
    if (!setjmp(fail_env)) { (void)get_fonts(NULL, NULL, 0, 0x00000053552d6e65ull); assert(!"null font count must fail"); }
    fail_armed = 0;

    int real_fonts = argc > 1 && !strcmp(argv[1], "real");
    if (real_fonts) {
        request_load(0);
    } else if (argc > 1 && !strcmp(argv[1], "count-first")) {
        int count = -1;
        assert(get_fonts(&count, NULL, 0, 0x00000053552d6e65ull) && count == 6);
        assert(setenv("TOTK_SHARED_FONTS", "/missing-after-request", 1) == 0);
    } else if (argc > 1 && !strcmp(argv[1], "request-first")) {
        request_load(2);
        assert(setenv("TOTK_SHARED_FONTS", "/missing-after-request", 1) == 0);
    } else {
        assert(address(0) != NULL); /* 首次地址查询也会触发初始化。 */
        request_load(2);
        assert(setenv("TOTK_SHARED_FONTS", "/missing-after-request", 1) == 0);
    }

    static const size_t sizes[6] = {8, 12, 16, 20, 24, 28};
    const uint8_t *addresses[6];
    for (int i = 0; i < 6; i++) {
        assert(load_state(i) == 1);
        addresses[i] = address(i);
        size_t font_size = size(i);
        if (real_fonts) {
            assert(font_size >= 4 && font_size % 4 == 0);
            assert(!memcmp(addresses[i], "\x00\x01\x00\x00", 4) || !memcmp(addresses[i], "OTTO", 4));
        } else {
            assert(font_size == sizes[i]);
            static const uint8_t sfnt[4] = {0, 1, 0, 0};
            assert(!memcmp(addresses[i], sfnt, sizeof sfnt));
            assert(addresses[i][4] == (uint8_t)('A' + i));
        }
    }
    for (int i = 0; i < 5; i++) assert(addresses[i + 1] - addresses[i] == (ptrdiff_t)(size(i) + 8));

    int count = -1;
    assert(get_fonts(&count, NULL, 0, 0x00000053552d6e65ull) && count == 6);
    assert(get_fonts(&count, NULL, -1, 0x00000053552d6e65ull) && count == 6);
    SharedFontData listed[6];
    memset(listed, 0xa5, sizeof listed);
    assert(get_fonts(&count, listed, 2, 0x00000053552d6e65ull) && count == 6);
    for (int i = 0; i < 2; i++) {
        assert(listed[i].type == (uint32_t)i && listed[i].reserved == 0);
        assert(listed[i].address == addresses[i] && listed[i].size == size(i));
    }
    for (size_t i = 0; i < sizeof listed[2]; i++) assert(((const uint8_t *)&listed[2])[i] == 0xa5);
    assert(get_fonts(&count, listed, 6, 0x00000053552d6e65ull) && count == 6);
    for (int i = 0; i < 6; i++) {
        assert(listed[i].type == (uint32_t)i && listed[i].reserved == 0);
        assert(listed[i].address == addresses[i] && listed[i].size == size(i));
    }
    uint64_t zh_hans = 0;
    memcpy(&zh_hans, "zh-Hans", 7);
    static const int zh_order[6] = {1, 2, 0, 3, 4, 5};
    assert(get_fonts(&count, listed, 6, zh_hans) && count == 6);
    for (int i = 0; i < 6; i++) {
        int type = zh_order[i];
        assert(listed[i].type == (uint32_t)type && listed[i].reserved == 0);
        assert(listed[i].address == addresses[type] && listed[i].size == size(type));
    }

    fail_armed = 1;
    if (!setjmp(fail_env)) { (void)load_state(6); assert(!"out-of-range font type must fail"); }
    if (!setjmp(fail_env)) { (void)address(-1); assert(!"negative font type must fail"); }
    fail_armed = 0;
    return 0;
}
