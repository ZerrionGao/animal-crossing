from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/device_save_probe.c"
FS_HOST = ROOT / "third_party/macnativegames/games/totk/runtime/fs_host.c"
APP = "01006f8002326000"
META_SIZE = 56
META_MAGIC = 0x41434E4844534156


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhDeviceSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compile_dir = tempfile.TemporaryDirectory()
        cls.acnh_binary = Path(cls.compile_dir.name) / "device-save-acnh"
        cls.other_binary = Path(cls.compile_dir.name) / "device-save-other"
        for flags, binary in ((["-DGAME_ACNH", "-DACNH_SAVE_TESTING"], cls.acnh_binary), ([], cls.other_binary)):
            result = subprocess.run(
                ["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                 str(PROBE), str(FS_HOST), "-o", str(binary)],
                capture_output=True, text=True,
            )
            if result.returncode:
                raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.compile_dir.cleanup()

    def run_probe(self, binary, profile, mode="first", expected=0, uid=None, extra_env=None, args=()):
        env = os.environ.copy()
        env["TOTK_PROFILE_DIR"] = str(profile)
        if uid is not None:
            env["TEST_UID"] = str(uid)
        if extra_env:
            env.update(extra_env)
        result = subprocess.run([str(binary), mode, *args], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    @staticmethod
    def metadata(profile):
        return profile / "device-save" / f"{APP}.meta"

    @classmethod
    def active_payload(cls, profile):
        slot = struct.unpack_from("<I", cls.metadata(profile).read_bytes(), 40)[0]
        return profile / "device-save" / (APP if slot == 0 else f".{APP}.slot1")

    @classmethod
    def tree_snapshot(cls, profile):
        root = cls.active_payload(profile)
        return {
            str(path.relative_to(root)): None if path.is_dir() else path.read_bytes()
            for path in root.rglob("*")
        }

    def test_first_create_mount_write_commit_and_reopen_independent_of_uid(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "first", uid=1)
            root = self.active_payload(profile)
            meta = self.metadata(profile)
            self.assertTrue(root.is_dir())
            self.assertEqual((root / "payload.bin").read_bytes(), bytes(16))
            self.assertTrue(meta.is_file())
            self.assertEqual(meta.stat().st_size, META_SIZE)
            self.run_probe(self.acnh_binary, profile, "reopen", uid=2)

    def test_first_save_sequence_cleans_root_renames_directories_and_counts_entries(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "first-save-sequence")
            self.assertEqual(
                self.tree_snapshot(profile),
                {"Villager0": None, "Villager0/personal.dat": b"per", "Moved": None, "Moved/x.bin": b"tmp"},
            )
            self.assertFalse((profile / "device-save" / f".{APP}.txn").exists())

    def test_uncommitted_changes_leave_active_payload_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_probe(self.acnh_binary, Path(tmp).resolve() / "profile", "transaction-red")

    def test_metadata_corruption_capacity_bounds_and_missing_payload_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "first")
            meta = self.metadata(profile)
            data = bytearray(meta.read_bytes())
            data[-1] ^= 0x80
            meta.write_bytes(data)
            self.run_probe(self.acnh_binary, profile, "bad-meta", expected=42)

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "first")
            meta = self.metadata(profile)
            data = bytearray(meta.read_bytes())
            fields = list(struct.unpack("<QIIQqqIIQ", data))
            fields[5] = 0x24000001
            prefix = struct.pack("<QIIQqqII", *fields[:-1])
            checksum = 1469598103934665603
            for byte in prefix:
                checksum = ((checksum ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            meta.write_bytes(prefix + struct.pack("<Q", checksum))
            self.run_probe(self.acnh_binary, profile, "bad-size", expected=42)

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            (profile / "device-save" / APP).mkdir(parents=True)
            (profile / "device-save" / APP / "keep.bin").write_bytes(b"preserve")
            self.run_probe(self.acnh_binary, profile, "missing-meta-with-file", expected=42)
            self.assertEqual((profile / "device-save" / APP / "keep.bin").read_bytes(), b"preserve")

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "first")
            root = profile / "device-save" / APP
            shutil.rmtree(root)
            self.run_probe(self.acnh_binary, profile, "missing-root")
            self.assertFalse(root.exists())

    def test_symlink_preflight_prevents_ready_from_deleting_external_temp(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp).resolve()
            profile = base / "profile"
            outside = base / "outside"
            for path in (profile / "save" / "88776655443322111122334455667788",
                         profile / "cache", profile / "device-save", outside):
                path.mkdir(parents=True, exist_ok=True)
            sentinel = outside / "sentinel"
            sentinel.write_text("keep")
            (profile / "temp").symlink_to(outside, target_is_directory=True)
            self.run_probe(self.acnh_binary, profile, "preflight", uid=0x1122334455667788)
            self.assertEqual(sentinel.read_text(), "keep")
            self.assertTrue((profile / "temp").is_symlink())

    def test_invalid_mount_and_null_size_do_not_create_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            for mode in ("invalid-name", "size-null"):
                profile = Path(tmp).resolve() / mode
                self.run_probe(self.acnh_binary, profile, mode)
                self.assertFalse(profile.exists())

    def test_device_payload_operations_reject_symlinked_parent_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp).resolve()
            profile = base / "profile"
            outside = base / "outside"
            self.run_probe(self.acnh_binary, profile, "first")
            outside.mkdir()
            sentinel = outside / "sentinel"
            sentinel.write_text("keep")
            (self.active_payload(profile) / "linked-dir").symlink_to(outside, target_is_directory=True)
            self.run_probe(self.acnh_binary, profile, "symlink-payload")
            self.assertEqual(sentinel.read_text(), "keep")
            self.assertFalse((outside / "file.bin").exists())

    def test_device_mount_root_replacement_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp).resolve()
            profile = base / "profile"
            outside = base / "outside"
            self.run_probe(self.acnh_binary, profile, "first")
            outside.mkdir()
            sentinel = outside / "sentinel"
            sentinel.write_text("keep")
            self.run_probe(self.acnh_binary, profile, "symlink-root-payload",
                           extra_env={"DEVICE_SAVE_OUTSIDE": str(outside)})
            self.assertEqual(sentinel.read_text(), "keep")
            self.assertFalse((outside / "root-escape.bin").exists())

    def test_device_mount_ancestor_replacement_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp).resolve()
            profile = base / "profile"
            outside = base / "outside"
            self.run_probe(self.acnh_binary, profile, "first")
            outside_app = outside / APP
            outside_app.mkdir(parents=True)
            sentinel = outside_app / "sentinel"
            sentinel.write_text("keep")
            self.run_probe(self.acnh_binary, profile, "symlink-base-payload",
                           extra_env={"DEVICE_SAVE_OUTSIDE": str(outside)})
            self.assertEqual(sentinel.read_text(), "keep")
            self.assertFalse((outside_app / "base-escape.bin").exists())

    def test_save_commit_restore_and_extend(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "save-ops")
            normal, journal = struct.unpack_from("<qq", self.metadata(profile).read_bytes(), 24)
            self.assertEqual(normal, 0x24000000)
            self.assertEqual(journal, 0x24000000)
            self.run_probe(self.other_binary, profile, "first")

    def test_transaction_recovery_and_slot_rotation(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            original_slot = struct.unpack_from("<I", self.metadata(profile).read_bytes(), 40)[0]
            self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), b"old")
            self.run_probe(self.acnh_binary, profile, "txn-stage-exit")
            self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), b"old")
            self.run_probe(self.acnh_binary, profile, "txn-check", args=("old", "1"))
            self.run_probe(self.acnh_binary, profile, "txn-check", args=("old", "1"))
            self.run_probe(self.acnh_binary, profile, "txn-commit")
            self.run_probe(self.acnh_binary, profile, "txn-check", args=("new", "0"))
            self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), b"new")
            for payload in ("one", "two", "tri", "end"):
                before = struct.unpack_from("<I", self.metadata(profile).read_bytes(), 40)[0]
                self.run_probe(self.acnh_binary, profile, "txn-round", args=(payload,))
                after = struct.unpack_from("<I", self.metadata(profile).read_bytes(), 40)[0]
                self.assertEqual(after, 1 - before)
                self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), payload.encode())
            self.assertNotEqual(original_slot, struct.unpack_from("<I", self.metadata(profile).read_bytes(), 40)[0])

    def test_duplicate_mounts_share_staging_and_reject_unsupported_commits(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            self.run_probe(self.acnh_binary, profile, "txn-two-mount")
            self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), b"new")

    def test_device_commit_does_not_consume_user_save_dirty_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            self.run_probe(self.acnh_binary, profile, "txn-isolation")
            self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), b"new")

    def test_mount_classification_race_cannot_bypass_staging(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            self.run_probe(self.acnh_binary, profile, "txn-class-race")
            self.assertEqual((self.active_payload(profile) / "town/race-device.bin").read_bytes(), b"\0")

    def test_sequential_mount_names_and_idle_cross_type_reuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-mount-reuse")

    def test_failure_points_and_crash_recovery(self):
        cases = (
            ("txn-stage-fail", "ACNH_SAVE_FAILPOINT", "clone-sync", "old", "0", 0),
            ("txn-stage-fail", "ACNH_SAVE_FAILPOINT", "copy-enospc", "old", "0", 0),
            ("txn-stage-fail", "ACNH_SAVE_FAILPOINT", "marker-sync", "old", "0", 0),
            ("txn-stage-fail", "ACNH_SAVE_FAILPOINT", "marker-rename", "old", "0", 0),
            ("txn-stage-fail", "ACNH_SAVE_FAILPOINT", "marker-enospc", "old", "0", 0),
            ("txn-stage-fail", "ACNH_SAVE_FAILPOINT", "marker-dir-sync", "old", "1", 0),
            ("txn-write-sync-fail", "ACNH_SAVE_FAILPOINT", "write-sync", "old", "1", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "commit-sync", "old", "1", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "commit-dir-sync", "old", "1", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "meta-sync", "old", "1", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "meta-rename", "old", "1", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "meta-dir-sync", "new", "0", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "marker-unlink", "new", "0", 0),
            ("txn-commit-fail", "ACNH_SAVE_FAILPOINT", "marker-cleanup-dir-sync", "new", "0", 0),
            ("txn-commit", "ACNH_SAVE_EXITPOINT", "after-marker-rename", "old", "1", 77),
            ("txn-stage-fail", "ACNH_SAVE_EXITPOINT", "after-clear-before-clone", "old", "0", 77),
            ("txn-stage-fail", "ACNH_SAVE_EXITPOINT", "after-clone-before-marker", "old", "0", 77),
            ("txn-commit", "ACNH_SAVE_EXITPOINT", "after-marker", "old", "1", 77),
            ("txn-commit", "ACNH_SAVE_EXITPOINT", "before-meta", "old", "1", 77),
            ("txn-commit", "ACNH_SAVE_EXITPOINT", "after-meta-rename", "new", "0", 77),
            ("txn-commit", "ACNH_SAVE_EXITPOINT", "after-meta", "new", "0", 77),
        )
        for mode, key, stage, expected_tree, restored, code in cases:
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as tmp:
                profile = Path(tmp).resolve() / "profile"
                self.run_probe(self.acnh_binary, profile, "txn-seed")
                self.run_probe(self.acnh_binary, profile, mode, expected=code, extra_env={key: stage})
                self.run_probe(self.acnh_binary, profile, "txn-check", args=(expected_tree, restored))
                expected = {"town": None, "town/state.bin": b"old", "town/remove.bin": b"rem",
                            "town/rename-src.bin": b"src"} if expected_tree == "old" else {
                                "town": None, "town/state.bin": b"new", "town/rename-dst.bin": b"src",
                                "town/newdir": None, "town/newdir/added.bin": b"add"}
                self.assertEqual(self.tree_snapshot(profile), expected)

    def test_clear_restore_option_without_staged_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            self.run_probe(self.acnh_binary, profile, "txn-commit", expected=77,
                           extra_env={"ACNH_SAVE_EXITPOINT": "after-marker"})
            self.run_probe(self.acnh_binary, profile, "txn-clear-restore")
            self.run_probe(self.acnh_binary, profile, "txn-check", args=("old", "0"))

    def test_post_publish_failure_freezes_writes_and_commit_can_finalize(self):
        for mode in ("txn-finalize-retry", "txn-finalize-exit", "txn-finalize-remount"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                profile = Path(tmp).resolve() / "profile"
                self.run_probe(self.acnh_binary, profile, "txn-seed")
                self.run_probe(self.acnh_binary, profile, mode,
                               extra_env={"ACNH_SAVE_FAILPOINT": "meta-dir-sync"})
                self.run_probe(self.acnh_binary, profile, "txn-check", args=("new", "0"))
                expected = {
                    "town": None, "town/state.bin": b"new", "town/rename-dst.bin": b"src",
                    "town/newdir": None, "town/newdir/added.bin": b"add"}
                if mode == "txn-finalize-remount":
                    expected["town/after-remount.bin"] = b"yes"
                self.assertEqual(self.tree_snapshot(profile), expected)

    def test_v1_metadata_migrates_without_losing_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            active = self.active_payload(profile)
            legacy = profile / "device-save" / APP
            if active != legacy:
                shutil.rmtree(legacy)
                shutil.copytree(active, legacy)
                shutil.rmtree(active)
            meta = self.metadata(profile)
            fields = struct.unpack("<QIIQqqIIQ", meta.read_bytes())
            prefix = struct.pack("<QIIQqq", fields[0], 1, 0, fields[3], fields[4], fields[5])
            checksum = 1469598103934665603
            for byte in prefix:
                checksum = ((checksum ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            meta.write_bytes(prefix + struct.pack("<Q", checksum))
            self.run_probe(self.acnh_binary, profile, "txn-check", args=("old", "0"))
            self.assertEqual(meta.stat().st_size, 56)
            self.assertTrue((profile / "device-save" / f".{APP}.slot1").is_dir())
            self.run_probe(self.acnh_binary, profile, "txn-commit")
            self.run_probe(self.acnh_binary, profile, "txn-check", args=("new", "0"))

    def test_bad_marker_and_symlinked_active_payload_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            self.run_probe(self.acnh_binary, profile, "txn-stage-exit")
            marker = profile / "device-save" / f".{APP}.txn"
            data = bytearray(marker.read_bytes())
            data[-1] ^= 0x40
            marker.write_bytes(data)
            self.run_probe(self.acnh_binary, profile, "bad-meta", expected=42)
            self.assertEqual(marker.read_bytes(), data)
            self.assertEqual((self.active_payload(profile) / "town/state.bin").read_bytes(), b"old")

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            outside = Path(tmp).resolve() / "marker-outside"
            outside.write_bytes(b"sentinel")
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            marker = profile / "device-save" / f".{APP}.txn"
            marker.symlink_to(outside)
            self.run_probe(self.acnh_binary, profile, "bad-meta", expected=42)
            self.assertEqual(outside.read_bytes(), b"sentinel")

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            outside = Path(tmp).resolve() / "outside"
            outside.mkdir()
            (outside / "sentinel").write_text("keep")
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            (self.active_payload(profile) / "town/linked").symlink_to(outside)
            self.run_probe(self.acnh_binary, profile, "txn-stage-fail")
            self.assertFalse((profile / "device-save" / f".{APP}.txn").exists())
            self.assertEqual((outside / "sentinel").read_text(), "keep")

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            outside = Path(tmp).resolve() / "outside"
            outside.mkdir()
            (outside / "sentinel").write_text("keep")
            self.run_probe(self.acnh_binary, profile, "txn-seed")
            inactive = profile / "device-save" / APP
            (inactive / "linked").symlink_to(outside)
            self.run_probe(self.acnh_binary, profile, "txn-stage-fail")
            self.assertTrue((inactive / "linked").is_symlink())
            self.assertFalse((profile / "device-save" / f".{APP}.txn").exists())
            self.assertEqual((outside / "sentinel").read_text(), "keep")

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "metadata-init")
            (profile / "device-save" / f".{APP}.slot1").rmdir()
            self.run_probe(self.acnh_binary, profile, "bad-meta", expected=42)

        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            outside = Path(tmp).resolve() / "slot-outside"
            outside.mkdir()
            (outside / "sentinel").write_text("keep")
            self.run_probe(self.acnh_binary, profile, "metadata-init")
            slot1 = profile / "device-save" / f".{APP}.slot1"
            slot1.rmdir()
            slot1.symlink_to(outside, target_is_directory=True)
            self.run_probe(self.acnh_binary, profile, "bad-meta", expected=42)
            self.assertEqual((outside / "sentinel").read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
