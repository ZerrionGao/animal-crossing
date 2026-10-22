#pragma once
#include <stddef.h>
#include <stdint.h>

enum { ROMFS_DIR_READ_DIRS = 1, ROMFS_DIR_READ_FILES = 2 };
/* libnx FsDirOpenMode_NoFileSize is BIT(31). */
#define ROMFS_DIR_NO_FILE_SIZE UINT32_C(0x80000000)

typedef struct RomfsDirIndex RomfsDirIndex;
typedef struct RomfsDirCursor RomfsDirCursor;
typedef struct { const char *path; uint64_t size; } RomfsDirFile;
typedef struct {
    char name[769];
    uint8_t pad0[3];
    int8_t type;
    uint8_t pad1[3];
    int64_t size;
} RomfsDirectoryEntry;

RomfsDirIndex *romfs_dir_index_create(const char *const *directories, size_t directory_count,
                                      const RomfsDirFile *files, size_t file_count);
void romfs_dir_index_destroy(RomfsDirIndex *index);
size_t romfs_dir_index_directory_count(const RomfsDirIndex *index, const char *path);
size_t romfs_dir_index_file_count(const RomfsDirIndex *index, const char *path);
RomfsDirCursor *romfs_dir_open(RomfsDirIndex *index, const char *path, uint32_t mode);
int romfs_dir_read(RomfsDirCursor *cursor, int64_t *count, RomfsDirectoryEntry *out, int64_t max);
void romfs_dir_close(RomfsDirCursor *cursor);
void romfs_dir_cursor_position(const RomfsDirCursor *cursor, size_t *next, size_t *end);
