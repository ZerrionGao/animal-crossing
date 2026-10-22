#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../third_party/macnativegames/games/totk/runtime/fs_host.h"
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    exit(42);
}
uint64_t runtime_now_ns(void) { static _Atomic uint64_t now; return atomic_fetch_add(&now, 1000000); }
void runtime_account_uid(uint64_t out[2]) {
    const char *value = getenv("TEST_UID");
    uint64_t uid = value ? strtoull(value, NULL, 0) : 0x1122334455667788ULL;
    out[0] = uid;
    out[1] = 0x8877665544332211ULL;
}
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void runtime_fs_log(const char *what, const char *path) { (void)what; (void)path; }
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
int runtime_romfs_owns(const char *path) { (void)path; return 0; }
int runtime_romfs_handle(uint64_t handle) { (void)handle; return 0; }
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) { (void)out; (void)path; (void)mode; return 1; }
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size) { (void)out; (void)h; (void)off; (void)buf; (void)size; return 1; }
unsigned runtime_romfs_size(int64_t *out, uint64_t h) { (void)out; (void)h; return 1; }
void runtime_romfs_close(uint64_t h) { (void)h; }
unsigned runtime_romfs_entry_type(int *out, const char *path) { (void)out; (void)path; return 1; }
int runtime_romfs_directory_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_romfs_open_directory(uint64_t *out, const char *path, int mode) { (void)out; (void)path; (void)mode; return 1; }
unsigned runtime_romfs_read_directory(int64_t *count, RomfsDirectoryEntry *out, uint64_t h, int64_t max) { (void)count; (void)out; (void)h; (void)max; return 1; }
void runtime_romfs_close_directory(uint64_t h) { (void)h; }

typedef unsigned (*MountDeviceSaveDataFn)(const char *);
typedef unsigned (*GetDeviceSaveDataSizeFn)(int64_t *, int64_t *);
typedef unsigned (*CreateFileFn)(const char *, int64_t);
typedef unsigned (*CommitFn)(const char *);
typedef unsigned (*RestoreFlagFn)(bool *, const char *);
typedef unsigned (*CommitMultipleFn)(const char *const *, int);
typedef unsigned (*CommitOptionsFn)(const char *, const void *);
typedef unsigned (*ExtendDeviceSaveDataFn)(int64_t, int64_t);

typedef struct {
    uint64_t magic;
    uint32_t version, reserved;
    uint64_t application_id;
    int64_t normal_size, journal_size;
    uint64_t checksum;
} DeviceSaveMetadata;

#ifdef GAME_ACNH
static uint64_t metadata_checksum(const DeviceSaveMetadata *metadata) {
    const uint8_t *bytes = (const uint8_t *)metadata;
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < offsetof(DeviceSaveMetadata, checksum); i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void path_for(char *out, size_t capacity, const char *suffix) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    assert(profile);
    int n = snprintf(out, capacity, "%s/%s", profile, suffix);
    assert(n >= 0 && (size_t)n < capacity);
}
#endif

int main(int argc, char **argv) {
    MountDeviceSaveDataFn mount_device = (MountDeviceSaveDataFn)runtime_fs_host_override("_ZN2nn2fs19MountDeviceSaveDataEPKc");
    GetDeviceSaveDataSizeFn get_size = (GetDeviceSaveDataSizeFn)runtime_fs_host_override("_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_");
    CreateFileFn create_file = (CreateFileFn)runtime_fs_host_override("_ZN2nn2fs10CreateFileEPKcl");
    CommitFn commit = (CommitFn)runtime_fs_host_override("_ZN2nn2fs6CommitEPKc");
    RestoreFlagFn restore_flag = (RestoreFlagFn)runtime_fs_host_override("_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc");
    CommitMultipleFn commit_multiple = (CommitMultipleFn)runtime_fs_host_override("_ZN2nn2fs6CommitEPKPKci");
    CommitOptionsFn commit_options = (CommitOptionsFn)runtime_fs_host_override("_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE");
    ExtendDeviceSaveDataFn extend = (ExtendDeviceSaveDataFn)runtime_fs_host_override("_ZN2nn2fs20ExtendDeviceSaveDataEll");
#ifdef GAME_ACNH
    assert(mount_device && get_size && create_file && commit && restore_flag && commit_multiple && commit_options && extend);
    const char *mode = argc > 1 ? argv[1] : "first";
    if (!strcmp(mode, "invalid-name")) {
        assert(mount_device("invalid:name") == 0x2ee202u);
        char path[1200]; path_for(path, sizeof path, "device-save");
        assert(access(path, F_OK) != 0);
        return 0;
    }
    if (!strcmp(mode, "size-null")) {
        int64_t journal = 0;
        assert(get_size(NULL, &journal) == 0x2ee202u);
        char path[1200]; path_for(path, sizeof path, "device-save");
        assert(access(path, F_OK) != 0);
        return 0;
    }
    if (!strcmp(mode, "first")) {
        int64_t normal = 0, journal = 0;
        unsigned size_result = get_size(&normal, &journal);
        if (size_result || normal != 0x06f00000LL || journal != 0x06f00000LL)
            fprintf(stderr, "device size result=%u normal=%lld journal=%lld\n", size_result,
                    (long long)normal, (long long)journal);
        assert(size_result == 0 && normal == 0x06f00000LL && journal == 0x06f00000LL);
        assert(!runtime_fs_save_busy());
        assert(mount_device("device") == 0);
        char root[1200]; path_for(root, sizeof root, "device-save/01006f8002326000");
        struct stat st; assert(!lstat(root, &st) && S_ISDIR(st.st_mode));
        assert(create_file("device:/payload.bin", 16) == 0 && runtime_fs_save_busy());
        assert(commit("device") == 0 && !runtime_fs_save_busy());
        assert(get_size(&normal, &journal) == 0 && normal == 0x06f00000LL && journal == 0x06f00000LL);
        return 0;
    }
    if (!strcmp(mode, "reopen")) {
        int64_t normal = 0, journal = 0;
        assert(get_size(&normal, &journal) == 0 && normal == 0x06f00000LL && journal == 0x06f00000LL);
        assert(mount_device("device") == 0);
        char payload[1200]; path_for(payload, sizeof payload, "device-save/01006f8002326000/payload.bin");
        assert(access(payload, F_OK) == 0);
        return 0;
    }
    if (!strcmp(mode, "symlink-payload")) {
        assert(mount_device("device") == 0);
        assert(create_file("device:/linked-dir/file.bin", 1) == 0x2ee602u);
        return 0;
    }
    if (!strcmp(mode, "symlink-root-payload")) {
        assert(mount_device("device") == 0);
        char root[1200], moved[1240];
        path_for(root, sizeof root, "device-save/01006f8002326000");
        int n = snprintf(moved, sizeof moved, "%s.moved", root);
        assert(n >= 0 && (size_t)n < sizeof moved);
        const char *outside = getenv("DEVICE_SAVE_OUTSIDE");
        assert(outside && rename(root, moved) == 0 && symlink(outside, root) == 0);
        assert(create_file("device:/root-escape.bin", 1) == 0x2ee602u);
        assert(unlink(root) == 0 && rename(moved, root) == 0);
        return 0;
    }
    if (!strcmp(mode, "symlink-base-payload")) {
        assert(mount_device("device") == 0);
        char base[1200], moved[1240];
        path_for(base, sizeof base, "device-save");
        int n = snprintf(moved, sizeof moved, "%s.moved", base);
        assert(n >= 0 && (size_t)n < sizeof moved);
        const char *outside = getenv("DEVICE_SAVE_OUTSIDE");
        assert(outside && rename(base, moved) == 0 && symlink(outside, base) == 0);
        assert(create_file("device:/base-escape.bin", 1) == 0x2ee602u);
        assert(unlink(base) == 0 && rename(moved, base) == 0);
        return 0;
    }
    if (!strcmp(mode, "bad-size")) {
        int64_t normal = 0, journal = 0;
        assert(get_size(&normal, &journal) == 0);
        char meta_path[1200]; path_for(meta_path, sizeof meta_path, "device-save/01006f8002326000.meta");
        FILE *file = fopen(meta_path, "r+b"); assert(file);
        DeviceSaveMetadata metadata; assert(fread(&metadata, 1, sizeof metadata, file) == sizeof metadata);
        metadata.normal_size = 0x24000001LL;
        metadata.checksum = metadata_checksum(&metadata);
        assert(fseek(file, 0, SEEK_SET) == 0 && fwrite(&metadata, 1, sizeof metadata, file) == sizeof metadata);
        fclose(file);
        (void)get_size(&normal, &journal);
        assert(!"out-of-range metadata must fail");
    }
    if (!strcmp(mode, "bad-meta")) {
        int64_t normal = 0, journal = 0;
        (void)get_size(&normal, &journal);
        assert(!"corrupt metadata must fail");
    }
    if (!strcmp(mode, "missing-meta-with-file")) {
        int64_t normal = 0, journal = 0;
        (void)get_size(&normal, &journal);
        assert(!"existing payload without metadata must fail");
    }
    if (!strcmp(mode, "missing-root")) {
        int64_t normal = 0, journal = 0;
        assert(get_size(&normal, &journal) == 0x2ee202u);
        char path[1200]; path_for(path, sizeof path, "device-save/01006f8002326000");
        assert(access(path, F_OK) != 0);
        return 0;
    }
    if (!strcmp(mode, "preflight")) {
        assert(mount_device("device") == 0x2ee202u);
        return 0;
    }
    if (!strcmp(mode, "restore-guard")) (void)restore_flag((bool[1]){false}, "device");
    if (!strcmp(mode, "commit-multiple-guard")) (void)commit_multiple(NULL, 0);
    if (!strcmp(mode, "commit-options-guard")) (void)commit_options("device", NULL);
    if (!strcmp(mode, "extend-guard")) (void)extend(0x04000000LL, 0x04000000LL);
    assert(!"unknown probe mode");
#else
    (void)argc;
    (void)argv;
    assert(!mount_device && !get_size && create_file && commit && !restore_flag && !commit_multiple && !commit_options && !extend);
#endif
    return 0;
}
