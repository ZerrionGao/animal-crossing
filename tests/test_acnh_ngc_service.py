from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/ngc_ipc_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhNgcServiceTests(unittest.TestCase):
    def compile_probe(self, directory):
        binary = Path(directory) / "ngc-acnh"
        result = subprocess.run(
            [
                "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "ngc_service.c"),
                f"-I{RUNTIME}", "-pthread", "-o", str(binary),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_first_request_logs_only_shape_then_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("opened service ngc:u", result.stderr)
            self.assertIn(
                "[acnh-ngc] request service=ngc:u cmd=23 type=4 domain=0 object=0 token=0 "
                "data=40 buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
                result.stderr,
            )
            self.assertIn("ACNH ngc:u command is not implemented", result.stderr)
            self.assertNotIn("PRIVATE_PLAYER_TEXT_SENTINEL", result.stderr)

    def test_service_is_not_registered_without_game_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            object_file = Path(temp) / "ngc-non-acnh.o"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
                    str(RUNTIME / "ngc_service.c"), f"-I{RUNTIME}", "-o", str(object_file),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            symbols = subprocess.run(["xcrun", "nm", "-u", str(object_file)], capture_output=True, text=True)
            self.assertEqual(symbols.returncode, 0, symbols.stderr)
            self.assertNotIn("runtime_ipc_register", symbols.stdout)


if __name__ == "__main__":
    unittest.main()
