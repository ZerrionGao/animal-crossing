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
META_SIZE = 48
META_MAGIC = 0x41434E4844534156


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhDeviceSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compile_dir = tempfile.TemporaryDirectory()
        cls.acnh_binary = Path(cls.compile_dir.name) / "device-save-acnh"
        cls.other_binary = Path(cls.compile_dir.name) / "device-save-other"
        for flags, binary in ((["-DGAME_ACNH"], cls.acnh_binary), ([], cls.other_binary)):
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

    def run_probe(self, binary, profile, mode="first", expected=0, uid=None, extra_env=None):
        env = os.environ.copy()
        env["TOTK_PROFILE_DIR"] = str(profile)
        if uid is not None:
            env["TEST_UID"] = str(uid)
        if extra_env:
            env.update(extra_env)
        result = subprocess.run([str(binary), mode], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    @staticmethod
    def metadata(profile):
        return profile / "device-save" / f"{APP}.meta"

    def test_first_create_mount_write_commit_and_reopen_independent_of_uid(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.acnh_binary, profile, "first", uid=1)
            root = profile / "device-save" / APP
            meta = self.metadata(profile)
            self.assertTrue(root.is_dir())
            self.assertEqual((root / "payload.bin").read_bytes(), bytes(16))
            self.assertTrue(meta.is_file())
            self.assertEqual(meta.stat().st_size, META_SIZE)
            self.run_probe(self.acnh_binary, profile, "reopen", uid=2)

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
            fields = list(struct.unpack("<QIIQqqQ", data))
            fields[5] = 0x24000001
            prefix = struct.pack("<QIIQqq", *fields[:-1])
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
            (profile / "device-save" / APP / "linked-dir").symlink_to(outside, target_is_directory=True)
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

    def test_unsupported_operations_fail_and_non_acnh_exports_remain_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            for mode in ("restore-guard", "commit-multiple-guard", "commit-options-guard", "extend-guard"):
                result = self.run_probe(self.acnh_binary, profile, mode, expected=42)
                self.assertIn("not supported", result.stderr)
            self.run_probe(self.other_binary, profile, "first")


if __name__ == "__main__":
    unittest.main()
