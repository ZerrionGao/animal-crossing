#include "../games/acnh/runtime/acnh_msl_normalize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(int ok, const char *name) {
    if (!ok) fprintf(stderr, "FAIL: %s\n", name);
    return !ok;
}

static int normalize(const char *source, acnh_msl_result expected, char **out) {
    char error[192] = {0}; size_t length = 0;
    acnh_msl_result result = acnh_msl_normalize(source, strlen(source), out, &length, error, sizeof error);
    if (result != expected) fprintf(stderr, "unexpected result %d: %s\n", result, error);
    return result == expected;
}

static size_t count_text(const char *text, const char *needle) {
    size_t count = 0, length = strlen(needle);
    for (const char *p = text; (p = strstr(p, needle)); p += length) count++;
    return count;
}

int main(void) {
    int failed = 0; char *out = NULL;
    const char *nested = "using namespace metal;\nfloat4 f() { return get_tex(i).array.sample(s, coord(float(rint(x))), uint(rint(layer + (delta)))); }\n";
    failed |= !normalize(nested, ACNH_MSL_CHANGED, &out);
    failed |= check(out && strstr(out, "nvn_layer( uint(rint(layer + (delta))), get_tex(i).array.get_array_size())"), "nested receiver and argument");
    char *again = NULL;
    failed |= !normalize(out ? out : "", ACNH_MSL_UNCHANGED, &again);
    failed |= check(again && !strcmp(out, again), "normalization is idempotent");
    free(out); free(again); out = NULL;

    failed |= !normalize("tex.sample(s, c, uint(rint(-x)))", ACNH_MSL_CHANGED, &out);
    failed |= check(out && strstr(out, "nvn_layer( uint(rint(-x)), tex.get_array_size())"), "preserve conversion before clamp");
    free(out); out = NULL;

    const char *compare = "using namespace metal;\nfloat f() { return tex.sample_compare(s, c, uint(rint(layer)), 0.5); }\n";
    failed |= !normalize(compare, ACNH_MSL_CHANGED, &out);
    failed |= check(out && strstr(out, "nvn_layer( uint(rint(layer)), tex.get_array_size())"), "sample_compare third argument");
    failed |= check(out && strstr(out, "static inline uint nvn_layer(uint i, uint n)"), "helper inserted");
    free(out); out = NULL;

    const char *cast_receiver = "using namespace metal;\nfloat f() { return spvDepthCast(spvDescriptorSet0.cp_t_tcb_20).sample_compare(spvDescriptorSet0.cp_t_tcb_20Smplr, c.xy, uint(rint(c.z)), ref, level(0.0)); }\n";
    failed |= !normalize(cast_receiver, ACNH_MSL_CHANGED, &out);
    failed |= check(out && strstr(out, "nvn_layer( uint(rint(c.z)), spvDepthCast(spvDescriptorSet0.cp_t_tcb_20).get_array_size())"), "call receiver");
    free(out); out = NULL;

    size_t cap = 8192, used = 0;
    char *many = malloc(cap); if (!many) return 2;
    used += (size_t)snprintf(many, cap, "using namespace metal;\n");
    for (int i = 0; i < 64; i++) used += (size_t)snprintf(many + used, cap - used, "t%d.sample(s,c,uint(rint(%d)));\n", i, i);
    failed |= !normalize(many, ACNH_MSL_CHANGED, &out);
    failed |= check(out && count_text(out, "nvn_layer(uint(rint(") == 64, "all 64 edits are applied");
    free(out); out = NULL;
    used += (size_t)snprintf(many + used, cap - used, "t64.sample(s,c,uint(rint(64)));\n");
    failed |= !normalize(many, ACNH_MSL_ERROR, &out);
    failed |= check(out == NULL, "65 edits fail without partial output");
    free(many);

    failed |= !normalize("t.sample(s,c,uint(rint(layer)", ACNH_MSL_ERROR, &out);
    failed |= check(out == NULL, "malformed call fails without fallback");

    char *long_arg = malloc(256); if (!long_arg) return 2;
    strcpy(long_arg, "t.sample(s,c,uint("); memset(long_arg + strlen(long_arg), 'x', 181);
    size_t used_arg = strlen("t.sample(s,c,uint(") + 181;
    strcpy(long_arg + used_arg, "))\nuint(rint(1))");
    failed |= !normalize(long_arg, ACNH_MSL_ERROR, &out);
    failed |= check(out == NULL, "oversized argument fails");
    free(long_arg);

    char long_receiver[512]; memset(long_receiver, 'r', 181);
    strcpy(long_receiver + 181, ".sample(s,c,uint(rint(layer)))");
    failed |= !normalize(long_receiver, ACNH_MSL_ERROR, &out);
    failed |= check(out == NULL, "oversized receiver fails");

    failed |= !normalize("a.sample(s, c, uint(rint(b.sample(s2, c2, uint(rint(z))).x)));", ACNH_MSL_ERROR, &out);
    failed |= check(out == NULL, "nested layer arguments fail without overlapping edits");

    free(out);
    if (failed) return 1;
    puts("ACNH MSL normalizer CPU tests passed");
    return 0;
}
