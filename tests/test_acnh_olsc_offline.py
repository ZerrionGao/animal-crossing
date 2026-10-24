from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/olsc_ipc_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhOlscOfflineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compile_dir = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.compile_dir.name) / "olsc-acnh"
        result = subprocess.run(
            [
                "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "olsc_offline.c"),
                f"-I{RUNTIME}", "-pthread", "-o", str(cls.binary),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.compile_dir.cleanup()

    def run_probe(self, mode, expected=0):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result.stderr

    def test_initialized_local_user_setting_returns_one_disabled_byte(self):
        stderr = self.run_probe("valid")
        self.assertIn("opened service olsc:u", stderr)
        self.assertIn(
            "[acnh-olsc] request service=olsc:u cmd=0 type=6 domain=1 object=1 token=0 data=8 "
            "buffers=x0/a0/b0/w0/c0 handles=pid:1,copy:0,move:0",
            stderr,
        )
        self.assertIn(
            "[acnh-olsc] request service=olsc:u cmd=13 type=6 domain=1 object=1 token=0 data=16 "
            "buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
            stderr,
        )
        self.assertNotIn("PRIVATE", stderr)

    def test_set_setting_is_kept_locally_and_read_back(self):
        stderr = self.run_probe("set-setting")
        self.assertIn("backup setting for the local user: disabled (kept in this process only)", stderr)
        self.assertIn("backup setting for the local user: enabled (kept in this process only)", stderr)
        self.assertNotIn("1122334455667788", stderr)

    def test_get_setting_requires_initialize_for_each_session(self):
        for mode in ("uninitialized", "reopened-session"):
            with self.subTest(mode=mode):
                stderr = self.run_probe(mode, expected=42)
                self.assertIn("GetSetting before Initialize", stderr)

    def test_unknown_uid_and_malformed_shapes_fail_without_exposing_arguments(self):
        expected = {
            "wrong-uid": "GetSetting UID is not the local user",
            "bad-init-argument": "Initialize argument is not supported",
            "bad-init-shape": "Initialize request shape is not supported",
            "get-setting-with-pid": "GetSetting request shape is not supported",
            "get-setting-with-buffer": "GetSetting request shape is not supported",
            "get-setting-with-handle": "GetSetting request shape is not supported",
            "set-setting-wrong-uid": "SetSettingEnabled UID is not the local user",
            "set-setting-bad-value": "SetSettingEnabled value is not a bool",
            "set-setting-uninitialized": "SetSettingEnabled before Initialize",
            "set-setting-bad-shape": "SetSettingEnabled request shape is not supported",
            "unknown-command": "command is not implemented",
        }
        for mode, message in expected.items():
            with self.subTest(mode=mode):
                stderr = self.run_probe(mode, expected=42)
                self.assertIn(message, stderr)
                self.assertNotIn("SENTINEL", stderr)

    def test_service_remains_acnh_only(self):
        with tempfile.TemporaryDirectory() as temp:
            object_file = Path(temp) / "olsc-non-acnh.o"
            result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
                    str(RUNTIME / "olsc_offline.c"), f"-I{RUNTIME}", "-o", str(object_file),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.run(["xcrun", "nm", "-u", str(object_file)], capture_output=True, text=True)
            self.assertEqual(symbols.returncode, 0, symbols.stderr)
            self.assertNotIn("runtime_ipc_register", symbols.stdout)


if __name__ == "__main__":
    unittest.main()
