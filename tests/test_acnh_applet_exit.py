from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/applet_exit_probe.c"
APPLET = ROOT / "third_party/macnativegames/games/totk/runtime/applet_host.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhAppletExitTests(unittest.TestCase):
    def test_exit_sections_notifications_and_inactivity(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "applet-exit-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(APPLET), f"-I{RUNTIME}", "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)


if __name__ == "__main__":
    unittest.main()
