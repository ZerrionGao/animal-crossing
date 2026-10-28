from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/acnh_touch_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhTouchTests(unittest.TestCase):
    def test_touch_symbols_and_empty_state_abi(self):
        with tempfile.TemporaryDirectory() as temp:
            for target, define, expected in (
                ("acnh-mac", "-DGAME_ACNH", "ACNH touch HLE probe passed"),
                ("non-acnh", None, "non-ACNH target leaves touch services to the SDK"),
            ):
                binary = Path(temp) / target
                command = [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-ffunction-sections", "-fdata-sections",
                ]
                if define:
                    command.append(define)
                command.extend([
                    str(PROBE), str(RUNTIME / "input.c"), f"-I{RUNTIME}",
                    "-pthread", "-Wl,-undefined,dynamic_lookup", "-o", str(binary),
                ])
                compile_result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
                run_result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(run_result.returncode, 0, run_result.stderr)
                self.assertIn(expected, run_result.stdout)


if __name__ == "__main__":
    unittest.main()
