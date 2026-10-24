/* NgWord2 Aho-Corasick 表。二进制布局按 Ryujinx NGC（MIT，gdkchan）核对，不复制词条。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct NgcAho NgcAho;

NgcAho *ngc_aho_load_gzip(const uint8_t *gzip, size_t gzip_size);
void ngc_aho_free(NgcAho *ac);
void ngc_aho_match(const NgcAho *ac, const uint8_t *text, size_t length,
                   int (*callback)(int start, int end, void *ctx), void *ctx);
/* 取出表中第一个词供测试，成功返回字节数；不打印正文。 */
int ngc_aho_first_word(const NgcAho *ac, uint8_t *out, size_t capacity);
