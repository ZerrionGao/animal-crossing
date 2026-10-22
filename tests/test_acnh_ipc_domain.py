from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/ipc_domain_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhIpcDomainTests(unittest.TestCase):
    def test_domain_framing_lifecycle_and_non_domain_regressions(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "ipc-domain-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "nifm_offline.c"),
                    f"-I{RUNTIME}", "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)
            self.assertIn("offline (no interface)", run_result.stderr)


if __name__ == "__main__":
    unittest.main()
