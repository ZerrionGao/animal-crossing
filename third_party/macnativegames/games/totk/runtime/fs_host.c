/* nn::fs host implementation for the functions main imports. RomFS paths and handles go to the
   verified read-only backend (romfs.c); save, temporary and cache storage are host directories under
   the profile directory. Application save data is created on first start, as the system launcher does
   for titles that declare save data. Writes reach the host file immediately; Commit flushes them.
   Because of that, quitting in the middle of a save (the game writes several files, then commits) would leave a slot half
   written: runtime_fs_save_busy tells the window close to wait until the save is done (runtime/frontend.m). */
#include "autoload.h"
#include "runtime.h"
#include "game_application_id.h"
#include "fs_host.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define R_PATH_NOT_FOUND 0x202u
#define R_ALREADY_EXISTS 0x402u
#define R_NOT_EMPTY 0x1002u
#define R_TARGET_NOT_FOUND 0x7d402u
#define R_INVALID_ARGUMENT 0x2ee202u
#define R_INVALID_PATH 0x2ee602u
#define R_OPEN_MODE 0x307402u
#define R_APPEND_REQUIRED 0x2f5e02u
#define DEVICE_SAVE_INITIAL_SIZE 0x06f00000LL
#define DEVICE_SAVE_MAX_SIZE 0x24000000LL
#define DEVICE_SAVE_META_MAGIC 0x41434e4844534156ULL
#define DEVICE_SAVE_META_VERSION 1u
#define DEVICE_SAVE_APP_ID_NAME "01006f8002326000"
#define TOTK_APPLICATION_ID GAME_APPLICATION_ID
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE };
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
#define FILE_BASE 0x200000000ULL
#define DIR_BASE 0x300000000ULL
#define NFILES 1024
#define NDIRS 256

typedef struct { int kind; char name[32]; char root[1024]; int device; } Mount;
typedef struct { int fd, mode, save; } File;   /* save: on a save-data mount (counts for runtime_fs_save_busy) */
typedef struct { DIR *dir; char path[1024]; int mode; } Dir;
static Mount mounts[16];
static File files[NFILES];
static Dir dirs[NDIRS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef GAME_ACNH
static pthread_mutex_t device_save_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    uint64_t magic;
    uint32_t version, reserved;
    uint64_t application_id;
    int64_t normal_size, journal_size;
    uint64_t checksum;
} DeviceSaveMetadata;
_Static_assert(sizeof(DeviceSaveMetadata) == 48, "device save metadata format");
#endif
static char profile[1024], save_root[1024];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void need(int c, const char *m) { if (!c) runtime_fail(m); }

/* Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet (TOTK
   writes a slot's files back to back and then commits; between the files nothing is open for a moment). */
static _Atomic int save_writers, save_dirty;
static _Atomic uint64_t save_changed_ns;
static void save_touched(void) { atomic_store(&save_changed_ns, runtime_now_ns()); atomic_store(&save_dirty, 1); }
static unsigned mount_add(const char *name, int kind, const char *root);
#ifdef GAME_ACNH
static unsigned mount_add_device(const char *name, const char *root);
#endif
int runtime_fs_save_busy(void) {
    if (atomic_load(&save_writers) > 0) return 1;
    return atomic_load(&save_dirty) && runtime_now_ns() - atomic_load(&save_changed_ns) < 3000000000ull;
}
static void make_dir(const char *p) { if (mkdir(p, 0700) && errno != EEXIST) runtime_fail("create profile storage directory"); }
static int remove_tree(const char *path) {
    DIR *d = opendir(path); if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e; int ok = 0; char child[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        ok |= e->d_type == DT_DIR ? remove_tree(child) : unlink(child);
    }
    closedir(d);
    return ok | rmdir(path);
}
static void init(void) {
    const char *dir = getenv("TOTK_PROFILE_DIR"); if (!dir) dir = "user-data";
    need(snprintf(profile, sizeof profile, "%s", dir) < (int)sizeof profile, "profile path");
    uint64_t uid[2]; runtime_account_uid(uid);
    char p[1100]; make_dir(profile);
    snprintf(p, sizeof p, "%s/save", profile); make_dir(p);
    need(snprintf(save_root, sizeof save_root, "%s/save/%016llx%016llx", profile, (unsigned long long)uid[1], (unsigned long long)uid[0]) < (int)sizeof save_root, "save path");
    make_dir(save_root);                                   /* launcher-style EnsureSaveData */
    snprintf(p, sizeof p, "%s/temp", profile); remove_tree(p); make_dir(p);
    snprintf(p, sizeof p, "%s/cache", profile); make_dir(p);
    fprintf(stderr, "[totk] host save storage: %s\n", save_root);
}
static void ready(void) { pthread_once(&once, init); }

#ifdef GAME_ACNH
#define DEVICE_PATH_CAP 1200
static int device_directory_path(const char *path, int create) {
    if (!path || !*path || strlen(path) >= DEVICE_PATH_CAP) return 0;
    const char *scan = path;
    while (*scan) {
        while (*scan == '/') scan++;
        const char *end = scan;
        while (*end && *end != '/') end++;
        size_t length = (size_t)(end - scan);
        if (length == 2 && scan[0] == '.' && scan[1] == '.') return 0;
        scan = end;
    }

    char current[DEVICE_PATH_CAP];
    if (path[0] == '/') snprintf(current, sizeof current, "/");
    else snprintf(current, sizeof current, ".");
    scan = path;
    while (*scan) {
        while (*scan == '/') scan++;
        const char *end = scan;
        while (*end && *end != '/') end++;
        size_t length = (size_t)(end - scan);
        if (!length) break;
        if (!(length == 1 && scan[0] == '.')) {
            size_t used = strlen(current);
            if (used + length + 2 > sizeof current) return 0;
            if (used && current[used - 1] != '/') current[used++] = '/';
            memcpy(current + used, scan, length);
            current[used + length] = 0;
            struct stat st;
            if (lstat(current, &st)) {
                if (errno != ENOENT) return 0;
                if (!create) return 1;
                if (mkdir(current, 0700) && errno != EEXIST) return 0;
                if (lstat(current, &st)) return 0;
            }
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return 0;
        }
        scan = end;
    }
    return 1;
}

static int device_metadata_path_safe(const char *path) {
    struct stat st;
    if (!lstat(path, &st)) return S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode);
    return errno == ENOENT;
}

static int device_profile_preflight(void) {
    const char *dir = getenv("TOTK_PROFILE_DIR");
    if (!dir) dir = "user-data";
    if (!*dir || strlen(dir) >= sizeof profile || !device_directory_path(dir, 0)) return 0;

    char path[DEVICE_PATH_CAP];
    if (snprintf(path, sizeof path, "%s/save", dir) >= (int)sizeof path || !device_directory_path(path, 0)) return 0;
    uint64_t uid[2];
    runtime_account_uid(uid);
    if (snprintf(path, sizeof path, "%s/save/%016llx%016llx", dir,
                 (unsigned long long)uid[1], (unsigned long long)uid[0]) >= (int)sizeof path ||
        !device_directory_path(path, 0)) return 0;
    if (snprintf(path, sizeof path, "%s/temp", dir) >= (int)sizeof path || !device_directory_path(path, 0)) return 0;
    if (snprintf(path, sizeof path, "%s/cache", dir) >= (int)sizeof path || !device_directory_path(path, 0)) return 0;

    char base[DEVICE_PATH_CAP], root[DEVICE_PATH_CAP], meta[DEVICE_PATH_CAP];
    if (snprintf(base, sizeof base, "%s/device-save", dir) >= (int)sizeof base ||
        !device_directory_path(base, 0)) return 0;
    if (snprintf(root, sizeof root, "%s/%s", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof root ||
        !device_directory_path(root, 0)) return 0;
    if (snprintf(meta, sizeof meta, "%s/%s.meta", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof meta ||
        !device_metadata_path_safe(meta)) return 0;
    struct stat meta_st, root_st;
    if (!lstat(meta, &meta_st) && lstat(root, &root_st) && errno == ENOENT) return 0;

    return device_directory_path(dir, 1);
}

static unsigned device_mount_name_check(const char *name) {
    if (!name || !*name || strlen(name) >= sizeof mounts[0].name || strchr(name, ':') || strchr(name, '/')) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int free_slot = 0, duplicate = 0;
    for (int i = 0; i < 16; i++) {
        if (!mounts[i].kind) free_slot = 1;
        else if (!strcmp(mounts[i].name, name)) duplicate = 1;
    }
    pthread_mutex_unlock(&lock);
    return duplicate ? R_ALREADY_EXISTS : free_slot ? 0 : R_INVALID_ARGUMENT;
}

static int device_save_paths(char *base, size_t base_cap, char *root, size_t root_cap,
                             char *meta, size_t meta_cap) {
    int n = snprintf(base, base_cap, "%s/device-save", profile);
    if (n < 0 || (size_t)n >= base_cap) return 0;
    n = snprintf(root, root_cap, "%s/%s", base, DEVICE_SAVE_APP_ID_NAME);
    if (n < 0 || (size_t)n >= root_cap) return 0;
    n = snprintf(meta, meta_cap, "%s/%s.meta", base, DEVICE_SAVE_APP_ID_NAME);
    return n >= 0 && (size_t)n < meta_cap;
}

static uint64_t device_metadata_checksum(const DeviceSaveMetadata *metadata) {
    const uint8_t *bytes = (const uint8_t *)metadata;
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < offsetof(DeviceSaveMetadata, checksum); i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int device_directory_empty(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int empty = 1;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { empty = 0; break; }
    }
    closedir(directory);
    return empty;
}

static int device_payload_path_safe(const char *root, const char *path) {
    size_t root_length = strlen(root);
    if (strncmp(root, path, root_length) || (path[root_length] && path[root_length] != '/')) return 0;
    if (!device_directory_path(root, 0)) return 0;
    struct stat root_stat;
    if (lstat(root, &root_stat) || !S_ISDIR(root_stat.st_mode) || S_ISLNK(root_stat.st_mode)) return 0;
    char current[DEVICE_PATH_CAP];
    if (root_length >= sizeof current) return 0;
    memcpy(current, root, root_length + 1);
    const char *scan = path + root_length;
    while (*scan == '/') scan++;
    while (*scan) {
        const char *end = strchr(scan, '/');
        size_t length = end ? (size_t)(end - scan) : strlen(scan);
        size_t used = strlen(current);
        if (!length || used + length + 2 > sizeof current) return 0;
        current[used++] = '/';
        memcpy(current + used, scan, length);
        current[used + length] = 0;
        struct stat st;
        if (lstat(current, &st)) return errno == ENOENT;
        if (S_ISLNK(st.st_mode) || (end && !S_ISDIR(st.st_mode))) return 0;
        if (!end) break;
        scan = end + 1;
        while (*scan == '/') scan++;
    }
    return 1;
}

static void device_metadata_write_atomic(const char *base, const char *path, DeviceSaveMetadata *metadata) {
    metadata->checksum = device_metadata_checksum(metadata);
    char temporary[DEVICE_PATH_CAP];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        int n = snprintf(temporary, sizeof temporary, "%s/.%s.meta.tmp.%d.%u",
                         base, DEVICE_SAVE_APP_ID_NAME, getpid(), attempt);
        if (n < 0 || (size_t)n >= sizeof temporary) runtime_fail("device save metadata temp path");
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) runtime_fail("create device save metadata temp file");
    size_t written = 0;
    while (written < sizeof *metadata) {
        ssize_t n = write(fd, (const uint8_t *)metadata + written, sizeof *metadata - written);
        if (n <= 0) { close(fd); unlink(temporary); runtime_fail("write device save metadata"); }
        written += (size_t)n;
    }
    if (fcntl(fd, F_FULLFSYNC) < 0 && fsync(fd)) { close(fd); unlink(temporary); runtime_fail("sync device save metadata"); }
    if (close(fd)) { unlink(temporary); runtime_fail("close device save metadata"); }
    if (rename(temporary, path)) { unlink(temporary); runtime_fail("publish device save metadata"); }
    int dirfd = open(base, O_RDONLY);
    if (dirfd < 0) runtime_fail("open device save metadata directory for sync");
    if (fsync(dirfd)) { close(dirfd); runtime_fail("sync device save metadata directory"); }
    close(dirfd);
}

static void device_metadata_load_or_create(const char *root, const char *base, const char *path,
                                           DeviceSaveMetadata *out) {
    struct stat st;
    if (!lstat(path, &st)) {
        if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || st.st_size != sizeof *out)
            runtime_fail("device save metadata type or length invalid");
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) runtime_fail("open device save metadata");
        size_t read_bytes = 0;
        while (read_bytes < sizeof *out) {
            ssize_t n = read(fd, (uint8_t *)out + read_bytes, sizeof *out - read_bytes);
            if (n <= 0) { close(fd); runtime_fail("read device save metadata"); }
            read_bytes += (size_t)n;
        }
        struct stat after;
        int bad = fstat(fd, &after) || !S_ISREG(after.st_mode) || after.st_size != sizeof *out;
        close(fd);
        if (bad || out->magic != DEVICE_SAVE_META_MAGIC || out->version != DEVICE_SAVE_META_VERSION || out->reserved ||
            out->application_id != TOTK_APPLICATION_ID || out->normal_size < DEVICE_SAVE_INITIAL_SIZE ||
            out->journal_size < DEVICE_SAVE_INITIAL_SIZE || out->normal_size > DEVICE_SAVE_MAX_SIZE ||
            out->journal_size > DEVICE_SAVE_MAX_SIZE || out->checksum != device_metadata_checksum(out))
            runtime_fail("device save metadata contents invalid");
        if (lstat(root, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
            runtime_fail("device save payload root missing or invalid for existing metadata");
        return;
    }
    if (errno != ENOENT) runtime_fail("inspect device save metadata");

    if (!lstat(root, &st)) {
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) runtime_fail("device save payload root is not a real directory");
        if (device_directory_empty(root) != 1) runtime_fail("device save files exist without capacity metadata");
    } else if (errno != ENOENT) runtime_fail("inspect device save payload root");
    else if (!device_directory_path(root, 1)) runtime_fail("create device save payload root");

    *out = (DeviceSaveMetadata){
        .magic = DEVICE_SAVE_META_MAGIC,
        .version = DEVICE_SAVE_META_VERSION,
        .application_id = TOTK_APPLICATION_ID,
        .normal_size = DEVICE_SAVE_INITIAL_SIZE,
        .journal_size = DEVICE_SAVE_INITIAL_SIZE,
    };
    device_metadata_write_atomic(base, path, out);
}

static unsigned mount_device_save_data(const char *name) {
    unsigned name_result = device_mount_name_check(name);
    if (name_result) return name_result;
    pthread_mutex_lock(&device_save_lock);
    if (!device_profile_preflight()) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
    ready();
    char base[DEVICE_PATH_CAP], root[DEVICE_PATH_CAP], meta[DEVICE_PATH_CAP];
    if (!device_save_paths(base, sizeof base, root, sizeof root, meta, sizeof meta) ||
        !device_directory_path(base, 1)) {
        pthread_mutex_unlock(&device_save_lock);
        return R_INVALID_ARGUMENT;
    }
    DeviceSaveMetadata metadata;
    device_metadata_load_or_create(root, base, meta, &metadata);
    if (!device_directory_path(root, 0)) {
        pthread_mutex_unlock(&device_save_lock);
        return R_INVALID_ARGUMENT;
    }
    unsigned result = mount_add_device(name, root);
    pthread_mutex_unlock(&device_save_lock);
    return result;
}

static unsigned get_device_save_data_size(int64_t *normal, int64_t *journal) {
    if (!normal || !journal) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&device_save_lock);
    if (!device_profile_preflight()) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
    ready();
    char base[DEVICE_PATH_CAP], root[DEVICE_PATH_CAP], meta[DEVICE_PATH_CAP];
    if (!device_save_paths(base, sizeof base, root, sizeof root, meta, sizeof meta) ||
        !device_directory_path(base, 1)) {
        pthread_mutex_unlock(&device_save_lock);
        return R_INVALID_ARGUMENT;
    }
    DeviceSaveMetadata metadata;
    device_metadata_load_or_create(root, base, meta, &metadata);
    pthread_mutex_unlock(&device_save_lock);
    *normal = metadata.normal_size;
    *journal = metadata.journal_size;
    return 0;
}

static unsigned get_device_save_restore_flag(bool *out, const char *name) {
    (void)out;
    (void)name;
    runtime_fail("ACNH GetSaveDataRestoreFlag is not supported");
}

static unsigned commit_multiple(const char *const *names, int count) {
    (void)names;
    (void)count;
    runtime_fail("ACNH multi-name Commit is not supported");
}

static unsigned commit_with_options(const char *name, const void *option) {
    (void)name;
    (void)option;
    runtime_fail("ACNH Commit with options is not supported");
}

static unsigned extend_device_save_data(int64_t normal, int64_t journal) {
    (void)normal;
    (void)journal;
    runtime_fail("ACNH ExtendDeviceSaveData is not supported");
}
#endif

/* ---------- mounts and paths ---------- */
static unsigned mount_add_internal(const char *name, int kind, const char *root, int device) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < 16; i++) { if (mounts[i].kind && !strcmp(mounts[i].name, name)) { pthread_mutex_unlock(&lock); return R_ALREADY_EXISTS; } if (!mounts[i].kind && slot < 0) slot = i; }
    if (slot < 0) runtime_fail("fs mount table full");
    mounts[slot].kind = kind; mounts[slot].device = device; strcpy(mounts[slot].name, name); snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[totk] fs mount %s: -> %s\n", name, root);
    return 0;
}
static unsigned mount_add(const char *name, int kind, const char *root) {
    return mount_add_internal(name, kind, root, 0);
}
#ifdef GAME_ACNH
static unsigned mount_add_device(const char *name, const char *root) {
    return mount_add_internal(name, M_SAVE, root, 1);
}
#endif
static int component_ok(const char *c, size_t n) { return n && !(n == 1 && c[0] == '.') && !(n == 2 && c[0] == '.' && c[1] == '.'); }
/* "name:/a/b" -> host path; 0 on success; *kind = the mount's kind (M_SAVE ...) when kind is given */
static unsigned host_path_kind(const char *path, char *out, size_t cap, int *kind) {
    if (!path) return R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return R_INVALID_PATH;
    size_t n = (size_t)(colon - path);
    pthread_mutex_lock(&lock);
    const Mount *m = NULL;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) m = &mounts[i];
    char root[1024]; if (m) strcpy(root, m->root);
#ifdef GAME_ACNH
    int device = m ? m->device : 0;
#endif
    int k = m ? m->kind : M_FREE;
    pthread_mutex_unlock(&lock);
    if (!m) return R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    const char *rest = colon + 1;
    for (const char *q = rest + 1; *q;) { const char *e = strchr(q, '/'); size_t len = e ? (size_t)(e - q) : strlen(q); if (!e && !len) break; if (!component_ok(q, len) || memchr(q, '\\', len)) return R_INVALID_PATH; q = e ? e + 1 : q + len; }
    if (snprintf(out, cap, "%s%s", root, rest) >= (int)cap) return R_INVALID_PATH;
    size_t L = strlen(out); while (L > strlen(root) && out[L - 1] == '/') out[--L] = 0;
#ifdef GAME_ACNH
    if (device && !device_payload_path_safe(root, out)) return R_INVALID_PATH;
#endif
    return 0;
}
static unsigned host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }
/* A change to save data through a path (create, delete, rename, directories): marks the save as in progress. */
static unsigned save_change_path(const char *path, char *out, size_t cap) {
    int kind = M_FREE; unsigned r = host_path_kind(path, out, cap, &kind);
    if (!r && kind == M_SAVE) save_touched();
    return r;
}
static unsigned errno_result(void) { return errno == ENOENT || errno == ENOTDIR ? R_PATH_NOT_FOUND : errno == EEXIST ? R_ALREADY_EXISTS : errno == ENOTEMPTY ? R_NOT_EMPTY : R_INVALID_ARGUMENT; }

/* ---------- storage mounts ---------- */
static unsigned mount_save(const char *name, const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    if (!uid || uid[0] != me[0] || uid[1] != me[1]) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned char save_exists(uint64_t application, const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    return application == TOTK_APPLICATION_ID && uid && uid[0] == me[0] && uid[1] == me[1];
}
/* Other titles' save data (e.g. data-link bonuses) is not present on this host. */
static unsigned mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned mount_temp(const char *name) { ready(); char p[1100]; snprintf(p, sizeof p, "%s/temp", profile); return mount_add(name, M_TEMP, p); }
static void cache_dir(int index, char *out, size_t cap) { snprintf(out, cap, "%s/cache/%d", profile, index); }
static unsigned create_cache(int index, int64_t size, int64_t journal) {
    ready(); if (index < 0 || size < 0 || journal < 0) return R_INVALID_ARGUMENT;
    char p[1100]; cache_dir(index, p, sizeof p);
    if (mkdir(p, 0700)) return errno == EEXIST ? R_ALREADY_EXISTS : errno_result();
    return 0;
}
static unsigned delete_cache(int index) { ready(); char p[1100]; cache_dir(index, p, sizeof p); return remove_tree(p) ? R_TARGET_NOT_FOUND : 0; }
static unsigned mount_cache(const char *name, int index) {
    ready(); char p[1100]; cache_dir(index, p, sizeof p); struct stat st;
    if (stat(p, &st) || !S_ISDIR(st.st_mode)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_CACHE, p);
}
typedef struct { int32_t index; uint8_t reserved[0x1c]; } CacheStorageInfo;
typedef struct { int list[64]; int count, next; } CacheList;
static unsigned open_cache_list(uint64_t *out) {
    ready(); if (!out) return R_INVALID_ARGUMENT;
    CacheList *l = calloc(1, sizeof *l); need(l != NULL, "cache list allocation");
    char p[1100]; snprintf(p, sizeof p, "%s/cache", profile); DIR *d = opendir(p); struct dirent *e;
    while (d && (e = readdir(d)) && l->count < 64) { char *end; long v = strtol(e->d_name, &end, 10); if (*e->d_name && !*end && v >= 0) l->list[l->count++] = (int)v; }
    if (d) closedir(d);
    *out = (uint64_t)(uintptr_t)l; return 0;
}
static unsigned read_cache_list(int *count, CacheStorageInfo *out, uint64_t h, int max) {
    CacheList *l = (CacheList *)(uintptr_t)h; if (!l || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    int n = 0; while (n < max && l->next < l->count) { memset(&out[n], 0, sizeof *out); out[n++].index = l->list[l->next++]; }
    *count = n; return 0;
}
static void close_cache_list(uint64_t h) { free((void *)(uintptr_t)h); }
static void unmount(const char *name) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { mounts[i].kind = M_FREE; pthread_mutex_unlock(&lock); return; }
    pthread_mutex_unlock(&lock);
    BOTW_AOC_UNMOUNT(name) if (!runtime_romfs_owns(name)) runtime_fail("unmount of unknown fs mount");
}
static unsigned commit(const char *name) {
    pthread_mutex_lock(&lock); int found = 0, save = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { found = 1; save = mounts[i].kind == M_SAVE; }
    pthread_mutex_unlock(&lock);
    if (!found) return R_PATH_NOT_FOUND;
    for (int i = 0; i < NFILES; i++) { pthread_mutex_lock(&lock); int fd = files[i].fd; pthread_mutex_unlock(&lock); if (fd > 0) fcntl(fd, F_FULLFSYNC); }
    if (save && !(getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 23)) atomic_store(&save_dirty, 0);   /* 23: probe-save's negative control */
    return 0;
}

/* ---------- files ---------- */
static File *file_of(uint64_t h, int *fd, int *mode) {
    if (h < FILE_BASE || h >= FILE_BASE + NFILES) return NULL;
    pthread_mutex_lock(&lock); File *f = &files[h - FILE_BASE]; int v = f->fd; if (fd) *fd = f->fd; if (mode) *mode = f->mode; pthread_mutex_unlock(&lock);
    return v > 0 ? f : NULL;
}
void runtime_fs_log(const char *what, const char *path);
static unsigned open_file(uint64_t *out, const char *path, int mode) {
    if (path && !runtime_romfs_owns(path)) runtime_fs_log(mode & 2 ? "write" : "read", path);
    if (runtime_romfs_owns(path)) return runtime_romfs_open(out, path, mode); BOTW_AOC_OPEN(out, path, mode)
    if (!out || !(mode & (OPEN_READ | OPEN_WRITE)) || (mode & ~7)) return R_INVALID_ARGUMENT;
    char p[1100]; int kind = M_FREE; unsigned r = host_path_kind(path, p, sizeof p, &kind); if (r) return r;
    struct stat st; if (stat(p, &st)) return R_PATH_NOT_FOUND; if (!S_ISREG(st.st_mode)) return R_PATH_NOT_FOUND;
    int save_writer = kind == M_SAVE && (mode & OPEN_WRITE);
    int fd = open(p, (mode & OPEN_WRITE ? (mode & OPEN_READ ? O_RDWR : O_WRONLY) : O_RDONLY) | O_NOFOLLOW);
    if (fd < 0) return errno_result();
    runtime_autoload_save_file_opened(path);   /* TOTK_AUTOLOAD: a slot's caption.sav starts the presses, its progress.sav ends them */
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++) if (files[i].fd <= 0) {
        files[i] = (File){fd, mode, save_writer}; *out = FILE_BASE + (uint64_t)i; pthread_mutex_unlock(&lock);
        if (save_writer) { atomic_fetch_add(&save_writers, 1); save_touched(); }
        return 0;
    }
    pthread_mutex_unlock(&lock); runtime_fail("host file handle table full");
}
static unsigned read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (runtime_romfs_handle(h)) return runtime_romfs_read(out, h, offset, buffer, size); BOTW_AOC_READ(out, h, offset, buffer, size)
    int fd, mode; if (!out || offset < 0 || (!buffer && size) || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_READ)) return R_OPEN_MODE;
    dm_guest_write_prepare(buffer, (size_t)size);
    uint64_t done = 0;
    while (done < size) { ssize_t n = pread(fd, (uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done); if (n < 0) runtime_fail("host save read failure"); if (!n) break; done += (uint64_t)n; }
    *out = done; return 0;
}
static unsigned read_options(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size, const void *options) { (void)options; return read_file(out, h, off, buf, size); }
static unsigned read_exact(uint64_t h, int64_t off, void *buf, uint64_t size) { uint64_t n = 0; unsigned r = read_file(&n, h, off, buf, size); return r ? r : n == size ? 0 : R_INVALID_ARGUMENT; }
static unsigned write_file(uint64_t h, int64_t offset, const void *buffer, uint64_t size, const int32_t *option) {
    int fd, mode; File *f = offset < 0 || (!buffer && size) ? NULL : file_of(h, &fd, &mode); if (!f) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    struct stat st; if (fstat(fd, &st)) runtime_fail("host save stat");
    if ((uint64_t)offset + size > (uint64_t)st.st_size && !(mode & OPEN_APPEND)) return R_APPEND_REQUIRED;
    uint64_t done = 0;
    if (f->save) {
        save_touched();
        static int delay_ms = -1; if (delay_ms < 0) { const char *e = getenv("TOTK_SAVE_WRITE_DELAY_MS"); delay_ms = e ? atoi(e) : 0; }
        if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);   /* tests only: slow saves down so a window close lands in the middle of one */
    }
    while (done < size) { ssize_t n = pwrite(fd, (const uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done); if (n <= 0) runtime_fail("host save write failure"); done += (uint64_t)n; }
    if (option && (*option & 1)) fcntl(fd, F_FULLFSYNC);
    return 0;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (runtime_romfs_handle(h)) return runtime_romfs_size(out, h); BOTW_AOC_SIZE(out, h)
    int fd; struct stat st; if (!out || !file_of(h, &fd, NULL) || fstat(fd, &st)) return R_INVALID_ARGUMENT;
    *out = st.st_size; return 0;
}
static unsigned set_file_size(uint64_t h, int64_t size) {
    int fd, mode; if (size < 0 || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    if (files[h - FILE_BASE].save) save_touched();
    return ftruncate(fd, size) ? errno_result() : 0;
}
static unsigned flush_file(uint64_t h) { int fd; if (!file_of(h, &fd, NULL)) return R_INVALID_ARGUMENT; fcntl(fd, F_FULLFSYNC); return 0; }
static void close_file(uint64_t h) {
    if (runtime_romfs_handle(h)) { runtime_romfs_close(h); return; } BOTW_AOC_CLOSE(h)
    if (!file_of(h, NULL, NULL)) runtime_fail("close invalid file handle");
    pthread_mutex_lock(&lock); int fd = files[h - FILE_BASE].fd, save = files[h - FILE_BASE].save; files[h - FILE_BASE].fd = 0; pthread_mutex_unlock(&lock);
    close(fd);
    if (save) { save_touched(); atomic_fetch_sub(&save_writers, 1); }
}
static unsigned create_file(const char *path, int64_t size) {
    char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; if (size < 0) return R_INVALID_ARGUMENT;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600); if (fd < 0) return errno_result();
    int bad = ftruncate(fd, size); close(fd); return bad ? errno_result() : 0;
}
static unsigned delete_file(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return unlink(p) ? errno_result() : 0; }
static unsigned rename_file(const char *from, const char *to) {
    char a[1100], b[1100]; unsigned r = save_change_path(from, a, sizeof a); if (!r) r = host_path(to, b, sizeof b); if (r) return r;
    struct stat st; if (!stat(b, &st)) return R_ALREADY_EXISTS;
    return rename(a, b) ? errno_result() : 0;
}
static unsigned entry_type(int *out, const char *path) {
    if (runtime_romfs_owns(path)) return runtime_romfs_entry_type(out, path); BOTW_AOC_TYPE(out, path)
    char p[1100]; unsigned r = host_path(path, p, sizeof p); if (r) return r; struct stat st;
    if (!out) return R_INVALID_ARGUMENT; if (stat(p, &st)) return R_PATH_NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? 0 : 1; return 0;
}

/* ---------- directories ---------- */
typedef RomfsDirectoryEntry DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");
_Static_assert(offsetof(DirectoryEntry, type) == 0x304 && offsetof(DirectoryEntry, size) == 0x308, "nn::fs::DirectoryEntry fields");
static unsigned create_directory(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return mkdir(p, 0700) ? errno_result() : 0; }
static unsigned delete_recursive(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; struct stat st; if (stat(p, &st)) return R_PATH_NOT_FOUND; return remove_tree(p) ? R_INVALID_ARGUMENT : 0; }
static unsigned open_directory(uint64_t *out, const char *path, int mode) {
    if (runtime_romfs_owns(path)) {
#if defined(GAME_ACNH)
        return runtime_romfs_open_directory(out, path, mode);
#else
        fprintf(stderr, "[totk] OpenDirectory on RomFS %s\n", path); runtime_fail("RomFS directory listing not implemented");
#endif
    }
    char p[1100]; unsigned r = host_path(path, p, sizeof p); if (r) return r;
#if defined(GAME_ACNH)
    uint32_t bits=(uint32_t)mode;if (!out || !(bits&3) || (bits&~(1u|2u|0x80000000u))) return R_INVALID_ARGUMENT;
#else
    if (!out || mode < 1 || mode > 3) return R_INVALID_ARGUMENT;
#endif
    DIR *d = opendir(p); if (!d) return errno_result();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++) if (!dirs[i].dir) { dirs[i].dir = d; dirs[i].mode = mode; strcpy(dirs[i].path, p); *out = DIR_BASE + (uint64_t)i; pthread_mutex_unlock(&lock); return 0; }
    pthread_mutex_unlock(&lock); runtime_fail("host directory handle table full");
}
static unsigned read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (runtime_romfs_directory_handle(h)) return runtime_romfs_read_directory(count,out,h,max);
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE]; if (!d->dir) return R_INVALID_ARGUMENT;
    int64_t n = 0; struct dirent *e;
    while (n < max && (e = readdir(d->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 768) continue;
        char child[2048]; snprintf(child, sizeof child, "%s/%s", d->path, e->d_name); struct stat st; if (stat(child, &st)) continue;
        int is_dir = S_ISDIR(st.st_mode);
#if defined(GAME_ACNH)
        uint32_t bits=(uint32_t)d->mode;if (!(bits & (is_dir ? 1u : 2u))) continue;
        memset(&out[n], 0, sizeof *out); strcpy(out[n].name, e->d_name); out[n].type = is_dir ? 0 : 1; out[n].size = is_dir || (bits&0x80000000u) ? 0 : st.st_size; n++;
#else
        if (!(d->mode & (is_dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out); strcpy(out[n].name, e->d_name); out[n].type = is_dir ? 0 : 1; out[n].size = is_dir ? 0 : st.st_size; n++;
#endif
    }
    *count = n; return 0;
}
static void close_directory(uint64_t h) {
    if (runtime_romfs_directory_handle(h)) { runtime_romfs_close_directory(h); return; }
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].dir) runtime_fail("close invalid directory handle");
    pthread_mutex_lock(&lock); closedir(dirs[h - DIR_BASE].dir); dirs[h - DIR_BASE].dir = NULL; pthread_mutex_unlock(&lock);
}
/* The fs priority only orders I/O inside the system fs process; host I/O is unordered. */
static void set_priority(int priority) { (void)priority; }

/* make probe-save: the "save in progress" state the window close waits on, through the fs entry points the game uses
   (a fresh profile in build/probe-save-profile; no game). */
void runtime_save_probe(void) {
    const char *dir = "build/probe-save-profile";   /* a fresh profile each run (a failed run leaves it for a look) */
    remove_tree(dir); setenv("TOTK_PROFILE_DIR", dir, 1);
    unsigned checks = 0; uint64_t uid[2]; runtime_account_uid(uid); uint64_t h = 0;
#define CHECK(c, what) do { checks++; if (!(c)) { fprintf(stderr, "save probe: %s\n", what); runtime_fail("save probe failed"); } } while (0)
    CHECK(mount_save("save", uid) == 0 && mount_temp("temp") == 0, "mount save and temporary storage");
    CHECK(!runtime_fs_save_busy(), "idle at start");
    CHECK(create_file("save:/a.sav", 16) == 0 && runtime_fs_save_busy(), "creating a save file starts a save");
    CHECK(commit("save") == 0 && !runtime_fs_save_busy(), "commit ends it");
    CHECK(open_file(&h, "save:/a.sav", OPEN_WRITE) == 0 && runtime_fs_save_busy(), "a save file open for writing");
    static const char data[16] = "progress";
    CHECK(write_file(h, 0, data, sizeof data, NULL) == 0 && runtime_fs_save_busy(), "writing");
    atomic_store(&save_changed_ns, runtime_now_ns() - 10000000000ull);
    CHECK(runtime_fs_save_busy(), "a file open for writing is busy however long ago it was written");
    close_file(h);
    CHECK(runtime_fs_save_busy(), "closed but not committed yet (the next file of the save follows)");
    CHECK(commit("save") == 0 && !runtime_fs_save_busy(), "committed");
    CHECK(open_file(&h, "save:/a.sav", OPEN_READ) == 0 && !runtime_fs_save_busy(), "reading is not saving");
    close_file(h);
    CHECK(create_file("save:/b.sav", 4) == 0 && runtime_fs_save_busy(), "second change");
    atomic_store(&save_changed_ns, runtime_now_ns() - 4000000000ull);
    CHECK(!runtime_fs_save_busy(), "uncommitted changes older than 3 s are not a save in progress");
    CHECK(create_file("temp:/t.bin", 4) == 0 && !runtime_fs_save_busy(), "temporary storage does not count");
    CHECK(rename_file("save:/b.sav", "save:/c.sav") == 0 && runtime_fs_save_busy(), "renaming save data");
    CHECK(commit("save") == 0 && !runtime_fs_save_busy() && delete_file("save:/c.sav") == 0 && runtime_fs_save_busy() && commit("save") == 0, "delete and commit");
#undef CHECK
    remove_tree(dir);
    printf("{\"status\":\"pass\",\"probe\":\"probe-save\",\"checks\":%u,\"scope\":\"save-in-progress state (runtime_fs_save_busy) through open / write / close / create / rename / delete / commit on a save mount; reads and temporary storage do not count; stale uncommitted changes do not hold a close\"}\n", checks);
}

void *runtime_fs_host_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", open_file}, {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", file_size}, BOTW_AOC_TABLE
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE", close_file}, {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", entry_type},
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", read_file}, {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", read_options},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", read_exact},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", write_file},
        {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", set_file_size}, {"_ZN2nn2fs9FlushFileENS0_10FileHandleE", flush_file},
        {"_ZN2nn2fs10CreateFileEPKcl", create_file}, {"_ZN2nn2fs10DeleteFileEPKc", delete_file}, {"_ZN2nn2fs10RenameFileEPKcS2_", rename_file},
        {"_ZN2nn2fs15CreateDirectoryEPKc", create_directory}, {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", delete_recursive},
        {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", open_directory},
        {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", read_directory},
        {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", close_directory},
        {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", mount_save},
        {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", mount_save_ro},
        {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", save_exists},
#ifdef GAME_ACNH
        {"_ZN2nn2fs19MountDeviceSaveDataEPKc", mount_device_save_data},
        {"_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_", get_device_save_data_size},
        {"_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc", get_device_save_restore_flag},
        {"_ZN2nn2fs6CommitEPKPKci", commit_multiple},
        {"_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE", commit_with_options},
        {"_ZN2nn2fs20ExtendDeviceSaveDataEll", extend_device_save_data},
#endif
        {"_ZN2nn2fs21MountTemporaryStorageEPKc", mount_temp}, {"_ZN2nn2fs7UnmountEPKc", unmount}, {"_ZN2nn2fs6CommitEPKc", commit},
#ifdef GAME_BOTW
        {"_ZN2nn2fs14CommitSaveDataEPKc", commit},   /* SDK 7 (Breath of the Wild 1.6.0) */
#endif
       
        {"_ZN2nn2fs18CreateCacheStorageEill", create_cache}, {"_ZN2nn2fs18DeleteCacheStorageEi", delete_cache},
        {"_ZN2nn2fs17MountCacheStorageEPKci", mount_cache},
        {"_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", open_cache_list},
        {"_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", read_cache_list},
        {"_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", close_cache_list},
        {"_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", set_priority},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
