#include "acnh_msl_normalize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_EDITS = 64, MAX_RECEIVER = 180, MAX_ARGUMENT = 180, MAX_REPLACEMENT = 512 };
typedef struct { size_t at, length; char replacement[MAX_REPLACEMENT]; size_t replacement_length; } edit_t;

static const char *find_bytes(const char *s, size_t length, const char *needle, size_t needle_length) {
    if (needle_length > length) return NULL;
    for (size_t i = 0; i <= length - needle_length; i++)
        if (!memcmp(s + i, needle, needle_length)) return s + i;
    return NULL;
}

static int ident(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* 接收者可以是标识符链（tex.array），也可以是调用结果（spvDepthCast(tex).sample_compare）。
   只在点号后面才认括号时，调用结果的接收者长度是 0，整份着色器会被拒绝。 */
static size_t receiver_start(const char *s, size_t dot) {
    size_t p = dot;
    for (;;) {
        while (p && ident((unsigned char)s[p - 1])) p--;
        if (p && s[p - 1] == ')') {
            size_t q = p - 1, depth = 0;
            do {
                if (s[q] == ')') depth++;
                else if (s[q] == '(') depth--;
                if (!q) return 0;
                q--;
            } while (depth);
            p = q + 1;
            continue;
        }
        if (p && s[p - 1] == '.') { p--; continue; }
        break;
    }
    return p;
}

static int starts_trimmed(const char *s, size_t start, size_t end, const char *prefix) {
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) start++;
    size_t n = strlen(prefix);
    return end - start >= n && !memcmp(s + start, prefix, n);
}

static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
}

acnh_msl_result acnh_msl_normalize(const char *s, size_t length,
                                   char **output, size_t *output_length,
                                   char *error, size_t error_capacity) {
    if (output) *output = NULL;
    if (output_length) *output_length = 0;
    if (!s || !output) { set_error(error, error_capacity, "invalid normalizer arguments"); return ACNH_MSL_ERROR; }
    if (memchr(s, '\0', length)) { set_error(error, error_capacity, "MSL source contains an embedded NUL"); return ACNH_MSL_ERROR; }

    edit_t edits[MAX_EDITS]; size_t edit_count = 0;
    if (!find_bytes(s, length, "uint(rint(", sizeof("uint(rint(") - 1)) goto unchanged;
    for (size_t p = 0; p + 8 <= length;) {
        const char *found = find_bytes(s + p, length - p, ".sample", 7);
        if (!found) break;
        size_t at = (size_t)(found - s), call;
        if (at + 8 <= length && !memcmp(s + at, ".sample(", 8)) call = at + 8;
        else if (at + 16 <= length && !memcmp(s + at, ".sample_compare(", 16)) call = at + 16;
        else { p = at + 7; continue; }
        p = at + 7;

        size_t depth = 1, arg = 0, third_start = 0, third_end = 0;
        int have_third = 0, malformed = 0;
        for (size_t q = call; q < length; q++) {
            if (s[q] == '(') depth++;
            else if (s[q] == ')') {
                if (!depth) { malformed = 1; break; }
                depth--;
                if (!depth) {
                    if (arg == 2) { third_end = q; have_third = 1; }
                    break;
                }
            } else if (s[q] == ',' && depth == 1) {
                if (arg == 2) { third_end = q; have_third = 1; break; }
                arg++;
                if (arg == 2) third_start = q + 1;
            }
        }
        if (!have_third) {
            if (starts_trimmed(s, third_start, length, "uint(")) {
                set_error(error, error_capacity, malformed ? "malformed sample call parentheses" : "unterminated sample call");
                return ACNH_MSL_ERROR;
            }
            continue;
        }
        if (!starts_trimmed(s, third_start, third_end, "uint(")) continue;
        if (starts_trimmed(s, third_start, third_end, "nvn_layer(")) continue;
        size_t recv = receiver_start(s, at), recv_len = at - recv, arg_len = third_end - third_start;
        if (!recv_len || recv_len > MAX_RECEIVER || arg_len > MAX_ARGUMENT) {
            set_error(error, error_capacity, "sample receiver or layer argument exceeds normalizer limit");
            return ACNH_MSL_ERROR;
        }
        if (edit_count && third_start < edits[edit_count - 1].at + edits[edit_count - 1].length) {
            set_error(error, error_capacity, "nested sample layer arguments are not supported");
            return ACNH_MSL_ERROR;
        }
        if (edit_count == MAX_EDITS) {
            set_error(error, error_capacity, "normalizer edit limit exceeded");
            return ACNH_MSL_ERROR;
        }
        edit_t *e = &edits[edit_count];
        int n = snprintf(e->replacement, sizeof e->replacement,
                         "nvn_layer(%.*s, %.*s.get_array_size())",
                         (int)arg_len, s + third_start, (int)recv_len, s + recv);
        if (n < 0 || n >= MAX_REPLACEMENT) {
            set_error(error, error_capacity, "normalizer replacement exceeds limit");
            return ACNH_MSL_ERROR;
        }
        e->at = third_start; e->length = arg_len; e->replacement_length = (size_t)n; edit_count++;
    }
    if (!edit_count) goto unchanged;

    {
        static const char helper[] = "static inline uint nvn_layer(uint i, uint n) { return n ? min(i, n - 1u) : 0u; }\n";
        static const char helper_decl[] = "static inline uint nvn_layer(";
        static const char ns_decl[] = "using namespace metal;\n";
        size_t helper_at = 0; const char *ns = find_bytes(s, length, ns_decl, sizeof ns_decl - 1);
        if (ns) helper_at = (size_t)(ns - s) + sizeof ns_decl - 1;
        else if (find_bytes(s, length, helper_decl, sizeof helper_decl - 1)) helper_at = length;
        size_t extra = sizeof helper - 1;
        if (helper_at == length && find_bytes(s, length, helper_decl, sizeof helper_decl - 1)) extra = 0;
        size_t total = length + extra;
        for (size_t i = 0; i < edit_count; i++) {
            if (edits[i].replacement_length >= edits[i].length) total += edits[i].replacement_length - edits[i].length;
            else total -= edits[i].length - edits[i].replacement_length;
        }
        char *dst = malloc(total + 1);
        if (!dst) { set_error(error, error_capacity, "out of memory"); return ACNH_MSL_ERROR; }
        size_t si = 0, di = 0;
        for (size_t i = 0; i < edit_count; i++) {
            edit_t *e = &edits[i];
            if (helper_at >= si && helper_at <= e->at) {
                memcpy(dst + di, s + si, helper_at - si); di += helper_at - si; si = helper_at;
                if (extra) { memcpy(dst + di, helper, extra); di += extra; extra = 0; }
            }
            memcpy(dst + di, s + si, e->at - si); di += e->at - si;
            memcpy(dst + di, e->replacement, e->replacement_length); di += e->replacement_length;
            si = e->at + e->length;
        }
        if (helper_at >= si && helper_at <= length) {
            memcpy(dst + di, s + si, helper_at - si); di += helper_at - si; si = helper_at;
            if (extra) { memcpy(dst + di, helper, extra); di += extra; }
        }
        memcpy(dst + di, s + si, length - si); di += length - si; dst[di] = '\0';
        *output = dst; if (output_length) *output_length = di;
        return ACNH_MSL_CHANGED;
    }

unchanged:
    *output = malloc(length + 1);
    if (!*output) { set_error(error, error_capacity, "out of memory"); return ACNH_MSL_ERROR; }
    memcpy(*output, s, length); (*output)[length] = '\0';
    if (output_length) *output_length = length;
    return ACNH_MSL_UNCHANGED;
}
