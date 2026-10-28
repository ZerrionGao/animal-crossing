from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/timezone_rule_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhTimezoneRuleTests(unittest.TestCase):
    def compile_probe(self, directory, game_define):
        binary = Path(directory) / f"timezone-rule-{game_define.lower()}"
        result = subprocess.run(
            [
                "xcrun", "clang", f"-D{game_define}", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(PROBE), str(RUNTIME / "time_host.c"), f"-I{RUNTIME}", "-pthread",
                "-framework", "CoreFoundation", "-o", str(binary),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_acnh_system_and_loaded_timezone_rules(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_acnh_malformed_location_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "invalid-name"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertIn("ACNH timezone location malformed", result.stderr)

    def test_acnh_malformed_timezone_rule_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "malformed-rule"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)

    def test_acnh_oversized_timezone_rule_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "oversized-rule"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)

    def test_acnh_tzif_internal_counts_are_validated(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "malformed-counts"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 42, result.stderr)

    def test_acnh_to_posix_rule_converts_local_time(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_ACNH")
            result = subprocess.run([str(binary), "to-posix-rule"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_timezone_symbols_are_not_overridden_outside_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, "GAME_BOTW")
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
