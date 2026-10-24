#include "../third_party/macnativegames/games/totk/runtime/ngc_aho.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int on_match(int start, int end, void *ctx) {
    int *found = ctx;
    if (start >= 0 && end > start) *found += 1;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *gzip = malloc((size_t)n);
    assert(gzip && fread(gzip, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    NgcAho *ac = ngc_aho_load_gzip(gzip, (size_t)n);
    free(gzip);
    if (!ac) { fprintf(stderr, "import failed\n"); return 1; }
    uint8_t word[256];
    int wlen = ngc_aho_first_word(ac, word, sizeof word);
    if (wlen <= 0) { fprintf(stderr, "no first word\n"); ngc_aho_free(ac); return 1; }
    int hits = 0;
    ngc_aho_match(ac, word, (size_t)wlen, on_match, &hits);
    const char *sentinel = "PRIVATE_PLAYER_TEXT_SENTINEL";
    int other = 0;
    ngc_aho_match(ac, (const uint8_t *)sentinel, strlen(sentinel), on_match, &other);
    fprintf(stderr, "first-word-bytes=%d first-word-hits=%d sentinel-hits=%d\n", wlen, hits, other);
    ngc_aho_free(ac);
    return hits > 0 && other == 0 ? 0 : 1;
}
