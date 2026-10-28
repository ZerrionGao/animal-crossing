from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/bcat_ipc_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhBcatOfflineTests(unittest.TestCase):
    def compile_probe(self, directory, acnh=True):
        binary = Path(directory) / ("bcat-acnh" if acnh else "bcat-non-acnh")
        command = ["xcrun", "clang"]
        if acnh:
            command.append("-DGAME_ACNH")
        command.extend([
            "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "bcat_offline.c"),
            f"-I{RUNTIME}", "-pthread", "-o", str(binary),
        ])
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_delivery_cache_leaf_is_registered_and_returns_not_found(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary), "mount-result"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("local delivery cache unavailable; result=0x47a", result.stderr)
            self.assertEqual(result.stderr.count("MountDeliveryCacheStorage"), 1)
            self.assertNotIn("mounted", result.stderr.lower())

    def test_factory_routes_and_closes_child_object(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("opened service bcat:u", result.stderr)
            self.assertIn(
                "[acnh-bcat] request service=bcat:u cmd=0 type=6 domain=1 object=1 token=0 data=8 "
                "buffers=x0/a0/b0/w0/c0 handles=pid:1,copy:0,move:0",
                result.stderr,
            )
            self.assertNotIn("1122334455667788", result.stderr.lower())
            self.assertNotIn("9087766554433221", result.stderr.lower())

    def test_child_command_is_traced_then_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary), "child-fail"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("service=IBcatService cmd=0 type=6 domain=1 object=2 token=0 data=8", result.stderr)
            self.assertIn("ACNH bcat:u command is not implemented", result.stderr)
            self.assertNotIn("1122334455667788", result.stderr.lower())
            self.assertNotIn("9087766554433221", result.stderr.lower())

    def test_factory_rejects_wrong_request_shape(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary), "malformed-root"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("service-creator request shape is not supported", result.stderr)

    def test_factory_rejects_wrong_argument_length(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            result = subprocess.run([str(binary), "malformed-size"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("data=4", result.stderr)
            self.assertIn("service-creator request shape is not supported", result.stderr)

    def test_service_is_not_registered_without_game_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            object_file = Path(temp) / "bcat-non-acnh.o"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
                    str(RUNTIME / "bcat_offline.c"), f"-I{RUNTIME}", "-o", str(object_file),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            symbols = subprocess.run(["xcrun", "nm", "-u", str(object_file)], capture_output=True, text=True)
            self.assertEqual(symbols.returncode, 0, symbols.stderr)
            self.assertNotIn("runtime_ipc_register", symbols.stdout)
            exported = subprocess.run(["xcrun", "nm", "-g", str(object_file)], capture_output=True, text=True)
            self.assertEqual(exported.returncode, 0, exported.stderr)
            self.assertNotIn("runtime_bcat_override", exported.stdout)


if __name__ == "__main__":
    unittest.main()
