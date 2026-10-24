/* NgWord2 Aho-Corasick。表格式依据 Ryujinx NGC（MIT）。不记录、不打印词条正文。 */
#include "ngc_aho.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { NGC_BITS = 32, NGC_RANK1_ENTRIES = 8, NGC_MAX_UNCOMPRESSED = 64, NGC_MAX_PLAIN = 64 * 1024 * 1024 };

typedef struct {
    const uint8_t *p;
    size_t n, o;
} NgcR;

typedef struct {
    int bit_length;
    uint32_t *words;
} NgcBv;

typedef struct {
    uint32_t *rank0;
    uint8_t *rank1;
} NgcSbvRank;

typedef struct {
    NgcBv bv;
    NgcSbvRank rank;
} NgcSet;

typedef struct {
    uint32_t *array;
    unsigned array_len;
    NgcBv bv1, bv2;
    NgcSbvRank rank1, rank2;
} NgcSbvSelect;

typedef struct {
    NgcSet set;
    NgcSbvSelect select;
} NgcSbv;

typedef struct {
    uint64_t range_values, range_start, range_end;
    uint32_t count, bitfield_length;
    uint32_t *bitfields;
    unsigned bitfields_len;
    NgcSbv sbv;
} NgcSparse;

typedef struct {
    uint32_t *ranges;
    unsigned ranges_len;
    uint32_t *bitfields;
    unsigned bitfields_len;
    int32_t uncompressed[NGC_MAX_UNCOMPRESSED];
    unsigned uncompressed_len;
} NgcCArr;

typedef struct NgcBpNode {
    NgcSet set;
    NgcSparse *sparse;
    struct NgcBpNode *next;
} NgcBpNode;

typedef struct {
    NgcBpNode first;
    NgcSbvSelect select;
} NgcBp;

struct NgcAho {
    NgcCArr word_lengths;
    NgcSparse word_map;
    NgcCArr multi_indices;
    NgcSparse multi_map;
    uint32_t nodes_per_character;
    NgcSparse node_map;
    NgcBp bp;
};

static int r_bytes(NgcR *r, void *out, size_t n) {
    if (r->o + n > r->n) return 0;
    if (out) memcpy(out, r->p + r->o, n);
    r->o += n;
    return 1;
}
static int r_u8(NgcR *r, uint8_t *v) { return r_bytes(r, v, 1); }
static int r_i32(NgcR *r, int32_t *v) { return r_bytes(r, v, 4); }
static int r_u32(NgcR *r, uint32_t *v) { return r_bytes(r, v, 4); }
static int r_u64(NgcR *r, uint64_t *v) { return r_bytes(r, v, 8); }
static int r_alloc(NgcR *r, void **out, size_t elem, int count) {
    if (count < 0) return 0;
    if (!count) { *out = NULL; return 1; }
    size_t n = (size_t)count * elem;
    if (n / elem != (size_t)count) return 0;
    void *p = malloc(n);
    if (!p) return 0;
    if (!r_bytes(r, p, n)) { free(p); return 0; }
    *out = p;
    return 1;
}

static int popc(uint32_t v) { return __builtin_popcount(v); }
static int select_pos(uint32_t bits, int bit_index) {
    int bit, remain = bit_index;
    for (bit = 0; bit < 32;) {
        if ((bits >> bit) & 1u) {
            if (remain-- == 0) break;
            bit++;
        } else {
            uint32_t rest = bits >> bit;
            bit += rest ? __builtin_ctz(rest) : 32;
        }
    }
    return bit;
}

static int bv_import(NgcR *r, NgcBv *bv) {
    memset(bv, 0, sizeof *bv);
    if (!r_i32(r, &bv->bit_length) || bv->bit_length < 0) return 0;
    int n = (bv->bit_length + NGC_BITS - 1) / NGC_BITS;
    return r_alloc(r, (void **)&bv->words, 4, n);
}
static int bv_has(const NgcBv *bv, int index) {
    if ((unsigned)index >= (unsigned)bv->bit_length) return 0;
    return (bv->words[index / NGC_BITS] >> (index % NGC_BITS)) & 1u;
}
static void bv_free(NgcBv *bv) { free(bv->words); bv->words = NULL; }

static int rank0_len(int cap) { return (cap / (NGC_BITS * NGC_RANK1_ENTRIES)) + 1; }
static int rank1_len(int cap) { return (cap / NGC_BITS) + 1; }
static int rank_import(NgcR *r, NgcSbvRank *rk, int cap) {
    memset(rk, 0, sizeof *rk);
    if (!cap) return 1;
    return r_alloc(r, (void **)&rk->rank0, 4, rank0_len(cap)) &&
           r_alloc(r, (void **)&rk->rank1, 1, rank1_len(cap));
}
static int rank1_at(const NgcSbvRank *rk, int index, const uint32_t *bitmap) {
    int i0 = index / (NGC_BITS * NGC_RANK1_ENTRIES), i1 = index / NGC_BITS;
    uint32_t bits = bitmap[i1] & (0xffffffffu >> (NGC_BITS - 1 - (index % NGC_BITS)));
    return (int)rk->rank0[i0] + rk->rank1[i1] + popc(bits);
}
static void rank_free(NgcSbvRank *rk) { free(rk->rank0); free(rk->rank1); rk->rank0 = NULL; rk->rank1 = NULL; }

static int set_import(NgcR *r, NgcSet *s) {
    memset(s, 0, sizeof *s);
    return bv_import(r, &s->bv) && rank_import(r, &s->rank, s->bv.bit_length);
}
static int set_has(const NgcSet *s, int index) { return bv_has(&s->bv, index); }
static int set_rank1(const NgcSet *s, int index) {
    if ((unsigned)index >= (unsigned)s->bv.bit_length) index = s->bv.bit_length - 1;
    if (index < 0) return 0;
    return rank1_at(&s->rank, index, s->bv.words);
}
static void set_free(NgcSet *s) { bv_free(&s->bv); rank_free(&s->rank); }

static int sel_import(NgcR *r, NgcSbvSelect *s) {
    memset(s, 0, sizeof *s);
    int32_t n;
    if (!r_i32(r, &n) || n < 0) return 0;
    s->array_len = (unsigned)n;
    if (!r_alloc(r, (void **)&s->array, 4, n)) return 0;
    return bv_import(r, &s->bv1) && bv_import(r, &s->bv2) &&
           rank_import(r, &s->rank1, s->bv1.bit_length) &&
           rank_import(r, &s->rank2, s->bv2.bit_length);
}
static int sel_select(const NgcSbvSelect *s, const NgcSet *set, int index) {
    if (index < 0 || index >= s->bv2.bit_length) return -1;
    int r1 = rank1_at(&s->rank2, index, s->bv2.words);
    int r0 = rank1_at(&s->rank1, r1 - 1, s->bv1.words);
    int value = (int)s->array[r0 - 1] + (r1 - 1);
    int base = 0;
    if (value) {
        base = value * 32;
        int bounded = base - 1;
        if (bounded >= set->bv.bit_length) bounded = set->bv.bit_length - 1;
        index -= set_rank1(set, bounded);
    }
    return select_pos(set->bv.words[value], index) + base;
}
static void sel_free(NgcSbvSelect *s) {
    free(s->array); s->array = NULL;
    bv_free(&s->bv1); bv_free(&s->bv2); rank_free(&s->rank1); rank_free(&s->rank2);
}

static int sbv_import(NgcR *r, NgcSbv *s) { return set_import(r, &s->set) && sel_import(r, &s->select); }
static void sbv_free(NgcSbv *s) { set_free(&s->set); sel_free(&s->select); }

static int sparse_import(NgcR *r, NgcSparse *s) {
    memset(s, 0, sizeof *s);
    int32_t n;
    if (!r_u64(r, &s->range_values) || !r_u64(r, &s->range_start) || !r_u64(r, &s->range_end) ||
        !r_u32(r, &s->count) || !r_u32(r, &s->bitfield_length) || !r_i32(r, &n) || n < 0)
        return 0;
    s->bitfields_len = (unsigned)n;
    return r_alloc(r, (void **)&s->bitfields, 4, n) && sbv_import(r, &s->sbv);
}
static long sparse_select_ex(const NgcSparse *s, int index) {
    if ((unsigned)index >= s->count) return -1;
    int offset = sel_select(&s->sbv.select, &s->sbv.set, index);
    int width = (int)s->bitfield_length;
    int bit = index * width, word = bit / NGC_BITS, shift = bit % NGC_BITS;
    uint64_t value = s->bitfields[word];
    if (shift + width > NGC_BITS && (unsigned)(word + 1) < s->bitfields_len)
        value |= (uint64_t)s->bitfields[word + 1] << 32;
    value >>= shift;
    if (width >= 32) value &= 0xffffffffu;
    else value &= 0xffffffffu >> (NGC_BITS - width);
    return ((long)(offset - index) << width) + (int)value;
}
static int sparse_rank1(const NgcSparse *s, long index) {
    uint32_t count = s->count;
    if ((uint64_t)index < s->range_start || !count) return 0;
    if (s->range_start == (uint64_t)index || count < 3) return 1;
    if (s->range_end <= (uint64_t)index) return (int)count;
    int left = 0, right = (int)count - 1;
    for (;;) {
        int range = right - left;
        if (range < 0) range++;
        int mid = left + range / 2;
        long found = sparse_select_ex(s, mid);
        if ((uint64_t)found <= (uint64_t)index) left = mid;
        else right = mid;
        if (right <= left + 1) break;
    }
    return left + 1;
}
static int sparse_has(const NgcSparse *s, long index) {
    int plain = sparse_rank1(s, index);
    return plain && sparse_select_ex(s, plain - 1) == index;
}
static void sparse_free(NgcSparse *s) { free(s->bitfields); s->bitfields = NULL; sbv_free(&s->sbv); }

static int carr_import(NgcR *r, NgcCArr *a) {
    memset(a, 0, sizeof *a);
    int32_t n;
    if (!r_i32(r, &n) || n < 0) return 0;
    a->ranges_len = (unsigned)n;
    if (!r_alloc(r, (void **)&a->ranges, 4, n)) return 0;
    if (!r_i32(r, &n) || n < 0) return 0;
    a->bitfields_len = (unsigned)n;
    if (!r_alloc(r, (void **)&a->bitfields, 4, n)) return 0;
    uint8_t un = 0;
    if (!r_u8(r, &un) || un > NGC_MAX_UNCOMPRESSED) return 0;
    a->uncompressed_len = un;
    return !un || r_bytes(r, a->uncompressed, (size_t)un * 4);
}
static int carr_get(const NgcCArr *a, int index) {
    unsigned blocks = a->ranges_len / 2;
    int block = index / NGC_MAX_UNCOMPRESSED;
    if ((unsigned)block < blocks) {
        uint32_t range = a->ranges[block * 2];
        int base = (int)a->ranges[block * 2 + 1];
        int bf_index = (int)(range & 0x7ffffffu);
        int width = (int)(range >> 27) + 1;
        int bit = (index % NGC_MAX_UNCOMPRESSED) * width;
        int word = bf_index + bit / NGC_BITS, shift = bit % NGC_BITS;
        uint64_t v = a->bitfields[word];
        if (shift + width > NGC_BITS && (unsigned)(word + 1) < a->bitfields_len)
            v |= (uint64_t)a->bitfields[word + 1] << 32;
        int value = (int)((v >> shift) & (width >= 31 ? 0x7fffffffu : ((1u << width) - 1u)));
        if (width < 32) {
            int rem = NGC_BITS - width;
            value = (value << rem) >> rem;
        }
        return value + base;
    }
    if (block < (int)a->uncompressed_len + (int)a->ranges_len * NGC_BITS)
        return a->uncompressed[index % NGC_MAX_UNCOMPRESSED];
    return 0;
}
static void carr_free(NgcCArr *a) { free(a->ranges); free(a->bitfields); a->ranges = a->bitfields = NULL; }

static int bp_node_import(NgcR *r, NgcBpNode *n) {
    memset(n, 0, sizeof *n);
    if (!set_import(r, &n->set)) return 0;
    uint8_t has = 0;
    if (!r_u8(r, &has)) return 0;
    if (!has) return 1;
    n->sparse = calloc(1, sizeof *n->sparse);
    n->next = calloc(1, sizeof *n->next);
    if (!n->sparse || !n->next) return 0;
    return sparse_import(r, n->sparse) && bp_node_import(r, n->next);
}
static int bp_find_open(const NgcBpNode *n, int index);
static int bp_enclose(const NgcBpNode *n, int index);
static int bp_find_open(const NgcBpNode *n, int index) {
    uint32_t bits = n->set.bv.words[index / NGC_BITS];
    int off = index % NGC_BITS, unset = 1;
    for (int bit = off - 1; bit >= 0; bit--) {
        if ((bits >> bit) & 1u) { if (--unset == 0) return (index & ~(NGC_BITS - 1)) | bit; }
        else unset++;
    }
    if (!n->sparse) return -1;
    int plain = sparse_rank1(n->sparse, index);
    if (!plain) return -1;
    int neu = index;
    if (!sparse_has(n->sparse, index)) {
        if (!n->next) return -1;
        neu = (int)sparse_select_ex(n->sparse, plain);
        if (neu < 0) return -1;
    } else plain--;
    int open = bp_find_open(n->next, plain);
    if (open < 0) return -1;
    int open_s = (int)sparse_select_ex(n->sparse, open);
    if (open_s < 0) return -1;
    if (neu != index) {
        unset = 1;
        for (int bit = neu % NGC_BITS - 1; bit > off; bit--)
            unset += ((bits >> bit) & 1u) ? -1 : 1;
        int best = -1;
        bits = n->set.bv.words[open_s / NGC_BITS];
        for (int bit = open_s % NGC_BITS + 1; bit < NGC_BITS; bit++) {
            if (unset - 1 == 0) best = bit;
            unset += ((bits >> bit) & 1u) ? -1 : 1;
        }
        return (open_s & ~(NGC_BITS - 1)) | best;
    }
    return open_s;
}
static int bp_enclose(const NgcBpNode *n, int index) {
    uint32_t bits = n->set.bv.words[index / NGC_BITS];
    int unset = 1;
    for (int bit = index % NGC_BITS - 1; bit >= 0; bit--) {
        if ((bits >> bit) & 1u) { if (--unset == 0) return (index & ~(NGC_BITS - 1)) + bit; }
        else unset++;
    }
    int set_bits = 2;
    for (int bit = index % NGC_BITS + 1; bit < NGC_BITS; bit++) {
        if ((bits >> bit) & 1u) set_bits++;
        else if (--set_bits == 0) return bp_find_open(n, (index & ~(NGC_BITS - 1)) + bit);
    }
    if (!n->sparse || !n->next) return -1;
    int neu = index;
    if (!sparse_has(n->sparse, index)) {
        neu = (int)sparse_select_ex(n->sparse, sparse_rank1(n->sparse, index));
        if (neu < 0) return -1;
    }
    if (!set_has(&n->set, neu)) {
        neu = bp_find_open(n, neu);
        if (neu < 0) return -1;
    } else {
        neu = bp_enclose(n->next, sparse_rank1(n->sparse, neu) - 1);
        if (neu < 0) return -1;
        neu = (int)sparse_select_ex(n->sparse, neu);
    }
    int nearest = (int)sparse_select_ex(n->sparse, sparse_rank1(n->sparse, neu));
    if (nearest < 0) return -1;
    set_bits = 0;
    bits = n->set.bv.words[neu / NGC_BITS];
    int from = (neu / NGC_BITS == nearest / NGC_BITS) ? nearest % NGC_BITS - 1 : NGC_BITS - 1;
    for (int bit = from; bit >= neu % NGC_BITS; bit--) {
        if ((bits >> bit) & 1u) { if (++set_bits > 0) return (neu & ~(NGC_BITS - 1)) + bit; }
        else set_bits--;
    }
    return -1;
}
static void bp_node_free(NgcBpNode *n) {
    if (!n) return;
    set_free(&n->set);
    if (n->sparse) { sparse_free(n->sparse); free(n->sparse); }
    if (n->next) { bp_node_free(n->next); free(n->next); }
}
static int bp_import(NgcR *r, NgcBp *bp) { return bp_node_import(r, &bp->first) && sel_import(r, &bp->select); }
static int bp_to_pos(const NgcBp *bp, int index) { return sel_select(&bp->select, &bp->first.set, index); }
static int bp_enclose_i(const NgcBp *bp, int index) {
    if ((unsigned)index >= (unsigned)bp->first.set.bv.bit_length) return -1;
    if (!set_has(&bp->first.set, index)) index = bp_find_open(&bp->first, index);
    return index > 0 ? bp_enclose(&bp->first, index) : -1;
}
static int bp_to_node(const NgcBp *bp, int index) {
    if ((unsigned)index >= (unsigned)bp->first.set.bv.bit_length) return -1;
    if (!set_has(&bp->first.set, index)) index = bp_find_open(&bp->first, index);
    return index >= 0 ? set_rank1(&bp->first.set, index) - 1 : -1;
}
static void bp_free(NgcBp *bp) { bp_node_free(&bp->first); sel_free(&bp->select); memset(&bp->first, 0, sizeof bp->first); }

static int aho_import(NgcR *r, NgcAho *ac) {
    return carr_import(r, &ac->word_lengths) && sparse_import(r, &ac->word_map) &&
           carr_import(r, &ac->multi_indices) && sparse_import(r, &ac->multi_map) &&
           r_u32(r, &ac->nodes_per_character) && sparse_import(r, &ac->node_map) &&
           bp_import(r, &ac->bp) && r->o == r->n;
}

static int inflate_gzip(const uint8_t *in, size_t in_n, uint8_t **out, size_t *out_n) {
    z_stream st; memset(&st, 0, sizeof st);
    if (inflateInit2(&st, 16 + MAX_WBITS) != Z_OK) return 0;
    size_t cap = in_n * 4 + 64; if (cap > NGC_MAX_PLAIN) cap = NGC_MAX_PLAIN;
    uint8_t *buf = malloc(cap);
    if (!buf) { inflateEnd(&st); return 0; }
    st.next_in = (Bytef *)in; st.avail_in = (uInt)in_n;
    for (;;) {
        if (st.total_out >= cap) {
            if (cap >= NGC_MAX_PLAIN) { free(buf); inflateEnd(&st); return 0; }
            size_t ncap = cap * 2; if (ncap > NGC_MAX_PLAIN) ncap = NGC_MAX_PLAIN;
            uint8_t *nbuf = realloc(buf, ncap);
            if (!nbuf) { free(buf); inflateEnd(&st); return 0; }
            buf = nbuf; cap = ncap;
        }
        st.next_out = buf + st.total_out;
        st.avail_out = (uInt)(cap - st.total_out);
        int rc = inflate(&st, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { free(buf); inflateEnd(&st); return 0; }
    }
    inflateEnd(&st);
    *out = buf; *out_n = st.total_out;
    return 1;
}

NgcAho *ngc_aho_load_gzip(const uint8_t *gzip, size_t gzip_size) {
    uint8_t *plain = NULL; size_t plain_n = 0;
    if (!inflate_gzip(gzip, gzip_size, &plain, &plain_n)) return NULL;
    NgcAho *ac = calloc(1, sizeof *ac);
    NgcR r = {plain, plain_n, 0};
    if (!ac || !aho_import(&r, ac)) { ngc_aho_free(ac); free(plain); return NULL; }
    free(plain);
    return ac;
}

void ngc_aho_free(NgcAho *ac) {
    if (!ac) return;
    carr_free(&ac->word_lengths); sparse_free(&ac->word_map);
    carr_free(&ac->multi_indices); sparse_free(&ac->multi_map);
    sparse_free(&ac->node_map); bp_free(&ac->bp);
    free(ac);
}

void ngc_aho_match(const NgcAho *ac, const uint8_t *text, size_t length,
                   int (*callback)(int start, int end, void *ctx), void *ctx) {
    if (!ac || !text || !callback) return;
    int node = 0;
    for (size_t i = 0; i < length; i++) {
        long c = text[i];
        for (;;) {
            long sparse_i = (long)ac->nodes_per_character * c + (uint32_t)node;
            int plain = sparse_rank1(&ac->node_map, sparse_i);
            if (plain) {
                long found = sparse_select_ex(&ac->node_map, plain - 1);
                if (found > 0 && found == sparse_i) {
                    node = plain;
                    int cur = plain;
                    if (sparse_has(&ac->word_map, cur)) {
                        int wlen = carr_get(&ac->word_lengths, sparse_rank1(&ac->word_map, cur) - 1);
                        int start = (int)i + 1 - wlen;
                        if (!callback(start, (int)i + 1, ctx)) return;
                    }
                    while (sparse_has(&ac->multi_map, cur)) {
                        cur = carr_get(&ac->multi_indices, sparse_rank1(&ac->multi_map, cur) - 1);
                        int wlen = sparse_has(&ac->word_map, cur) ? carr_get(&ac->word_lengths, sparse_rank1(&ac->word_map, cur) - 1) : 0;
                        int start = (int)i + 1 - wlen;
                        if (!callback(start, (int)i + 1, ctx)) return;
                    }
                    break;
                }
            }
            if (!node) break;
            int pos = bp_to_pos(&ac->bp, node);
            pos = bp_enclose_i(&ac->bp, pos);
            if (pos < 0) return;
            node = bp_to_node(&ac->bp, pos);
        }
    }
}

static int first_word_walk(const NgcAho *ac, uint8_t *out, size_t cap, size_t off, int node) {
    int characters = (int)((ac->node_map.range_end + ac->nodes_per_character - 1) / ac->nodes_per_character);
    for (int c = 0; c < characters && c < 256; c++) {
        long sparse_i = (long)ac->nodes_per_character * c + (uint32_t)node;
        int plain = sparse_rank1(&ac->node_map, sparse_i);
        if (!plain) continue;
        long found = sparse_select_ex(&ac->node_map, plain - 1);
        if (!(found > 0 && found == sparse_i) || off >= cap) continue;
        out[off] = (uint8_t)c;
        if (sparse_has(&ac->word_map, plain)) return (int)off + 1;
        int n = first_word_walk(ac, out, cap, off + 1, plain);
        if (n) return n;
    }
    return 0;
}
int ngc_aho_first_word(const NgcAho *ac, uint8_t *out, size_t capacity) {
    if (!ac || !out || !capacity) return 0;
    return first_word_walk(ac, out, capacity, 0, 0);
}
