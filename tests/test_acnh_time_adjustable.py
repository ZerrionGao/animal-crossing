from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/time_adjustable_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhAdjustableClockTests(unittest.TestCase):
    def compile_probe(self, directory, game_define):
        binary = Path(directory) / f"adjustable-clock-{game_define.lower()}"
        compile_flags = ["-DACNH_TIME_TEST_CLOCKS"] if game_define == "GAME_ACNH" else []
        result = subprocess.run(
            [
                "xcrun", "clang", f"-D{game_define}", *compile_flags, "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(PROBE), str(RUNTIME / "time_host.c"), f"-I{RUNTIME}", "-pthread", "-framework", "CoreFoundation", "-o", str(binary),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_acnh_initialize_adjust_elapsed_time_and_finalize(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            env = {**os.environ, "TOTK_TIME_OFFSET": "60"}
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_acnh_adjust_before_initialize_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "adjust-before-init"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("adjustable clock Adjust before Initialize", result.stderr)

    def test_acnh_initialize_and_reinitialize_establish_adjusted_time(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            env = {**os.environ, "TOTK_TIME_OFFSET": "60"}
            result = subprocess.run([str(binary), "reinit-auto-adjust"], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_acnh_null_output_is_a_fatal_precondition(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "null-output"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("adjustable clock GetCurrentTime null output", result.stderr)

    def test_adjustable_clock_symbols_are_not_overridden_outside_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_BOTW")
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
