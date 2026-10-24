import shutil
import subprocess
import tempfile
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/acnh_input_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhInputTests(unittest.TestCase):
    def test_dialogue_rush_edge_and_synthetic_button_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "acnh-input-probe"
            result = subprocess.run(
                [
                    "xcrun",
                    "clang",
                    "-DGAME_ACNH",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-deprecated-declarations",
                    "-fsanitize=undefined",
                    "-fno-sanitize-recover=undefined",
                    str(PROBE),
                    f"-I{RUNTIME}",
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("连按空格快进对话", result.stderr)
            self.assertIn("已停止快进对话", result.stderr)
            self.assertIn("影片已开始，停止快进", result.stderr)


if __name__ == "__main__":
    unittest.main()
