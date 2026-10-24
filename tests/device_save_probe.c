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
typedef unsigned (*DeleteFileFn)(const char *);
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
    uint32_t active_slot, restore_pending;
    uint64_t checksum;
} DeviceSaveMetadata;

#ifdef GAME_ACNH
void acnh_save_test_pause_arm(void);
int acnh_save_test_pause_state(void);
void acnh_save_test_pause_release(void);
typedef struct { const char *path; unsigned result; } RaceCreate;
typedef struct { const char *name; _Atomic int done; } RaceUnmount;
static void *probe_race_create(void *opaque) {
    RaceCreate *task = opaque;
    unsigned (*create)(const char *, int64_t) = runtime_fs_host_override("_ZN2nn2fs10CreateFileEPKcl");
    task->result = create(task->path, 1);
    return NULL;
}
static void *probe_race_unmount(void *opaque) {
    RaceUnmount *task = opaque;
    void (*unmount)(const char *) = runtime_fs_host_override("_ZN2nn2fs7UnmountEPKc");
    unmount(task->name);
    atomic_store(&task->done, 1);
    return NULL;
}
static void probe_wait_classified(void) {
    for (int i = 0; i < 3000 && acnh_save_test_pause_state() != 2; i++) usleep(1000);
    assert(acnh_save_test_pause_state() == 2);
}
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
static void active_path_for(char *out, size_t capacity, const char *suffix) {
    char meta_path[1200]; path_for(meta_path, sizeof meta_path, "device-save/01006f8002326000.meta");
    FILE *file = fopen(meta_path, "rb"); assert(file);
    DeviceSaveMetadata metadata; assert(fread(&metadata, 1, sizeof metadata, file) == sizeof metadata);
    fclose(file);
    char relative[1200];
    int n = snprintf(relative, sizeof relative, "device-save/%s%s%s",
        metadata.active_slot ? ".01006f8002326000.slot1" : "01006f8002326000",
        *suffix ? "/" : "", suffix);
    assert(n >= 0 && (size_t)n < sizeof relative);
    path_for(out, capacity, relative);
}

static void probe_put(const char *path, const char data[3]) {
    unsigned (*create)(const char *, int64_t) = runtime_fs_host_override("_ZN2nn2fs10CreateFileEPKcl");
    unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    unsigned (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) =
        runtime_fs_host_override("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    uint64_t h = 0; assert(create(path, 3) == 0);
    assert(open_file(&h, path, 2) == 0);
    assert(write_file(h, 0, data, 3, NULL) == 0);
    close_file(h);
}
static void probe_overwrite(const char *path, const char data[3]) {
    unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    unsigned (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) =
        runtime_fs_host_override("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    uint64_t h = 0; assert(open_file(&h, path, 2) == 0);
    assert(write_file(h, 0, data, 3, NULL) == 0);
    close_file(h);
}
static void probe_transaction_changes(void) {
    unsigned (*create_dir)(const char *) = runtime_fs_host_override("_ZN2nn2fs15CreateDirectoryEPKc");
    unsigned (*delete_file)(const char *) = runtime_fs_host_override("_ZN2nn2fs10DeleteFileEPKc");
    unsigned (*rename_file)(const char *, const char *) = runtime_fs_host_override("_ZN2nn2fs10RenameFileEPKcS2_");
    probe_overwrite("device:/town/state.bin", "new");
    assert(delete_file("device:/town/remove.bin") == 0);
    assert(rename_file("device:/town/rename-src.bin", "device:/town/rename-dst.bin") == 0);
    assert(create_dir("device:/town/newdir") == 0);
    probe_put("device:/town/newdir/added.bin", "add");
}
static void probe_check_file(const char *path, const char data[3]) {
    unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    unsigned (*read_file)(uint64_t *, uint64_t, int64_t, void *, uint64_t) = runtime_fs_host_override("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    uint64_t h = 0, count = 0; char actual[3];
    assert(open_file(&h, path, 1) == 0);
    assert(read_file(&count, h, 0, actual, sizeof actual) == 0 && count == 3);
    assert(!memcmp(actual, data, 3)); close_file(h);
}
#endif

int main(int argc, char **argv) {
    MountDeviceSaveDataFn mount_device = (MountDeviceSaveDataFn)runtime_fs_host_override("_ZN2nn2fs19MountDeviceSaveDataEPKc");
    GetDeviceSaveDataSizeFn get_size = (GetDeviceSaveDataSizeFn)runtime_fs_host_override("_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_");
    CreateFileFn create_file = (CreateFileFn)runtime_fs_host_override("_ZN2nn2fs10CreateFileEPKcl");
    DeleteFileFn delete_file = (DeleteFileFn)runtime_fs_host_override("_ZN2nn2fs10DeleteFileEPKc");
    CommitFn commit = (CommitFn)runtime_fs_host_override("_ZN2nn2fs6CommitEPKc");
    RestoreFlagFn restore_flag = (RestoreFlagFn)runtime_fs_host_override("_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc");
    CommitMultipleFn commit_multiple = (CommitMultipleFn)runtime_fs_host_override("_ZN2nn2fs6CommitEPKPKci");
    CommitOptionsFn commit_options = (CommitOptionsFn)runtime_fs_host_override("_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE");
    ExtendDeviceSaveDataFn extend = (ExtendDeviceSaveDataFn)runtime_fs_host_override("_ZN2nn2fs20ExtendDeviceSaveDataEll");
#ifdef GAME_ACNH
    assert(mount_device && get_size && create_file && commit && restore_flag && commit_multiple && commit_options && extend);
    const char *mode = argc > 1 ? argv[1] : "first";
    if (!strcmp(mode, "transaction-red")) {
        assert(mount_device("device") == 0);
        assert(create_file("device:/old.bin", 1) == 0);
        assert(commit("device") == 0);
        char meta_path[1200]; path_for(meta_path, sizeof meta_path, "device-save/01006f8002326000.meta");
        FILE *meta_file = fopen(meta_path, "rb"); assert(meta_file);
        unsigned char meta_bytes[56] = {0}; size_t meta_length = fread(meta_bytes, 1, sizeof meta_bytes, meta_file);
        fclose(meta_file);
        unsigned active = meta_length == 56 ? meta_bytes[40] : 0;
        assert(active < 2);
        const char *slot = active ? "device-save/.01006f8002326000.slot1" : "device-save/01006f8002326000";
        char old_path[1200], new_path[1200], suffix[1200];
        snprintf(suffix, sizeof suffix, "%s/old.bin", slot); path_for(old_path, sizeof old_path, suffix);
        snprintf(suffix, sizeof suffix, "%s/new.bin", slot); path_for(new_path, sizeof new_path, suffix);
        assert(create_file("device:/new.bin", 1) == 0);
        assert(access(old_path, F_OK) == 0);
        assert(access(new_path, F_OK) != 0);
        assert(delete_file("device:/old.bin") == 0);
        assert(access(old_path, F_OK) == 0);
        return 0;
    }
    if (!strcmp(mode, "txn-seed")) {
        unsigned (*create_dir)(const char *) = runtime_fs_host_override("_ZN2nn2fs15CreateDirectoryEPKc");
        assert(mount_device("device") == 0);
        assert(create_dir("device:/town") == 0);
        probe_put("device:/town/state.bin", "old");
        probe_put("device:/town/remove.bin", "rem");
        probe_put("device:/town/rename-src.bin", "src");
        assert(commit("device") == 0);
        return 0;
    }
    if (!strcmp(mode, "first-save-sequence")) {
        unsigned (*create_dir)(const char *) = runtime_fs_host_override("_ZN2nn2fs15CreateDirectoryEPKc");
        unsigned (*clean)(const char *) = runtime_fs_host_override("_ZN2nn2fs25CleanDirectoryRecursivelyEPKc");
        unsigned (*rename_dir)(const char *, const char *) = runtime_fs_host_override("_ZN2nn2fs15RenameDirectoryEPKcS2_");
        unsigned (*count_entries)(int64_t *, uint64_t) = runtime_fs_host_override("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE");
        unsigned (*open_dir)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
        unsigned (*read_dir)(int64_t *, void *, uint64_t, int64_t) = runtime_fs_host_override("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
        void (*close_dir)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
        unsigned (*entry_type)(int *, const char *) = runtime_fs_host_override("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc");
        void (*unmount)(const char *) = runtime_fs_host_override("_ZN2nn2fs7UnmountEPKc");
        assert(clean && rename_dir && count_entries);
        assert(mount_device("device") == 0 && create_dir("device:/old") == 0);
        probe_put("device:/old/a.bin", "old");
        assert(commit("device") == 0);
        /* 游戏每次提交后都会卸载，下次操作前重新挂载。 */
        unmount("device"); assert(mount_device("device") == 0);
        assert(clean("missing:/") == 0x202u);
        assert(clean("device:/") == 0 && runtime_fs_save_busy());
        char old_payload[1200]; active_path_for(old_payload, sizeof old_payload, "old/a.bin");
        assert(access(old_payload, F_OK) == 0);
        int type = -1; assert(entry_type(&type, "device:/old") == 0x202u);
        assert(create_dir("device:/Villager0") == 0);
        probe_put("device:/Villager0/personal.dat", "per");
        assert(create_dir("device:/tmp") == 0);
        probe_put("device:/tmp/x.bin", "tmp");
        assert(rename_dir("device:/tmp", "device:/Moved") == 0);
        assert(rename_dir("device:/Villager0/personal.dat", "device:/y") == 0x202u);
        assert(rename_dir("device:/Moved", "device:/Villager0") == 0x402u);
        assert(rename_dir("device:/nothing", "device:/z") == 0x202u);
        uint64_t root = 0, villager = 0; int64_t n = -1, got = 0; unsigned char entry[0x310];
        assert(open_dir(&root, "device:/", 3) == 0 && count_entries(&n, root) == 0 && n == 2);
        assert(read_dir(&got, entry, root, 1) == 0 && got == 1);
        assert(count_entries(&n, root) == 0 && n == 2);
        close_dir(root);
        assert(open_dir(&root, "device:/", 2) == 0 && count_entries(&n, root) == 0 && n == 0);
        close_dir(root);
        assert(open_dir(&villager, "device:/Villager0", 2) == 0 && count_entries(&n, villager) == 0 && n == 1);
        close_dir(villager);
        assert(count_entries(&n, 0x3000000ffULL) == 0x2ee202u && count_entries(NULL, villager) == 0x2ee202u);
        uint32_t clear_restore = 1;
        assert(commit_options("device", &clear_restore) == 0 && !runtime_fs_save_busy());
        assert(access(old_payload, F_OK) == 0);
        unmount("device"); assert(mount_device("device") == 0);
        probe_check_file("device:/Villager0/personal.dat", "per");
        probe_check_file("device:/Moved/x.bin", "tmp");
        assert(entry_type(&type, "device:/old") == 0x202u && entry_type(&type, "device:/tmp") == 0x202u);
        char new_old[1200]; active_path_for(new_old, sizeof new_old, "old");
        assert(access(new_old, F_OK) != 0);
        return 0;
    }
    if (!strcmp(mode, "metadata-init")) {
        int64_t normal = 0, journal = 0;
        assert(get_size(&normal, &journal) == 0);
        assert(normal == 0x06f00000LL && journal == 0x06f00000LL);
        return 0;
    }
    if (!strcmp(mode, "txn-stage-exit") || !strcmp(mode, "txn-commit") ||
        !strcmp(mode, "txn-commit-fail") || !strcmp(mode, "txn-stage-fail") ||
        !strcmp(mode, "txn-finalize-retry") || !strcmp(mode, "txn-finalize-exit") ||
        !strcmp(mode, "txn-finalize-remount")) {
        assert(mount_device("device") == 0);
        if (!strcmp(mode, "txn-stage-fail")) {
            unsigned (*create_dir)(const char *) = runtime_fs_host_override("_ZN2nn2fs15CreateDirectoryEPKc");
            assert(create_dir("device:/town/newdir") != 0);
            return 0;
        }
        probe_transaction_changes();
        if (!strcmp(mode, "txn-stage-exit")) return 0;
        unsigned result = commit("device");
        if (!strcmp(mode, "txn-finalize-retry") || !strcmp(mode, "txn-finalize-exit") ||
            !strcmp(mode, "txn-finalize-remount")) {
            assert(result != 0 && runtime_fs_save_busy());
            assert(create_file("device:/town/must-not-exist.bin", 1) == 0x2ee202u);
            if (!strcmp(mode, "txn-finalize-exit")) return 0;
            if (!strcmp(mode, "txn-finalize-remount")) {
                void (*unmount)(const char *) = runtime_fs_host_override("_ZN2nn2fs7UnmountEPKc");
                unmount("device");
            }
            assert(unsetenv("ACNH_SAVE_FAILPOINT") == 0);
            if (!strcmp(mode, "txn-finalize-remount")) assert(mount_device("device") == 0);
            assert(commit("device") == 0 && !runtime_fs_save_busy());
            char marker_path[1200]; path_for(marker_path, sizeof marker_path, "device-save/.01006f8002326000.txn");
            assert(access(marker_path, F_OK) != 0);
            if (!strcmp(mode, "txn-finalize-remount")) {
                probe_put("device:/town/after-remount.bin", "yes");
                assert(commit("device") == 0);
            }
        } else if (!strcmp(mode, "txn-commit-fail")) assert(result != 0 && runtime_fs_save_busy());
        else assert(result == 0 && !runtime_fs_save_busy());
        return 0;
    }
    if (!strcmp(mode, "txn-write-sync-fail")) {
        assert(mount_device("device") == 0);
        unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
        unsigned (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) =
            runtime_fs_host_override("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
        void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
        uint64_t h = 0; int32_t option = 1;
        assert(open_file(&h, "device:/town/state.bin", 2) == 0);
        assert(write_file(h, 0, "new", 3, &option) == 0x2ee202u);
        close_file(h);
        assert(runtime_fs_save_busy());
        return 0;
    }
    if (!strcmp(mode, "txn-clear-restore")) {
        assert(mount_device("device") == 0);
        bool restored = false; assert(restore_flag(&restored, "device") == 0 && restored);
        uint32_t clear_restore = 1;
        assert(commit_options("device", &clear_restore) == 0 && !runtime_fs_save_busy());
        assert(restore_flag(&restored, "device") == 0 && !restored);
        return 0;
    }
    if (!strcmp(mode, "txn-check")) {
        assert(argc >= 4 && mount_device("device") == 0);
        bool restored = false; assert(restore_flag(&restored, "device") == 0);
        assert(restored == (argv[3][0] == '1'));
        unsigned (*entry_type)(int *, const char *) = runtime_fs_host_override("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc");
        int type = -1;
        if (!strcmp(argv[2], "old")) {
            probe_check_file("device:/town/state.bin", "old");
            probe_check_file("device:/town/rename-src.bin", "src");
            probe_check_file("device:/town/remove.bin", "rem");
            assert(entry_type(&type, "device:/town/newdir") == 0x202u);
            assert(entry_type(&type, "device:/town/rename-dst.bin") == 0x202u);
        } else {
            probe_check_file("device:/town/state.bin", "new");
            probe_check_file("device:/town/rename-dst.bin", "src");
            probe_check_file("device:/town/newdir/added.bin", "add");
            assert(entry_type(&type, "device:/town/remove.bin") == 0x202u);
            assert(entry_type(&type, "device:/town/rename-src.bin") == 0x202u);
        }
        return 0;
    }
    if (!strcmp(mode, "txn-round")) {
        assert(argc >= 3 && strlen(argv[2]) == 3 && mount_device("device") == 0);
        probe_overwrite("device:/town/state.bin", argv[2]);
        assert(commit("device") == 0);
        return 0;
    }
    if (!strcmp(mode, "txn-two-mount")) {
        assert(mount_device("device") == 0 && mount_device("other") == 0);
        unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
        unsigned (*read_file)(uint64_t *, uint64_t, int64_t, void *, uint64_t) = runtime_fs_host_override("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
        unsigned (*file_size)(int64_t *, uint64_t) = runtime_fs_host_override("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
        void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
        unsigned (*open_dir)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
        unsigned (*read_dir)(int64_t *, void *, uint64_t, int64_t) = runtime_fs_host_override("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
        void (*close_dir)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
        uint64_t old_fd = 0, old_dir = 0, count = 0; int64_t size = 0, dir_count = 0;
        char buffer[3]; unsigned char entries[0x310];
        assert(open_file(&old_fd, "other:/town/state.bin", 1) == 0);
        assert(open_dir(&old_dir, "other:/town", 3) == 0);
        probe_overwrite("device:/town/state.bin", "new");
        assert(extend(0x07000000LL, 0x07000000LL) == 0x2ee202u);
        assert(read_file(&count, old_fd, 0, buffer, sizeof buffer) == 0x2ee202u);
        assert(file_size(&size, old_fd) == 0x2ee202u);
        assert(read_dir(&dir_count, entries, old_dir, 1) == 0x2ee202u);
        close_file(old_fd); close_dir(old_dir);
        uint64_t alias_fd = 0; assert(open_file(&alias_fd, "other:/town/state.bin", 1) == 0);
        assert(read_file(&count, alias_fd, 0, buffer, 3) == 0 && count == 3 && !memcmp(buffer, "new", 3));
        close_file(alias_fd);
        const char *names[] = {"device", "other"};
        assert(commit_multiple(names, 2) == 0x2ee202u && runtime_fs_save_busy());
        uint32_t option = 3; assert(commit_options("other", &option) == 0x2ee202u && runtime_fs_save_busy());
        option = 2; assert(commit_options("device", &option) == 0x2ee202u && runtime_fs_save_busy());
        assert(open_file(&alias_fd, "other:/town/state.bin", 2) == 0);
        assert(commit("device") == 0x2ee202u && runtime_fs_save_busy());
        assert(extend(0x07000000LL, 0x07000000LL) == 0x2ee202u);
        close_file(alias_fd);
        assert(commit("other") == 0 && !runtime_fs_save_busy());
        return 0;
    }
    if (!strcmp(mode, "txn-isolation")) {
        uint64_t uid[2]; runtime_account_uid(uid);
        unsigned (*mount_user)(const char *, const uint64_t *) =
            runtime_fs_host_override("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
        unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
        void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
        assert(mount_device("device") == 0 && mount_user("user", uid) == 0);
        probe_put("user:/foreign.bin", "usr");
        uint64_t foreign_writer = 0; assert(open_file(&foreign_writer, "user:/foreign.bin", 2) == 0);
        probe_overwrite("device:/town/state.bin", "new");
        assert(commit("device") == 0 && runtime_fs_save_busy());
        close_file(foreign_writer);
        assert(runtime_fs_save_busy());
        assert(commit("user") == 0 && !runtime_fs_save_busy());
        return 0;
    }
    if (!strcmp(mode, "txn-class-race")) {
        uint64_t uid[2]; runtime_account_uid(uid);
        unsigned (*mount_user)(const char *, const uint64_t *) =
            runtime_fs_host_override("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
        void (*unmount)(const char *) = runtime_fs_host_override("_ZN2nn2fs7UnmountEPKc");
        assert(mount_device("device") == 0 && mount_user("user", uid) == 0);
        assert(create_file("late:/town/unmounted.bin", 1) == 0x202u);
        RaceCreate task = { .path = "device:/town/race-device.bin" }; pthread_t thread, unmount_thread;
        acnh_save_test_pause_arm(); assert(pthread_create(&thread, NULL, probe_race_create, &task) == 0);
        probe_wait_classified();
        RaceUnmount removing = { .name = "device" };
        assert(pthread_create(&unmount_thread, NULL, probe_race_unmount, &removing) == 0);
        usleep(10000); assert(!atomic_load(&removing.done));
        acnh_save_test_pause_release(); assert(pthread_join(thread, NULL) == 0 && task.result == 0);
        assert(pthread_join(unmount_thread, NULL) == 0 && atomic_load(&removing.done));
        assert(mount_user("device", uid) == 0); /* 闲置名称可改作其他类型。 */
        unmount("device");
        assert(mount_device("device") == 0);
        assert(commit("device") == 0);

        char meta_path[1200]; path_for(meta_path, sizeof meta_path, "device-save/01006f8002326000.meta");
        DeviceSaveMetadata before, after; FILE *file = fopen(meta_path, "rb"); assert(file);
        assert(fread(&before, 1, sizeof before, file) == sizeof before); fclose(file);
        task = (RaceCreate){ .path = "user:/race-user.bin" };
        acnh_save_test_pause_arm(); assert(pthread_create(&thread, NULL, probe_race_create, &task) == 0);
        probe_wait_classified();
        atomic_store(&removing.done, 0); removing.name = "user";
        assert(pthread_create(&unmount_thread, NULL, probe_race_unmount, &removing) == 0);
        usleep(10000); assert(!atomic_load(&removing.done));
        acnh_save_test_pause_release(); assert(pthread_join(thread, NULL) == 0 && task.result == 0);
        assert(pthread_join(unmount_thread, NULL) == 0 && atomic_load(&removing.done));
        assert(mount_device("user") == 0);
        unmount("user");
        assert(mount_user("user", uid) == 0);
        file = fopen(meta_path, "rb"); assert(file);
        assert(fread(&after, 1, sizeof after, file) == sizeof after); fclose(file);
        assert(!memcmp(&before, &after, sizeof before));
        char marker_path[1200]; path_for(marker_path, sizeof marker_path, "device-save/.01006f8002326000.txn");
        assert(access(marker_path, F_OK) != 0);
        return 0;
    }
    if (!strcmp(mode, "txn-mount-reuse")) {
        uint64_t uid[2]; runtime_account_uid(uid);
        unsigned (*mount_user)(const char *, const uint64_t *) =
            runtime_fs_host_override("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
        void (*unmount)(const char *) = runtime_fs_host_override("_ZN2nn2fs7UnmountEPKc");
        for (int i = 0; i < 160; i++) {
            char name[32]; snprintf(name, sizeof name, "test-%d", i);
            assert(mount_device(name) == 0);
            unmount(name);
        }
        assert(mount_user("test-159", uid) == 0);
        unmount("test-159");
        assert(mount_device("test-159") == 0);
        return 0;
    }
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
        uint64_t handle = 0, read_count = 0; unsigned char data[16];
        unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
        unsigned (*read_file)(uint64_t *, uint64_t, int64_t, void *, uint64_t) = runtime_fs_host_override("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
        void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
        assert(open_file(&handle, "device:/payload.bin", 1) == 0);
        assert(read_file(&read_count, handle, 0, data, sizeof data) == 0 && read_count == sizeof data);
        close_file(handle);
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
        active_path_for(root, sizeof root, "");
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
    if (!strcmp(mode, "save-ops")) {
        bool restored = true;
        assert(restore_flag(NULL, "device") == 0x2ee202u);
        assert(restore_flag(&restored, "device") == 0x202u && restored == true);
        assert(mount_device("device") == 0);
        assert(restore_flag(&restored, "device") == 0 && restored == false);
        assert(create_file("device:/payload.bin", 16) == 0 && runtime_fs_save_busy());
        const char *names[] = {"device"};
        assert(commit_multiple(NULL, 1) == 0x2ee202u);
        assert(commit_multiple(names, -1) == 0x2ee202u);
        assert(commit_multiple(names, 1) == 0 && !runtime_fs_save_busy());
        assert(create_file("device:/second.bin", 4) == 0 && runtime_fs_save_busy());
        unsigned char option[8] = {0};
        assert(commit_options("device", NULL) == 0x2ee202u);
        assert(commit_options(NULL, option) == 0x2ee202u);
        option[0] = 2; assert(commit_options("device", option) == 0x2ee202u && runtime_fs_save_busy());
        option[0] = 1; assert(commit_options("device", option) == 0 && !runtime_fs_save_busy());
        assert(create_file("device:/third.bin", 4) == 0 && runtime_fs_save_busy());
        option[0] = 0;
        assert(commit_options("device", option) == 0 && !runtime_fs_save_busy());
        int64_t normal = 0, journal = 0;
        assert(extend(0x04000000LL, 0x04000000LL) == 0x2ee202u);
        assert(get_size(&normal, &journal) == 0 && normal == 0x06f00000LL && journal == 0x06f00000LL);
        assert(extend(0x07000000LL, 0x07000000LL) == 0);
        assert(get_size(&normal, &journal) == 0 && normal == 0x07000000LL && journal == 0x07000000LL);
        assert(extend(0x06f00000LL, 0x07000000LL) == 0x2ee202u);
        assert(get_size(&normal, &journal) == 0 && normal == 0x07000000LL && journal == 0x07000000LL);
        assert(extend(0x24000001LL, 0x07000000LL) == 0x2ee202u);
        assert(extend(0x24000000LL, 0x24000000LL) == 0);
        assert(get_size(&normal, &journal) == 0 && normal == 0x24000000LL && journal == 0x24000000LL);
        return 0;
    }
    assert(!"unknown probe mode");
#else
    (void)argc;
    (void)argv;
    (void)delete_file;
    assert(!mount_device && !get_size && create_file && commit && !restore_flag && !commit_multiple && !commit_options && !extend);
    assert(!runtime_fs_host_override("_ZN2nn2fs25CleanDirectoryRecursivelyEPKc") &&
           !runtime_fs_host_override("_ZN2nn2fs15RenameDirectoryEPKcS2_") &&
           !runtime_fs_host_override("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE"));
#endif
    return 0;
}
