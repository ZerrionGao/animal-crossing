from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/nifm_offline_probe.c"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhNifmTests(unittest.TestCase):
    def test_registered_nifm_reports_only_offline_state(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "nifm-offline-probe"
            compile_result = subprocess.run(
                ["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(PROBE), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)
            self.assertIn("offline (no interface)", run_result.stderr)
            self.assertIn("false (offline)", run_result.stderr)


if __name__ == "__main__":
    unittest.main()
