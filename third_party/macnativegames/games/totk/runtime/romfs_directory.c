#include "romfs_directory.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *path; } Dir;
typedef struct { uint32_t parent; char *name; size_t name_len; uint64_t size; int8_t type; } Child;
struct RomfsDirIndex { Dir *dirs; size_t dir_count; Child *children; size_t child_count; };
struct RomfsDirCursor { RomfsDirIndex *index; size_t next, end; uint32_t mode; };

static int dir_order(const void *a, const void *b) { return strcmp(((const Dir *)a)->path, ((const Dir *)b)->path); }
static int child_order(const void *a, const void *b) {
    const Child *x = a, *y = b;
    if (x->parent != y->parent) return (x->parent > y->parent) - (x->parent < y->parent);
    size_t n = x->name_len < y->name_len ? x->name_len : y->name_len;
    int c = memcmp(x->name, y->name, n);
    return c ? c : (x->name_len > y->name_len) - (x->name_len < y->name_len);
}
static size_t find_dir(const RomfsDirIndex *index, const char *path) {
    size_t lo = 0, hi = index->dir_count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; int c = strcmp(index->dirs[mid].path, path); if (c < 0) lo = mid + 1; else hi = mid; }
    return lo < index->dir_count && !strcmp(index->dirs[lo].path, path) ? lo : SIZE_MAX;
}
static void index_free(RomfsDirIndex *index) {
    if (!index) return;
    for (size_t i = 0; i < index->dir_count; i++) free(index->dirs[i].path);
    for (size_t i = 0; i < index->child_count; i++) free(index->children[i].name);
    free(index->dirs); free(index->children); free(index);
}
RomfsDirIndex *romfs_dir_index_create(const char *const *directories, size_t directory_count,
                                      const RomfsDirFile *files, size_t file_count) {
    if (!directories || !directory_count || (!files && file_count) || directory_count > UINT32_MAX ||
        directory_count - 1 > SIZE_MAX - file_count) return NULL;
    RomfsDirIndex *index = calloc(1, sizeof(*index)); if (!index) return NULL;
    index->dirs = calloc(directory_count, sizeof(*index->dirs));
    index->child_count = directory_count - 1 + file_count;
    index->children = calloc(index->child_count ? index->child_count : 1, sizeof(*index->children));
    if (!index->dirs || !index->children) { index_free(index); return NULL; }
    index->dir_count = directory_count;
    for (size_t i = 0; i < directory_count; i++) {
        if (!directories[i] || !(index->dirs[i].path = strdup(directories[i]))) { index_free(index); return NULL; }
    }
    qsort(index->dirs, directory_count, sizeof(*index->dirs), dir_order);
    for (size_t i = 0; i < directory_count; i++) {
        const char *path = index->dirs[i].path;
        if ((i == 0 && *path) || (i && !strcmp(path, index->dirs[i - 1].path))) { index_free(index); return NULL; }
    }
    if (find_dir(index, "") == SIZE_MAX) { index_free(index); return NULL; }
    size_t out = 0;
    for (size_t i = 0; i < directory_count; i++) {
        const char *path = index->dirs[i].path; if (!*path) continue;
        const char *slash = strrchr(path, '/'); size_t parent_len = slash ? (size_t)(slash - path) : 0;
        char *parent = strndup(path, parent_len); if (!parent) { index_free(index); return NULL; }
        size_t parent_id = find_dir(index, parent); free(parent);
        if (parent_id == SIZE_MAX || (slash && !slash[1])) { index_free(index); return NULL; }
        const char *name = slash ? slash + 1 : path; size_t len = strlen(name);
        if (len > 768) { index_free(index); return NULL; }
        index->children[out] = (Child){(uint32_t)parent_id, strndup(name, len), len, 0, 0};
        if (!index->children[out].name) { index_free(index); return NULL; } out++;
    }
    for (size_t i = 0; i < file_count; i++) {
        const char *path = files[i].path; if (!path || !*path) { index_free(index); return NULL; }
        const char *slash = strrchr(path, '/'); size_t parent_len = slash ? (size_t)(slash - path) : 0;
        char *parent = strndup(path, parent_len); if (!parent) { index_free(index); return NULL; }
        size_t parent_id = find_dir(index, parent); free(parent);
        const char *name = slash ? slash + 1 : path; size_t len = strlen(name);
        if (parent_id == SIZE_MAX || !len || len > 768 || files[i].size > INT64_MAX) { index_free(index); return NULL; }
        index->children[out] = (Child){(uint32_t)parent_id, strndup(name, len), len, files[i].size, 1};
        if (!index->children[out].name) { index_free(index); return NULL; } out++;
    }
    qsort(index->children, index->child_count, sizeof(*index->children), child_order);
    for (size_t i = 1; i < index->child_count; i++) if (!child_order(&index->children[i - 1], &index->children[i])) { index_free(index); return NULL; }
    return index;
}
void romfs_dir_index_destroy(RomfsDirIndex *index) { index_free(index); }
static void range_for(const RomfsDirIndex *index, size_t parent, size_t *begin, size_t *end) {
    size_t lo = 0, hi = index->child_count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (index->children[mid].parent < parent) lo = mid + 1; else hi = mid; }
    *begin = lo; hi = index->child_count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (index->children[mid].parent <= parent) lo = mid + 1; else hi = mid; }
    *end = lo;
}
static size_t count_type(const RomfsDirIndex *index, const char *path, int8_t type) {
    size_t parent = find_dir(index, path); if (parent == SIZE_MAX) return SIZE_MAX;
    size_t begin, end, count = 0; range_for(index, parent, &begin, &end);
    for (size_t i = begin; i < end; i++) if (index->children[i].type == type) count++;
    return count;
}
size_t romfs_dir_index_directory_count(const RomfsDirIndex *index, const char *path) { return index && path ? count_type(index, path, 0) : SIZE_MAX; }
size_t romfs_dir_index_file_count(const RomfsDirIndex *index, const char *path) { return index && path ? count_type(index, path, 1) : SIZE_MAX; }
RomfsDirCursor *romfs_dir_open(RomfsDirIndex *index, const char *path, uint32_t mode) {
    if (!index || !path || !(mode & 3) || (mode & ~(ROMFS_DIR_READ_DIRS | ROMFS_DIR_READ_FILES | ROMFS_DIR_NO_FILE_SIZE))) return NULL;
    size_t parent = find_dir(index, path); if (parent == SIZE_MAX) return NULL;
    RomfsDirCursor *cursor = malloc(sizeof(*cursor)); if (!cursor) return NULL;
    cursor->index = index; cursor->mode = mode; range_for(index, parent, &cursor->next, &cursor->end); return cursor;
}
int romfs_dir_read(RomfsDirCursor *cursor, int64_t *count, RomfsDirectoryEntry *out, int64_t max) {
    if (!cursor || !count || max < 0 || (!out && max)) return -1;
    int64_t n = 0;
    while (n < max && cursor->next < cursor->end) {
        const Child *child = &cursor->index->children[cursor->next++];
        if (!(cursor->mode & (child->type ? ROMFS_DIR_READ_FILES : ROMFS_DIR_READ_DIRS))) continue;
        RomfsDirectoryEntry *entry = &out[n++]; memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, child->name, child->name_len); entry->type = child->type;
        if (child->type && !(cursor->mode & ROMFS_DIR_NO_FILE_SIZE)) entry->size = (int64_t)child->size;
    }
    *count = n; return 0;
}
void romfs_dir_close(RomfsDirCursor *cursor) { free(cursor); }
void romfs_dir_cursor_position(const RomfsDirCursor *cursor, size_t *next, size_t *end) { if (cursor) { if (next) *next = cursor->next; if (end) *end = cursor->end; } }
