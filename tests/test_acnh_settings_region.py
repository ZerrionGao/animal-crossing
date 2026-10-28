from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/settings_service_ipc_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class SettingsRegionTests(unittest.TestCase):
    def compile_probe(self, directory, game_define):
        binary = Path(directory) / f"settings-{game_define.lower()}"
        result = subprocess.run(
            [
                "xcrun", "clang", f"-D{game_define}", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "settings_service.c"),
                f"-I{RUNTIME}", "-pthread", "-o", str(binary),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_acnh_get_region_code_returns_usa1_through_production_dispatch(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(
                "[acnh-settings] request service=set cmd=4 type=4 domain=0 object=0 token=0 data=8 "
                "buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
                result.stderr,
            )

    def test_acnh_accepts_legal_domain_dispatch_without_input(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "domain"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("service=set cmd=4 type=4 domain=1 object=1 token=0 data=0", result.stderr)

    def test_acnh_accepts_request_with_context_framing(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "request-context"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("cmd=4 type=6 domain=0", result.stderr)

    def test_acnh_get_language_code_returns_zh_hans(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "language"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(
                "[acnh-settings] request service=set cmd=0 type=6 domain=0 object=0 token=0 data=8 "
                "buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
                result.stderr,
            )

    def test_acnh_other_commands_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "unsupported"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("ACNH nn::settings command is not implemented", result.stderr)

    def test_acnh_get_region_code_rejects_input_arguments(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "bad-shape"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("GetRegionCode request shape is not supported", result.stderr)

    def test_botw_settings_commands_remain_available(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_BOTW")
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
