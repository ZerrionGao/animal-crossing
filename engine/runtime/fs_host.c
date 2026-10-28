/* nn::fs 宿主实现：RomFS 交给只读后端 romfs.c，存档、临时和缓存目录位于 profile 下。
   首次启动会创建应用存档。ACNH 设备存档使用双槽事务；普通存档保留原有直接写入和关窗忙状态。 */
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
#define DEVICE_SAVE_META_VERSION 2u
#define DEVICE_SAVE_APP_ID_NAME "01006f8002326000"
#define TOTK_APPLICATION_ID GAME_APPLICATION_ID
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE };
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
#define FILE_BASE 0x200000000ULL
#define DIR_BASE 0x300000000ULL
#define NFILES 1024
#define NDIRS 256

typedef struct { int kind; char name[32]; char root[1024]; int device; } Mount;
typedef struct { int fd, mode, save, device; uint64_t generation; } File;   /* save 表示存档挂载上的写句柄 */
typedef struct { DIR *dir; char path[1024]; int mode, device; uint64_t generation; } Dir;
static Mount mounts[16];
#ifdef GAME_ACNH
/* 只保护挂载生命周期，不保留已卸载名称的历史记录。 */
static pthread_mutex_t mount_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static File files[NFILES];
static Dir dirs[NDIRS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t device_save_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef GAME_ACNH
typedef struct {
    uint64_t magic;
    uint32_t version, reserved;
    uint64_t application_id;
    int64_t normal_size, journal_size;
    uint32_t active_slot, restore_pending;
    uint64_t checksum;
} DeviceSaveMetadata;
_Static_assert(sizeof(DeviceSaveMetadata) == 56, "device save metadata format");
typedef struct {
    uint64_t magic;
    uint32_t version, reserved;
    uint64_t application_id;
    int64_t normal_size, journal_size;
    uint64_t checksum;
} DeviceSaveMetadataV1;
_Static_assert(sizeof(DeviceSaveMetadataV1) == 48, "legacy device save metadata format");
typedef struct {
    uint64_t magic;
    uint32_t version, target;
    uint64_t checksum;
} DeviceSaveTxn;
#define DEVICE_SAVE_TXN_MAGIC 0x41434e4854584e31ULL
static DeviceSaveMetadata device_store_meta;
static char device_store_base[1200], device_store_meta_path[1200], device_store_slots[2][1200], device_store_txn_path[1200];
static int device_store_loaded, device_store_staging, device_store_writers;
enum { DEVICE_FINAL_NONE, DEVICE_FINAL_META_SYNC, DEVICE_FINAL_MARKER_UNLINK, DEVICE_FINAL_MARKER_SYNC };
static int device_store_finalizing;
static uint64_t device_store_generation = 1;
#ifdef ACNH_SAVE_TESTING
static _Atomic int device_test_classification_pause;
void acnh_save_test_pause_arm(void) { atomic_store(&device_test_classification_pause, 1); }
int acnh_save_test_pause_state(void) { return atomic_load(&device_test_classification_pause); }
void acnh_save_test_pause_release(void) { atomic_store(&device_test_classification_pause, 3); }
#endif
#endif
static char profile[1024], save_root[1024];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void need(int c, const char *m) { if (!c) runtime_fail(m); }

/* Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet (TOTK
   writes a slot's files back to back and then commits; between the files nothing is open for a moment). */
static _Atomic int save_writers, save_dirty;
#ifdef GAME_ACNH
static _Atomic int device_save_dirty;
#endif
static _Atomic uint64_t save_changed_ns;
static void save_touched(void) { atomic_store(&save_changed_ns, runtime_now_ns()); atomic_store(&save_dirty, 1); }
static void save_touched_device(int device) {
#ifdef GAME_ACNH
    if (device) { atomic_store(&save_changed_ns, runtime_now_ns()); atomic_store(&device_save_dirty, 1); return; }
#else
    (void)device;
#endif
    save_touched();
}
static unsigned mount_add(const char *name, int kind, const char *root);
static unsigned host_path(const char *path, char *out, size_t cap);
#ifdef GAME_ACNH
static unsigned mount_add_device(const char *name, const char *root);
#endif
int runtime_fs_save_busy(void) {
    if (atomic_load(&save_writers) > 0) return 1;
    int dirty = atomic_load(&save_dirty);
#ifdef GAME_ACNH
    /* 提交和开始暂存时逐文件落盘，可能超过 3 秒且不刷新改动时间；持锁期间一律算忙。 */
    if (pthread_mutex_trylock(&device_save_lock)) return 1;
    pthread_mutex_unlock(&device_save_lock);
    dirty |= atomic_load(&device_save_dirty);
#endif
    return dirty && runtime_now_ns() - atomic_load(&save_changed_ns) < 3000000000ull;
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
#ifdef GAME_ACNH
static int clear_tree(const char *path) {
    DIR *d = opendir(path); if (!d) return -1;
    struct dirent *e; int ok = 0; char child[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        ok |= e->d_type == DT_DIR ? remove_tree(child) : unlink(child);
    }
    closedir(d);
    return ok;
}
#endif
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
#ifdef GAME_ACNH
    fprintf(stderr, "[acnh] host save storage initialized\n");
#else
    fprintf(stderr, "[totk] host save storage: %s\n", save_root);
#endif
}
static void ready(void) { pthread_once(&once, init); }

#ifdef GAME_ACNH
#define DEVICE_PATH_CAP 1200
static int device_directory_path_checked(const char *path, int create, const char **failure) {
    if (failure) *failure = NULL;
    if (!path || !*path || strlen(path) >= DEVICE_PATH_CAP) {
        if (failure) *failure = "empty-or-too-long";
        return 0;
    }
    const char *scan = path;
    while (*scan) {
        while (*scan == '/') scan++;
        const char *end = scan;
        while (*end && *end != '/') end++;
        size_t length = (size_t)(end - scan);
        if (length == 2 && scan[0] == '.' && scan[1] == '.') {
            if (failure) *failure = "parent-component";
            return 0;
        }
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
            if (used + length + 2 > sizeof current) {
                if (failure) *failure = "component-too-long";
                return 0;
            }
            if (used && current[used - 1] != '/') current[used++] = '/';
            memcpy(current + used, scan, length);
            current[used + length] = 0;
            struct stat st;
            if (lstat(current, &st)) {
                if (errno != ENOENT) {
                    if (failure) *failure = "lstat-error";
                    return 0;
                }
                if (!create) return 1;
                if (mkdir(current, 0700) && errno != EEXIST) {
                    if (failure) *failure = "mkdir-error";
                    return 0;
                }
                if (lstat(current, &st)) {
                    if (failure) *failure = "lstat-after-mkdir-error";
                    return 0;
                }
            }
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                if (failure) *failure = "component-not-directory-or-symlink";
                return 0;
            }
        }
        scan = end;
    }
    return 1;
}
static int device_directory_path(const char *path, int create) {
    return device_directory_path_checked(path, create, NULL);
}

static int device_metadata_path_safe(const char *path) {
    struct stat st;
    if (!lstat(path, &st)) return S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode);
    return errno == ENOENT;
}

static int device_profile_preflight(void) {
    const char *dir = getenv("TOTK_PROFILE_DIR");
    if (!dir) dir = "user-data";
#define PREFLIGHT_REJECT(stage) do { fprintf(stderr, "[acnh-save] profile preflight failed: %s\n", stage); return 0; } while (0)
    const char *path_failure = NULL;
    if (!*dir) PREFLIGHT_REJECT("profile-root-empty");
    if (strlen(dir) >= sizeof profile) PREFLIGHT_REJECT("profile-root-too-long");
    if (!device_directory_path_checked(dir, 0, &path_failure)) {
        fprintf(stderr, "[acnh-save] profile root path rejected: %s\n", path_failure ? path_failure : "unknown");
        return 0;
    }

    char path[DEVICE_PATH_CAP];
    if (snprintf(path, sizeof path, "%s/save", dir) >= (int)sizeof path || !device_directory_path(path, 0)) PREFLIGHT_REJECT("user-save-path");
    uint64_t uid[2];
    runtime_account_uid(uid);
    if (snprintf(path, sizeof path, "%s/save/%016llx%016llx", dir,
                 (unsigned long long)uid[1], (unsigned long long)uid[0]) >= (int)sizeof path ||
        !device_directory_path(path, 0)) PREFLIGHT_REJECT("user-save-account-path");
    if (snprintf(path, sizeof path, "%s/temp", dir) >= (int)sizeof path || !device_directory_path(path, 0)) PREFLIGHT_REJECT("temp-path");
    if (snprintf(path, sizeof path, "%s/cache", dir) >= (int)sizeof path || !device_directory_path(path, 0)) PREFLIGHT_REJECT("cache-path");

    char base[DEVICE_PATH_CAP], root[DEVICE_PATH_CAP], meta[DEVICE_PATH_CAP];
    if (snprintf(base, sizeof base, "%s/device-save", dir) >= (int)sizeof base ||
        !device_directory_path(base, 0)) PREFLIGHT_REJECT("device-save-base");
    if (snprintf(root, sizeof root, "%s/%s", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof root ||
        !device_directory_path(root, 0)) PREFLIGHT_REJECT("device-save-root");
    if (snprintf(meta, sizeof meta, "%s/%s.meta", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof meta ||
        !device_metadata_path_safe(meta)) PREFLIGHT_REJECT("device-save-metadata");
    struct stat meta_st, root_st;
    if (!lstat(meta, &meta_st) && lstat(root, &root_st) && errno == ENOENT) PREFLIGHT_REJECT("metadata-without-payload");

    if (!device_directory_path(dir, 1)) PREFLIGHT_REJECT("create-profile-root");
#undef PREFLIGHT_REJECT
    return 1;
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
    return duplicate ? R_ALREADY_EXISTS : !free_slot ? R_INVALID_ARGUMENT : 0;
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

static int device_test_failure(const char *stage) {
#ifdef ACNH_SAVE_TESTING
    const char *requested = getenv("ACNH_SAVE_FAILPOINT");
    if (requested && ((!strcmp(requested, "marker-enospc") && !strcmp(stage, "marker-sync")) ||
                      (!strcmp(requested, "copy-enospc") && !strcmp(stage, "copy-enospc")))) {
        errno = ENOSPC; return 1;
    }
    if (requested && !strcmp(requested, stage)) { errno = EIO; return 1; }
#else
    (void)stage;
#endif
    return 0;
}
static void device_test_exit(const char *stage) {
#ifdef ACNH_SAVE_TESTING
    const char *requested = getenv("ACNH_SAVE_EXITPOINT");
    if (requested && !strcmp(requested, stage)) _exit(77);
#else
    (void)stage;
#endif
}
static int device_sync_fd(int fd, const char *stage) {
    if (device_test_failure(stage)) return -1;
    if (!fcntl(fd, F_FULLFSYNC)) return 0;
    if (errno != EINVAL && errno != ENOTSUP && errno != ENOTTY) return -1;
    return fsync(fd);
}
static int device_sync_directory(const char *path, const char *stage) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    int result = device_sync_fd(fd, stage);
    int saved = errno;
    if (close(fd) && !result) result = -1;
    errno = saved;
    return result;
}
static int device_write_atomic(const char *base, const char *path, const void *data, size_t size,
                               const char *stage, int *published) {
    char temporary[DEVICE_PATH_CAP]; int fd = -1;
    if (published) *published = 0;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        int n = snprintf(temporary, sizeof temporary, "%s/.%s.tmp.%d.%u",
                         base, DEVICE_SAVE_APP_ID_NAME, getpid(), attempt);
        if (n < 0 || (size_t)n >= sizeof temporary) { errno = ENAMETOOLONG; return -1; }
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return -1;
    size_t written = 0; int result = 0;
    while (written < size) {
        ssize_t n = write(fd, (const uint8_t *)data + written, size - written);
        if (n <= 0) { result = -1; break; }
        written += (size_t)n;
    }
    if (!result && device_sync_fd(fd, stage)) result = -1;
    if (close(fd)) result = -1;
    if (!result && device_test_failure(!strcmp(stage, "marker-sync") ? "marker-rename" : "meta-rename")) result = -1;
    if (!result && rename(temporary, path)) result = -1;
    if (result) { unlink(temporary); return -1; }
    if (published) *published = 1;
    device_test_exit(!strcmp(stage, "marker-sync") ? "after-marker-rename" : "after-meta-rename");
    return device_sync_directory(base, !strcmp(stage, "marker-sync") ? "marker-dir-sync" : "meta-dir-sync");
}
static int device_metadata_try_write(const char *base, const char *path, DeviceSaveMetadata *metadata, int *published) {
    metadata->checksum = device_metadata_checksum(metadata);
    return device_write_atomic(base, path, metadata, sizeof *metadata, "meta-sync", published);
}
static void device_metadata_write_atomic(const char *base, const char *path, DeviceSaveMetadata *metadata) {
    if (device_metadata_try_write(base, path, metadata, NULL)) runtime_fail("publish device save metadata");
}
static uint64_t device_txn_checksum(const DeviceSaveTxn *txn) {
    const uint8_t *bytes = (const uint8_t *)txn; uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < offsetof(DeviceSaveTxn, checksum); i++) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
static DIR *device_open_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    DIR *dir = fdopendir(fd);
    if (!dir) close(fd);
    return dir;
}
static int device_tree_validate(const char *root) {
    DIR *dir = device_open_directory(root); if (!dir) return -1;
    int result = 0; struct dirent *entry;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[2048];
        if (snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= (int)sizeof path) { result = -1; break; }
        struct stat st; if (lstat(path, &st)) { result = -1; break; }
        if (S_ISDIR(st.st_mode)) result = device_tree_validate(path);
        else if (!S_ISREG(st.st_mode)) result = -1;
    }
    if (closedir(dir)) result = -1;
    return result;
}
static int device_tree_clear(const char *root) {
    DIR *dir = device_open_directory(root); if (!dir) return -1;
    int result = 0; struct dirent *entry;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[2048];
        if (snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= (int)sizeof path) { result = -1; break; }
        struct stat st; if (lstat(path, &st)) { result = -1; break; }
        result = S_ISDIR(st.st_mode) ? remove_tree(path) : unlink(path);
    }
    if (closedir(dir)) result = -1;
    return result;
}
static int device_tree_sync(const char *root, const char *file_stage, const char *dir_stage) {
    DIR *dir = device_open_directory(root); if (!dir) return -1;
    int result = 0; struct dirent *entry;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[2048];
        if (snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= (int)sizeof path) { result = -1; break; }
        struct stat st; if (lstat(path, &st)) { result = -1; break; }
        if (S_ISDIR(st.st_mode)) result = device_tree_sync(path, file_stage, dir_stage);
        else if (S_ISREG(st.st_mode)) {
            int fd = open(path, O_RDONLY | O_NOFOLLOW);
            if (fd < 0) result = -1;
            else { result = device_sync_fd(fd, file_stage); if (close(fd)) result = -1; }
        } else result = -1;
    }
    if (closedir(dir)) result = -1;
    if (!result) result = device_sync_directory(root, dir_stage);
    return result;
}
static int device_tree_copy(const char *from, const char *to) {
    if (device_test_failure("copy-enospc")) return -1;
    DIR *dir = device_open_directory(from); if (!dir) return -1;
    int result = 0; struct dirent *entry;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char src[2048], dst[2048];
        if (snprintf(src, sizeof src, "%s/%s", from, entry->d_name) >= (int)sizeof src ||
            snprintf(dst, sizeof dst, "%s/%s", to, entry->d_name) >= (int)sizeof dst) { result = -1; break; }
        struct stat st; if (lstat(src, &st)) { result = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(dst, 0700)) result = -1;
            else result = device_tree_copy(src, dst);
        } else if (S_ISREG(st.st_mode)) {
            int in = open(src, O_RDONLY | O_NOFOLLOW);
            int out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (in < 0 || out < 0) result = -1;
            char buffer[65536]; ssize_t n = 0;
            while (!result && (n = read(in, buffer, sizeof buffer)) > 0) {
                for (ssize_t at = 0; at < n;) { ssize_t w = write(out, buffer + at, (size_t)(n - at)); if (w <= 0) { result = -1; break; } at += w; }
            }
            if (!result && n < 0) result = -1;
            if (out >= 0 && close(out)) result = -1;
            if (in >= 0 && close(in)) result = -1;
        } else result = -1;
    }
    if (closedir(dir)) result = -1;
    return result;
}

static void device_metadata_load_or_create(const char *root, const char *base, const char *path,
                                           DeviceSaveMetadata *out) {
    if (device_store_loaded && device_store_staging) { *out = device_store_meta; return; }
    struct stat st; char slot1[DEVICE_PATH_CAP], marker[DEVICE_PATH_CAP];
    if (snprintf(slot1, sizeof slot1, "%s/.%s.slot1", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof slot1 ||
        snprintf(marker, sizeof marker, "%s/.%s.txn", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof marker)
        runtime_fail("device save path too long");
    if (!lstat(path, &st)) {
        if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) ||
            (st.st_size != sizeof *out && st.st_size != sizeof(DeviceSaveMetadataV1)))
            runtime_fail("device save metadata type or length invalid");
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) runtime_fail("open device save metadata");
        uint8_t bytes[sizeof *out]; size_t length = (size_t)st.st_size, read_bytes = 0;
        while (read_bytes < length) {
            ssize_t n = read(fd, bytes + read_bytes, length - read_bytes);
            if (n <= 0) { close(fd); runtime_fail("read device save metadata"); }
            read_bytes += (size_t)n;
        }
        struct stat after;
        int bad = fstat(fd, &after) || !S_ISREG(after.st_mode) || after.st_size != (off_t)length;
        close(fd);
        if (bad) runtime_fail("device save metadata changed while reading");
        if (length == sizeof(DeviceSaveMetadataV1)) {
            DeviceSaveMetadataV1 old; memcpy(&old, bytes, sizeof old);
            uint64_t hash = 1469598103934665603ULL;
            for (size_t i = 0; i < offsetof(DeviceSaveMetadataV1, checksum); i++) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
            if (old.version != 1 || old.checksum != hash) runtime_fail("legacy device save metadata invalid");
            *out = (DeviceSaveMetadata){ .magic = old.magic, .version = DEVICE_SAVE_META_VERSION,
                .reserved = old.reserved, .application_id = old.application_id,
                .normal_size = old.normal_size, .journal_size = old.journal_size };
        } else memcpy(out, bytes, sizeof *out);
        if (out->magic != DEVICE_SAVE_META_MAGIC || out->version != DEVICE_SAVE_META_VERSION || out->reserved ||
            out->application_id != TOTK_APPLICATION_ID || out->normal_size < DEVICE_SAVE_INITIAL_SIZE ||
            out->journal_size < DEVICE_SAVE_INITIAL_SIZE || out->normal_size > DEVICE_SAVE_MAX_SIZE ||
            out->journal_size > DEVICE_SAVE_MAX_SIZE || out->active_slot > 1 || out->restore_pending > 1 ||
            (length == sizeof *out && out->checksum != device_metadata_checksum(out)))
            runtime_fail("device save metadata contents invalid");
        if (lstat(root, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
            runtime_fail("device save payload root missing or invalid for existing metadata");
        if (length == sizeof(DeviceSaveMetadataV1)) {
            if (lstat(marker, &st) != -1 || errno != ENOENT) runtime_fail("legacy device save marker exists");
            if (lstat(slot1, &st)) { if (errno != ENOENT || mkdir(slot1, 0700)) runtime_fail("create second device save slot"); }
            else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || device_directory_empty(slot1) != 1)
                runtime_fail("legacy device save second slot invalid");
            if (device_sync_directory(slot1, "clone-dir-sync") || device_sync_directory(base, "clone-dir-sync"))
                runtime_fail("sync second device save slot");
            device_metadata_write_atomic(base, path, out);
        } else if (lstat(slot1, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
            runtime_fail("second device save slot missing or invalid");
        const char *active = out->active_slot ? slot1 : root;
        if (lstat(active, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) runtime_fail("active device save slot missing");
        if (!lstat(marker, &st)) {
            if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || st.st_size != sizeof(DeviceSaveTxn)) runtime_fail("device save marker invalid");
            DeviceSaveTxn txn; fd = open(marker, O_RDONLY | O_NOFOLLOW);
            if (fd < 0 || read(fd, &txn, sizeof txn) != sizeof txn) runtime_fail("read device save marker");
            if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != sizeof txn)
                runtime_fail("device save marker changed while reading");
            close(fd);
            if (txn.magic != DEVICE_SAVE_TXN_MAGIC || txn.version != 1 || txn.target > 1 ||
                txn.checksum != device_txn_checksum(&txn)) runtime_fail("device save marker contents invalid");
            if (txn.target != out->active_slot) {
                out->restore_pending = 1;
                device_metadata_write_atomic(base, path, out);
            }
            if (unlink(marker) || device_sync_directory(base, "marker-dir-sync")) runtime_fail("clear device save recovery marker");
        } else if (errno != ENOENT) runtime_fail("inspect device save marker");
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
    if (lstat(slot1, &st)) { if (errno != ENOENT || mkdir(slot1, 0700)) runtime_fail("create second device save slot"); }
    else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || device_directory_empty(slot1) != 1)
        runtime_fail("second device save slot invalid on first launch");
    if (lstat(marker, &st) != -1 || errno != ENOENT) runtime_fail("device save marker without metadata");
    if (device_sync_directory(root, "clone-dir-sync") || device_sync_directory(slot1, "clone-dir-sync") ||
        device_sync_directory(base, "clone-dir-sync")) runtime_fail("sync initial device save slots");
    device_metadata_write_atomic(base, path, out);
}

static void device_store_attach(const char *base, const char *root, const char *meta, const DeviceSaveMetadata *metadata) {
    if (snprintf(device_store_base, sizeof device_store_base, "%s", base) >= (int)sizeof device_store_base ||
        snprintf(device_store_meta_path, sizeof device_store_meta_path, "%s", meta) >= (int)sizeof device_store_meta_path ||
        snprintf(device_store_slots[0], sizeof device_store_slots[0], "%s", root) >= (int)sizeof device_store_slots[0] ||
        snprintf(device_store_slots[1], sizeof device_store_slots[1], "%s/.%s.slot1", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof device_store_slots[1] ||
        snprintf(device_store_txn_path, sizeof device_store_txn_path, "%s/.%s.txn", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof device_store_txn_path)
        runtime_fail("device save store path too long");
    if (strlen(device_store_slots[0]) >= sizeof mounts[0].root ||
        strlen(device_store_slots[1]) >= sizeof mounts[0].root)
        runtime_fail("device save mount path too long");
    device_store_meta = *metadata;
    device_store_loaded = 1;
}
static void device_store_update_mounts(const char *root) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++) if (mounts[i].kind && mounts[i].device)
        snprintf(mounts[i].root, sizeof mounts[i].root, "%s", root);
    pthread_mutex_unlock(&lock);
}
static unsigned device_stage_begin(void) {
    if (!device_store_loaded) return R_INVALID_ARGUMENT;
    if (device_store_finalizing) return R_INVALID_ARGUMENT;
    if (device_store_staging) return 0;
    unsigned target = 1u - device_store_meta.active_slot;
    const char *inactive = device_store_slots[target];
    struct stat st;
    if (lstat(inactive, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
        device_tree_validate(inactive)) return R_INVALID_PATH;
    if (device_tree_clear(inactive)) return R_INVALID_ARGUMENT;
    device_test_exit("after-clear-before-clone");
    if (device_tree_copy(device_store_slots[device_store_meta.active_slot], inactive) ||
        device_tree_sync(inactive, "clone-sync", "clone-dir-sync") ||
        device_sync_directory(device_store_base, "clone-dir-sync")) return R_INVALID_ARGUMENT;
    device_test_exit("after-clone-before-marker");
    DeviceSaveTxn txn = { .magic = DEVICE_SAVE_TXN_MAGIC, .version = 1, .target = target };
    txn.checksum = device_txn_checksum(&txn);
    int published = 0;
    if (device_write_atomic(device_store_base, device_store_txn_path, &txn, sizeof txn, "marker-sync", &published))
        return R_INVALID_ARGUMENT;
    device_test_exit("after-marker");
    device_store_staging = 1;
    device_store_generation++;
    device_store_update_mounts(inactive);
    return 0;
}
static unsigned device_finalize_locked(void) {
    if (device_store_finalizing == DEVICE_FINAL_META_SYNC) {
        if (device_sync_directory(device_store_base, "meta-dir-sync")) return R_INVALID_ARGUMENT;
        device_store_finalizing = DEVICE_FINAL_MARKER_UNLINK;
    }
    if (device_store_finalizing == DEVICE_FINAL_MARKER_UNLINK) {
        if (device_test_failure("marker-unlink")) return R_INVALID_ARGUMENT;
        if (unlink(device_store_txn_path) && errno != ENOENT) return R_INVALID_ARGUMENT;
        device_store_finalizing = DEVICE_FINAL_MARKER_SYNC;
    }
    if (device_store_finalizing == DEVICE_FINAL_MARKER_SYNC) {
        if (device_sync_directory(device_store_base, "marker-cleanup-dir-sync")) return R_INVALID_ARGUMENT;
        device_store_finalizing = DEVICE_FINAL_NONE;
        atomic_store(&device_save_dirty, 0);
    }
    return 0;
}
/* 返回的位标记表示持有的锁：1 设备存档，2 挂载生命周期。 */
static unsigned device_path_enter(const char *path, int mutate, int *held) {
    *held = 0;
    if (!path) return R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return R_INVALID_PATH;
    if (mutate) { const char *rest = colon + 2; while (*rest == '/') rest++; if (!*rest) return R_INVALID_PATH; }
    pthread_mutex_lock(&mount_lifecycle_lock);
    size_t n = (size_t)(colon - path); int device = 0, found = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++) if (mounts[i].kind &&
        strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) {
        found = 1; device = mounts[i].device; break;
    }
    pthread_mutex_unlock(&lock);
    if (!found) { pthread_mutex_unlock(&mount_lifecycle_lock); return R_PATH_NOT_FOUND; }
#ifdef ACNH_SAVE_TESTING
    int armed = 1;
    if (atomic_compare_exchange_strong(&device_test_classification_pause, &armed, 2))
        while (atomic_load(&device_test_classification_pause) == 2) usleep(1000);
#endif
    if (!device) { *held = 2; return 0; }
    pthread_mutex_lock(&device_save_lock);
    if (device_store_finalizing) { pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock); return R_INVALID_ARGUMENT; }
    if (mutate) {
        char checked[1100]; unsigned result = host_path(path, checked, sizeof checked);
        if (!result) result = device_stage_begin();
        if (result) { pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock); return result; }
    }
    *held = 3;
    return 0;
}

static void device_path_leave(int held) {
    if (held & 1) pthread_mutex_unlock(&device_save_lock);
    if (held & 2) pthread_mutex_unlock(&mount_lifecycle_lock);
}

static unsigned mount_device_save_data(const char *name) {
    pthread_mutex_lock(&mount_lifecycle_lock);
    unsigned name_result = device_mount_name_check(name);
    if (name_result) { pthread_mutex_unlock(&mount_lifecycle_lock); return name_result; }
    pthread_mutex_lock(&device_save_lock);
    if (device_store_finalizing && device_finalize_locked()) {
        pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock); return R_INVALID_ARGUMENT;
    }
    if (!device_profile_preflight()) { pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock); return R_INVALID_ARGUMENT; }
    ready();
    char base[DEVICE_PATH_CAP], root[DEVICE_PATH_CAP], meta[DEVICE_PATH_CAP];
    if (!device_save_paths(base, sizeof base, root, sizeof root, meta, sizeof meta) ||
        !device_directory_path(base, 1)) {
        pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock);
        return R_INVALID_ARGUMENT;
    }
    DeviceSaveMetadata metadata;
    device_metadata_load_or_create(root, base, meta, &metadata);
    device_store_attach(base, root, meta, &metadata);
    if (!device_directory_path(root, 0)) {
        pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock);
        return R_INVALID_ARGUMENT;
    }
    unsigned result = mount_add_device(name, device_store_slots[metadata.active_slot ^ (unsigned)device_store_staging]);
    pthread_mutex_unlock(&device_save_lock);
    pthread_mutex_unlock(&mount_lifecycle_lock);
    return result;
}

static unsigned get_device_save_data_size(int64_t *normal, int64_t *journal) {
    if (!normal || !journal) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&device_save_lock);
    if (device_store_finalizing) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
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
    if (device_store_loaded && !device_store_staging) device_store_meta = metadata;
    pthread_mutex_unlock(&device_save_lock);
    *normal = metadata.normal_size;
    *journal = metadata.journal_size;
    return 0;
}

static unsigned get_device_save_restore_flag(bool *out, const char *name) {
    if (!out || !name) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&mount_lifecycle_lock);
    pthread_mutex_lock(&lock);
    int found = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind == M_SAVE && mounts[i].device && !strcmp(mounts[i].name, name)) found = 1;
    pthread_mutex_unlock(&lock);
    if (!found) { pthread_mutex_unlock(&mount_lifecycle_lock); return R_PATH_NOT_FOUND; }
    pthread_mutex_lock(&device_save_lock);
    if (device_store_finalizing) { pthread_mutex_unlock(&device_save_lock); pthread_mutex_unlock(&mount_lifecycle_lock); return R_INVALID_ARGUMENT; }
    *out = device_store_meta.restore_pending != 0;
    pthread_mutex_unlock(&device_save_lock);
    pthread_mutex_unlock(&mount_lifecycle_lock);
    return 0;
}

static unsigned commit(const char *name);
static unsigned commit_multiple(const char *const *names, int count) {
    if (count < 0 || count > 16 || (count > 0 && !names)) return R_INVALID_ARGUMENT;
    if (count > 1) { fprintf(stderr, "[acnh-save] Commit rejected: %d mount names require cross-store atomic commit\n", count); return R_INVALID_ARGUMENT; } /* 跨物理 store 的原子提交尚未实现。 */
    if (!count) return 0;
    if (!names[0]) return R_INVALID_ARGUMENT;
    return commit(names[0]);
}

static unsigned commit_with_options(const char *name, const void *option) {
    if (!name || !option) return R_INVALID_ARGUMENT;
    uint32_t value; memcpy(&value, option, sizeof value);
    /* 1 是 ClearRestoreFlag：有暂存时提交本身清除恢复提示；没有暂存时单独改写元数据清除。2 SetRestoreFlag 尚未实现。 */
    if (value > 1) return R_INVALID_ARGUMENT;
    unsigned result = commit(name);
    if (result || value != 1) return result;
    pthread_mutex_lock(&mount_lifecycle_lock);
    pthread_mutex_lock(&lock);
    int device = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind == M_SAVE && mounts[i].device && !strcmp(mounts[i].name, name)) device = 1;
    pthread_mutex_unlock(&lock);
    if (device) {
        pthread_mutex_lock(&device_save_lock);
        if (device_store_staging || device_store_finalizing) result = R_INVALID_ARGUMENT;
        else if (device_store_meta.restore_pending) {
            DeviceSaveMetadata next = device_store_meta;
            next.restore_pending = 0;
            int published = 0;
            if (device_metadata_try_write(device_store_base, device_store_meta_path, &next, &published)) result = R_INVALID_ARGUMENT;
            if (published) device_store_meta = next;
        }
        pthread_mutex_unlock(&device_save_lock);
    }
    pthread_mutex_unlock(&mount_lifecycle_lock);
    return result;
}

/* 只允许把设备存档容量加大到上限以内，不能缩小。 */
static unsigned extend_device_save_data(int64_t normal, int64_t journal) {
    if (normal < DEVICE_SAVE_INITIAL_SIZE || journal < DEVICE_SAVE_INITIAL_SIZE ||
        normal > DEVICE_SAVE_MAX_SIZE || journal > DEVICE_SAVE_MAX_SIZE) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&device_save_lock);
    if (device_store_finalizing) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
    if (!device_profile_preflight()) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
    ready();
    char base[DEVICE_PATH_CAP], root[DEVICE_PATH_CAP], meta[DEVICE_PATH_CAP];
    if (!device_save_paths(base, sizeof base, root, sizeof root, meta, sizeof meta) ||
        !device_directory_path(base, 1)) {
        pthread_mutex_unlock(&device_save_lock);
        return R_INVALID_ARGUMENT;
    }
    DeviceSaveMetadata metadata;
    if (device_store_staging || device_store_writers) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
    char marker[DEVICE_PATH_CAP];
    if (snprintf(marker, sizeof marker, "%s/.%s.txn", base, DEVICE_SAVE_APP_ID_NAME) >= (int)sizeof marker) {
        pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
    }
    struct stat marker_st;
    if (!lstat(marker, &marker_st) || errno != ENOENT) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
    device_metadata_load_or_create(root, base, meta, &metadata);
    if (device_store_loaded) device_store_meta = metadata;
    if (normal < metadata.normal_size || journal < metadata.journal_size) {
        pthread_mutex_unlock(&device_save_lock);
        return R_INVALID_ARGUMENT;
    }
    if (normal != metadata.normal_size || journal != metadata.journal_size) {
        metadata.normal_size = normal;
        metadata.journal_size = journal;
        device_metadata_write_atomic(base, meta, &metadata);
        if (device_store_loaded) device_store_meta = metadata;
        fprintf(stderr, "[acnh] 设备存档容量扩到 normal %lld journal %lld\n", (long long)normal, (long long)journal);
    }
    pthread_mutex_unlock(&device_save_lock);
    return 0;
}
#endif

/* ---------- mounts and paths ---------- */
#ifndef GAME_ACNH
static unsigned device_path_enter(const char *path, int mutate, int *held) {
    (void)path; (void)mutate; *held = 0; return 0;
}
static void device_path_leave(int held) { (void)held; }
#endif
static unsigned mount_add_internal(const char *name, int kind, const char *root, int device) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return R_INVALID_ARGUMENT;
#ifdef GAME_ACNH
    if (!device) pthread_mutex_lock(&mount_lifecycle_lock);
#endif
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < 16; i++) { if (mounts[i].kind && !strcmp(mounts[i].name, name)) {
        pthread_mutex_unlock(&lock);
#ifdef GAME_ACNH
        if (!device) pthread_mutex_unlock(&mount_lifecycle_lock);
#endif
        return R_ALREADY_EXISTS;
    } if (!mounts[i].kind && slot < 0) slot = i; }
    if (slot < 0) runtime_fail("fs mount table full");
    mounts[slot].kind = kind; mounts[slot].device = device; strcpy(mounts[slot].name, name); snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    pthread_mutex_unlock(&lock);
#ifdef GAME_ACNH
    if (!device) pthread_mutex_unlock(&mount_lifecycle_lock);
#endif
#ifdef GAME_ACNH
    fprintf(stderr, "[acnh] fs mount %s\n", name);
#else
    fprintf(stderr, "[totk] fs mount %s: -> %s\n", name, root);
#endif
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
static unsigned save_change_path(const char *path, char *out, size_t cap, int device) {
    int kind = M_FREE; unsigned r = host_path_kind(path, out, cap, &kind);
    if (!r && kind == M_SAVE) save_touched_device(device);
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
#ifdef GAME_ACNH
    pthread_mutex_lock(&mount_lifecycle_lock);
#endif
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) {
        mounts[i].kind = M_FREE; pthread_mutex_unlock(&lock);
#ifdef GAME_ACNH
        pthread_mutex_unlock(&mount_lifecycle_lock);
#endif
        return;
    }
    pthread_mutex_unlock(&lock);
#ifdef GAME_ACNH
    pthread_mutex_unlock(&mount_lifecycle_lock);
#endif
    BOTW_AOC_UNMOUNT(name) if (!runtime_romfs_owns(name)) runtime_fail("unmount of unknown fs mount");
}
static unsigned commit_while_mounted(const char *name) {
    if (!name) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock); int found = 0, save = 0, device = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) {
        found = 1; save = mounts[i].kind == M_SAVE; device = mounts[i].device;
    }
    pthread_mutex_unlock(&lock);
    if (!found) return R_PATH_NOT_FOUND;
#ifndef GAME_ACNH
    (void)device;
#endif
#ifdef GAME_ACNH
    if (device) {
        pthread_mutex_lock(&device_save_lock);
        if (device_store_writers) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
        if (device_store_finalizing) {
            unsigned result = device_finalize_locked();
            pthread_mutex_unlock(&device_save_lock);
            return result;
        }
        if (!device_store_staging) { pthread_mutex_unlock(&device_save_lock); return 0; }
        unsigned target = 1u - device_store_meta.active_slot;
        if (device_tree_sync(device_store_slots[target], "commit-sync", "commit-dir-sync") ||
            device_sync_directory(device_store_base, "commit-dir-sync")) {
            pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
        }
        device_test_exit("before-meta");
        DeviceSaveMetadata next = device_store_meta;
        next.active_slot = target;
        next.restore_pending = 0; /* 本机策略：一次成功提交清除恢复提示。 */
        int published = 0;
        int error = device_metadata_try_write(device_store_base, device_store_meta_path, &next, &published);
        if (published) {
            device_store_meta = next;
            device_store_staging = 0;
            device_store_finalizing = error ? DEVICE_FINAL_META_SYNC : DEVICE_FINAL_MARKER_UNLINK;
            device_store_generation++;
            device_store_update_mounts(device_store_slots[target]);
        }
        if (error) { pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT; }
        device_test_exit("after-meta");
        unsigned result = device_finalize_locked();
        pthread_mutex_unlock(&device_save_lock);
        return result;
    }
#endif
    for (int i = 0; i < NFILES; i++) { pthread_mutex_lock(&lock); int fd = files[i].fd; pthread_mutex_unlock(&lock); if (fd > 0) fcntl(fd, F_FULLFSYNC); }
    if (save && !(getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 23)) atomic_store(&save_dirty, 0);   /* 23: probe-save's negative control */
    return 0;
}
static unsigned commit(const char *name) {
#ifdef GAME_ACNH
    pthread_mutex_lock(&mount_lifecycle_lock);
#endif
    unsigned result = commit_while_mounted(name);
#ifdef GAME_ACNH
    pthread_mutex_unlock(&mount_lifecycle_lock);
#endif
    return result;
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
    int device_held = 0;
    unsigned entered = device_path_enter(path, (mode & OPEN_WRITE) != 0, &device_held);
    if (entered) return entered;
    char p[1100]; int kind = M_FREE; unsigned r = host_path_kind(path, p, sizeof p, &kind);
    if (r) { device_path_leave(device_held); return r; }
    struct stat st; if (stat(p, &st) || !S_ISREG(st.st_mode)) { device_path_leave(device_held); return R_PATH_NOT_FOUND; }
    int save_writer = kind == M_SAVE && (mode & OPEN_WRITE);
    int fd = open(p, (mode & OPEN_WRITE ? (mode & OPEN_READ ? O_RDWR : O_WRONLY) : O_RDONLY) | O_NOFOLLOW);
    if (fd < 0) { device_path_leave(device_held); return errno_result(); }
    runtime_autoload_save_file_opened(path);   /* TOTK_AUTOLOAD: a slot's caption.sav starts the presses, its progress.sav ends them */
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++) if (files[i].fd <= 0) {
        files[i] = (File){ .fd = fd, .mode = mode, .save = save_writer,
            .device = device_held & 1,
#ifdef GAME_ACNH
            .generation = (device_held & 1) ? device_store_generation : 0,
#endif
        };
        *out = FILE_BASE + (uint64_t)i; pthread_mutex_unlock(&lock);
#ifdef GAME_ACNH
        if ((device_held & 1) && save_writer) device_store_writers++;
#endif
        device_path_leave(device_held);
        if (save_writer) { atomic_fetch_add(&save_writers, 1); save_touched_device(device_held & 1); }
        return 0;
    }
    pthread_mutex_unlock(&lock); device_path_leave(device_held);
    runtime_fail("host file handle table full");
}
static unsigned read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (runtime_romfs_handle(h)) return runtime_romfs_read(out, h, offset, buffer, size); BOTW_AOC_READ(out, h, offset, buffer, size)
    int fd, mode; File *f = (!out || offset < 0 || (!buffer && size)) ? NULL : file_of(h, &fd, &mode);
    if (!f) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_READ)) return R_OPEN_MODE;
#ifdef GAME_ACNH
    if (f->device) { pthread_mutex_lock(&device_save_lock); if (f->generation != device_store_generation) {
        pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
    } }
#endif
    dm_guest_write_prepare(buffer, (size_t)size);
    uint64_t done = 0;
    while (done < size) { ssize_t n = pread(fd, (uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done); if (n < 0) runtime_fail("host save read failure"); if (!n) break; done += (uint64_t)n; }
    *out = done;
#ifdef GAME_ACNH
    if (f->device) pthread_mutex_unlock(&device_save_lock);
#endif
    return 0;
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
        save_touched_device(f->device);
        static int delay_ms = -1; if (delay_ms < 0) { const char *e = getenv("TOTK_SAVE_WRITE_DELAY_MS"); delay_ms = e ? atoi(e) : 0; }
        if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);   /* tests only: slow saves down so a window close lands in the middle of one */
    }
    while (done < size) { ssize_t n = pwrite(fd, (const uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done); if (n <= 0) runtime_fail("host save write failure"); done += (uint64_t)n; }
    if (option && (*option & 1)) {
#ifdef GAME_ACNH
        if (f->device) return device_sync_fd(fd, "write-sync") ? R_INVALID_ARGUMENT : 0;
#endif
        fcntl(fd, F_FULLFSYNC);
    }
    return 0;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (runtime_romfs_handle(h)) return runtime_romfs_size(out, h); BOTW_AOC_SIZE(out, h)
    int fd; struct stat st; File *f = out ? file_of(h, &fd, NULL) : NULL;
    if (!f) return R_INVALID_ARGUMENT;
#ifdef GAME_ACNH
    if (f->device) { pthread_mutex_lock(&device_save_lock); if (f->generation != device_store_generation) {
        pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
    } }
#endif
    int bad = fstat(fd, &st);
#ifdef GAME_ACNH
    if (f->device) pthread_mutex_unlock(&device_save_lock);
#endif
    if (bad) return R_INVALID_ARGUMENT;
    *out = st.st_size; return 0;
}
static unsigned set_file_size(uint64_t h, int64_t size) {
    int fd, mode; if (size < 0 || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    if (files[h - FILE_BASE].save) save_touched_device(files[h - FILE_BASE].device);
    return ftruncate(fd, size) ? errno_result() : 0;
}
static unsigned flush_file(uint64_t h) {
    int fd; File *f = file_of(h, &fd, NULL); if (!f) return R_INVALID_ARGUMENT;
#ifdef GAME_ACNH
    if (f->device) { pthread_mutex_lock(&device_save_lock); if (f->generation != device_store_generation) {
        pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
    }
        unsigned result = device_sync_fd(fd, "flush-sync") ? R_INVALID_ARGUMENT : 0;
        pthread_mutex_unlock(&device_save_lock); return result;
    }
#endif
    fcntl(fd, F_FULLFSYNC); return 0;
}
static void close_file(uint64_t h) {
    if (runtime_romfs_handle(h)) { runtime_romfs_close(h); return; } BOTW_AOC_CLOSE(h)
    if (!file_of(h, NULL, NULL)) runtime_fail("close invalid file handle");
    int device = files[h - FILE_BASE].device;
    if (device) pthread_mutex_lock(&device_save_lock);
    pthread_mutex_lock(&lock); int fd = files[h - FILE_BASE].fd, save = files[h - FILE_BASE].save;
    files[h - FILE_BASE].fd = 0; pthread_mutex_unlock(&lock);
    close(fd);
    if (device && save) {
#ifdef GAME_ACNH
        device_store_writers--;
#endif
    }
    if (save) { save_touched_device(device); atomic_fetch_sub(&save_writers, 1); }
    if (device) pthread_mutex_unlock(&device_save_lock);
}
static unsigned create_file(const char *path, int64_t size) {
    if (size < 0) return R_INVALID_ARGUMENT;
    int held = 0; unsigned r = device_path_enter(path, 1, &held); if (r) return r;
    char p[1100]; r = save_change_path(path, p, sizeof p, held & 1);
    if (!r) { int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) r = errno_result();
        else { int bad = ftruncate(fd, size); if (close(fd)) bad = -1; if (bad) r = errno_result(); }
    }
    device_path_leave(held);
    return r;
}
static unsigned delete_file(const char *path) {
    int held = 0; unsigned r = device_path_enter(path, 1, &held); if (r) return r;
    char p[1100]; r = save_change_path(path, p, sizeof p, held & 1);
    if (!r && unlink(p)) r = errno_result();
    device_path_leave(held);
    return r;
}
static unsigned rename_entry(const char *from, const char *to, int directory) {
    int held = 0; unsigned r = device_path_enter(from, 1, &held); if (r) return r;
    if (held & 1) { const char *a = strchr(from, ':'), *b = to ? strchr(to, ':') : NULL;
        if ((held & 1) && (!b || b - to != a - from || strncmp(from, to, (size_t)(a - from)))) r = R_INVALID_ARGUMENT;
    }
#ifdef GAME_ACNH
    if (!r && !(held & 1) && to) {
        const char *colon = strchr(to, ':');
        if (!colon || colon[1] != '/') r = R_INVALID_PATH;
        else {
            size_t n = (size_t)(colon - to);
            pthread_mutex_lock(&lock);
            for (int i = 0; i < 16; i++)
                if (mounts[i].kind && mounts[i].device && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, to, n))
                    r = R_INVALID_ARGUMENT;
            pthread_mutex_unlock(&lock);
        }
    }
#endif
    char a[1100], b[1100]; if (!r) r = save_change_path(from, a, sizeof a, held & 1);
    if (!r) r = host_path(to, b, sizeof b);
    if (!r && directory) { struct stat st; if (stat(a, &st) || !S_ISDIR(st.st_mode)) r = R_PATH_NOT_FOUND; }
    if (!r) { struct stat st; if (!stat(b, &st)) r = R_ALREADY_EXISTS;
        else if (rename(a, b)) r = errno_result(); }
    device_path_leave(held);
    return r;
}
static unsigned rename_file(const char *from, const char *to) { return rename_entry(from, to, 0); }
#ifdef GAME_ACNH
static unsigned rename_directory(const char *from, const char *to) { return rename_entry(from, to, 1); }
#endif
static unsigned entry_type(int *out, const char *path) {
    if (runtime_romfs_owns(path)) return runtime_romfs_entry_type(out, path); BOTW_AOC_TYPE(out, path)
    if (!out) return R_INVALID_ARGUMENT;
    int held = 0; unsigned r = device_path_enter(path, 0, &held); if (r) return r;
    char p[1100]; r = host_path(path, p, sizeof p); struct stat st;
    if (!r && stat(p, &st)) r = R_PATH_NOT_FOUND;
    if (!r) *out = S_ISDIR(st.st_mode) ? 0 : 1;
    device_path_leave(held);
    return r;
}

/* ---------- directories ---------- */
typedef RomfsDirectoryEntry DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");
_Static_assert(offsetof(DirectoryEntry, type) == 0x304 && offsetof(DirectoryEntry, size) == 0x308, "nn::fs::DirectoryEntry fields");
static unsigned create_directory(const char *path) {
    int held = 0; unsigned r = device_path_enter(path, 1, &held); if (r) return r;
    char p[1100]; r = save_change_path(path, p, sizeof p, held & 1);
    if (!r && mkdir(p, 0700)) r = errno_result();
    device_path_leave(held);
    return r;
}
static unsigned delete_recursive(const char *path) {
    int held = 0; unsigned r = device_path_enter(path, 1, &held); if (r) return r;
    char p[1100]; r = save_change_path(path, p, sizeof p, held & 1);
    if (!r) { struct stat st; if (stat(p, &st)) r = R_PATH_NOT_FOUND;
        else if (remove_tree(p)) r = R_INVALID_ARGUMENT; }
    device_path_leave(held);
    return r;
}
#ifdef GAME_ACNH
/* 清空目录内容但保留目录本身；ACNH 首次存档会清空整个设备存档根目录。 */
static unsigned clean_recursive(const char *path) {
    int held = 0; unsigned r = device_path_enter(path, 0, &held); if (r) return r;
    char p[1100];
    /* device_path_enter 的写入检查不接受挂载根，这里自己开始暂存。 */
    if (held & 1) { r = host_path(path, p, sizeof p); if (!r) r = device_stage_begin(); }
    if (!r) r = save_change_path(path, p, sizeof p, held & 1);
    if (!r) { struct stat st; if (stat(p, &st) || !S_ISDIR(st.st_mode)) r = R_PATH_NOT_FOUND;
        else if (clear_tree(p)) r = R_INVALID_ARGUMENT; }
    device_path_leave(held);
    return r;
}
#endif
static unsigned open_directory(uint64_t *out, const char *path, int mode) {
    if (runtime_romfs_owns(path)) {
#if defined(GAME_ACNH)
        return runtime_romfs_open_directory(out, path, mode);
#else
        fprintf(stderr, "[totk] OpenDirectory on RomFS %s\n", path); runtime_fail("RomFS directory listing not implemented");
#endif
    }
    int device_held = 0; unsigned r = device_path_enter(path, 0, &device_held); if (r) return r;
    char p[1100]; r = host_path(path, p, sizeof p); if (r) { device_path_leave(device_held); return r; }
#if defined(GAME_ACNH)
    uint32_t bits=(uint32_t)mode;if (!out || !(bits&3) || (bits&~(1u|2u|0x80000000u))) {
        device_path_leave(device_held); return R_INVALID_ARGUMENT;
    }
#else
    if (!out || mode < 1 || mode > 3) { device_path_leave(device_held); return R_INVALID_ARGUMENT; }
#endif
    if (strlen(p) >= sizeof dirs[0].path) {
        device_path_leave(device_held);
        return R_INVALID_PATH;
    }
    DIR *d = opendir(p); if (!d) { device_path_leave(device_held); return errno_result(); }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++) if (!dirs[i].dir) {
        dirs[i].dir = d; dirs[i].mode = mode; dirs[i].device = device_held & 1;
#ifdef GAME_ACNH
        dirs[i].generation = (device_held & 1) ? device_store_generation : 0;
#endif
        strcpy(dirs[i].path, p); *out = DIR_BASE + (uint64_t)i;
        pthread_mutex_unlock(&lock); device_path_leave(device_held); return 0;
    }
    pthread_mutex_unlock(&lock); device_path_leave(device_held);
    runtime_fail("host directory handle table full");
}
static unsigned read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (runtime_romfs_directory_handle(h)) return runtime_romfs_read_directory(count,out,h,max);
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE]; if (!d->dir) return R_INVALID_ARGUMENT;
#ifdef GAME_ACNH
    if (d->device) { pthread_mutex_lock(&device_save_lock); if (d->generation != device_store_generation) {
        pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
    } }
#endif
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
    *count = n;
#ifdef GAME_ACNH
    if (d->device) pthread_mutex_unlock(&device_save_lock);
#endif
    return 0;
}
static void close_directory(uint64_t h) {
    if (runtime_romfs_directory_handle(h)) { runtime_romfs_close_directory(h); return; }
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].dir) runtime_fail("close invalid directory handle");
    pthread_mutex_lock(&lock); closedir(dirs[h - DIR_BASE].dir); dirs[h - DIR_BASE].dir = NULL; pthread_mutex_unlock(&lock);
}
#ifdef GAME_ACNH
/* 与读取位置无关，统计句柄打开模式下的全部条目。RomFS 游标不记总数，游戏只在联网证书目录上调用。 */
static unsigned directory_entry_count(int64_t *out, uint64_t h) {
    if (runtime_romfs_directory_handle(h) || !out || h < DIR_BASE || h >= DIR_BASE + NDIRS) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock); Dir d = dirs[h - DIR_BASE]; pthread_mutex_unlock(&lock);
    if (!d.dir) return R_INVALID_ARGUMENT;
    if (d.device) { pthread_mutex_lock(&device_save_lock); if (d.generation != device_store_generation) {
        pthread_mutex_unlock(&device_save_lock); return R_INVALID_ARGUMENT;
    } }
    DIR *it = opendir(d.path); unsigned r = it ? 0 : errno_result(); int64_t n = 0; struct dirent *e;
    while (it && (e = readdir(it))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 768) continue;
        char child[2048]; snprintf(child, sizeof child, "%s/%s", d.path, e->d_name); struct stat st; if (stat(child, &st)) continue;
        if ((uint32_t)d.mode & (S_ISDIR(st.st_mode) ? 1u : 2u)) n++;
    }
    if (it) closedir(it);
    if (d.device) pthread_mutex_unlock(&device_save_lock);
    if (!r) *out = n;
    return r;
}
#endif
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
        {"_ZN2nn2fs25CleanDirectoryRecursivelyEPKc", clean_recursive}, {"_ZN2nn2fs15RenameDirectoryEPKcS2_", rename_directory},
        {"_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", directory_entry_count},
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
