from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/nifm_event_lifecycle_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"
NVN_INCLUDE = ROOT / "third_party/macnativegames/games/totk/third_party/d2r_gfx/gfx/nvn/include"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhNifmEventTests(unittest.TestCase):
    def test_production_event_handles_survive_and_reclaim_both_close_orders(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "nifm-event-lifecycle-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    f"-I{NVN_INCLUDE}", str(PROBE), str(RUNTIME / "threads.c"),
                    str(RUNTIME / "thread_enter.S"), "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)
            self.assertIn('"event_cycles":5000', run_result.stdout)


if __name__ == "__main__":
    unittest.main()
