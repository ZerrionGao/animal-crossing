from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/pseudo_device_probe.c"
APPLET = ROOT / "third_party/macnativegames/games/totk/runtime/applet_host.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"
SYMBOL = "_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhPseudoDeviceIdTests(unittest.TestCase):
    def compile_probe(self, directory, acnh=True):
        binary = Path(directory) / ("pseudo-acnh" if acnh else "pseudo-other")
        command = ["xcrun", "clang"]
        if acnh:
            command.append("-DGAME_ACNH")
        command.extend([
            "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(PROBE), str(APPLET), f"-I{RUNTIME}", "-pthread", "-o", str(binary),
        ])
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def run_probe(self, binary, mode, profile):
        env = os.environ.copy()
        env["TOTK_PROFILE_DIR"] = str(profile)
        result = subprocess.run([str(binary), mode], capture_output=True, env=env)
        result.stderr_text = result.stderr.decode()
        return result

    def test_new_id_is_stable_uuid_and_logged_once(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            profile = Path(temp) / "profile"
            first = self.run_probe(binary, "write", profile)
            self.assertEqual(first.returncode, 0, first.stderr_text)
            self.assertEqual(len(first.stdout), 16)
            self.assertEqual(first.stderr_text.count("persistent local pseudo device id loaded"), 1)
            self.assertNotIn(first.stdout.hex(), first.stderr_text)
            stored = (profile / "pseudo-device-id").read_bytes()
            self.assertEqual(stored, first.stdout)
            second = self.run_probe(binary, "read", profile)
            self.assertEqual(second.returncode, 0, second.stderr_text)
            self.assertEqual(second.stdout, first.stdout)

    def test_separate_profiles_do_not_share_an_id(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            first = self.run_probe(binary, "write", Path(temp) / "one")
            second = self.run_probe(binary, "write", Path(temp) / "two")
            self.assertEqual(first.returncode, 0, first.stderr_text)
            self.assertEqual(second.returncode, 0, second.stderr_text)
            self.assertNotEqual(first.stdout, second.stdout)

    def test_existing_bytes_are_returned_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            profile = Path(temp) / "profile"
            profile.mkdir()
            stored = bytes(range(16))
            (profile / "pseudo-device-id").write_bytes(stored)
            result = self.run_probe(binary, "read", profile)
            self.assertEqual(result.returncode, 0, result.stderr_text)
            self.assertEqual(result.stdout, stored)

    def test_null_output_fails_before_creating_a_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            profile = Path(temp) / "profile"
            result = self.run_probe(binary, "null", profile)
            self.assertEqual(result.returncode, 0, result.stderr_text)
            self.assertIn("GetPseudoDeviceId output is null", result.stderr_text)
            self.assertFalse(profile.exists())

    def test_corrupt_identity_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp)
            for payload in (b"\x00" * 16, b"\x11" * 8):
                profile = Path(temp) / payload.hex()
                profile.mkdir()
                (profile / "pseudo-device-id").write_bytes(payload)
                result = self.run_probe(binary, "read", profile)
                self.assertNotEqual(result.returncode, 0, result.stderr_text)
                self.assertIn("corrupt pseudo device id", result.stderr_text)

    def test_symbol_is_absent_without_game_acnh(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = self.compile_probe(temp, acnh=False)
            result = self.run_probe(binary, "absent", Path(temp) / "unused")
            self.assertEqual(result.returncode, 0, result.stderr_text)
            self.assertNotIn(SYMBOL, result.stderr_text)


if __name__ == "__main__":
    unittest.main()
