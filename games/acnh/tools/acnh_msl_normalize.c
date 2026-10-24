#include "../runtime/acnh_msl_normalize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_file(const char *path, char **data, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    *length = (size_t)end;
    *data = malloc(*length + 1);
    if (!*data) { fclose(f); return -1; }
    if (fread(*data, 1, *length, f) != *length) { free(*data); *data = NULL; fclose(f); return -1; }
    (*data)[*length] = '\0'; fclose(f); return 0;
}

int main(int argc, char **argv) {
    int policy = 0, flags = 0; const char *input = NULL, *output = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--policy-id=acnh-metal-v1")) policy = 1;
        else if (!strcmp(argv[i], "--metal-flags")) flags = 1;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
        else if (!input) input = argv[i];
        else if (!output) output = argv[i];
        else { fprintf(stderr, "expected at most INPUT OUTPUT\n"); return 2; }
    }
    if (!policy) { fprintf(stderr, "--policy-id=acnh-metal-v1 is required\n"); return 2; }
    if (flags) {
        if (input || output) { fprintf(stderr, "--metal-flags does not accept INPUT OUTPUT\n"); return 2; }
        puts("-fmetal-math-mode=safe -fpreserve-invariance"); return 0;
    }
    if (!input || !output) { fprintf(stderr, "usage: acnh_msl_normalize --policy-id=acnh-metal-v1 INPUT OUTPUT\n"); return 2; }
    char *source = NULL, *normalized = NULL, error[192] = {0}; size_t length = 0, out_length = 0;
    if (read_file(input, &source, &length)) { perror(input); return 1; }
    acnh_msl_result result = acnh_msl_normalize(source, length, &normalized, &out_length, error, sizeof error);
    free(source);
    if (result == ACNH_MSL_ERROR) { fprintf(stderr, "normalization failed: %s\n", error); return 1; }
    size_t path_length = strlen(output);
    char *temporary = malloc(path_length + 32);
    if (!temporary) { free(normalized); fprintf(stderr, "out of memory\n"); return 1; }
    snprintf(temporary, path_length + 32, "%s.tmp.%ld", output, (long)getpid());
    FILE *f = fopen(temporary, "wb");
    int failed = !f;
    if (f) {
        if (fwrite(normalized, 1, out_length, f) != out_length) failed = 1;
        if (fclose(f)) failed = 1;
    }
    if (!failed && rename(temporary, output)) failed = 1;
    if (failed) {
        perror(output); remove(temporary); free(normalized); free(temporary); return 1;
    }
    free(normalized); free(temporary);
    return 0;
}
