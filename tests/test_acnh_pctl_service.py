from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/pctl_ipc_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhPctlServiceTests(unittest.TestCase):
    def compile_probe(self, directory):
        binary = Path(directory) / "pctl-acnh"
        result = subprocess.run(
            [
                "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "pctl_offline.c"),
                f"-I{RUNTIME}", "-pthread", "-o", str(binary),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_observed_factory_shape_then_communication_query_returns_success(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("opened service pctl", result.stderr)
            self.assertIn(
                "[acnh-pctl] factory cmd=1 type=6 domain=0 object=0 token=0 data=8 "
                "buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
                result.stderr,
            )
            self.assertIn(
                "[acnh-pctl] service cmd=1 type=6 domain=0 object=0 token=0 data=0 "
                "buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
                result.stderr,
            )
            self.assertIn(
                "[acnh-pctl] service cmd=1018 type=6 domain=0 object=0 token=0 data=0 "
                "buffers=x0/a0/b0/w0/c0 handles=pid:0,copy:0,move:0",
                result.stderr,
            )
            self.assertIn("[acnh-pctl] service cmd=1001 ", result.stderr)
            self.assertNotIn("local parental-control policy is unavailable", result.stderr)

    def test_unknown_or_malformed_service_request_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            for mode in ("service-unknown", "service-bad-shape"):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 42, result.stderr)
                    self.assertIn("IPC command not implemented", result.stderr)

    def test_unknown_factory_request_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary), "factory-unknown"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("IPC command not implemented", result.stderr)

    def test_sdk_parental_control_ports_are_registered(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary), "special"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('opened service pctl:s', result.stderr)

    def test_service_is_not_registered_without_game_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            object_file = Path(temp) / "pctl-non-acnh.o"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
                    str(RUNTIME / "pctl_offline.c"), f"-I{RUNTIME}", "-o", str(object_file),
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
