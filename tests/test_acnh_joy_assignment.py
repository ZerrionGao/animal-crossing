from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/joy_assignment_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhJoyAssignmentTests(unittest.TestCase):
    def test_dual_mode_for_known_ids_and_absent_without_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            for target, define, expected, log in (
                ("acnh", "-DGAME_ACNH", "ACNH joy assignment probe passed", True),
                ("other", None, "non-ACNH target leaves joy assignment to the SDK", False),
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
                if log:
                    self.assertEqual(run_result.stderr.count("mode=dual"), 4)
                    self.assertIn("id=0x0 ", run_result.stderr)
                    self.assertIn("id=0x7 ", run_result.stderr)
                    self.assertIn("id=0x10 ", run_result.stderr)
                    self.assertIn("id=0x20 ", run_result.stderr)
                else:
                    self.assertNotIn("mode=dual", run_result.stderr)


if __name__ == "__main__":
    unittest.main()
