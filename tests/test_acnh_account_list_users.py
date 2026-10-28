from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/account_list_users_probe.c"
ACCOUNT = ROOT / "engine/runtime/account.c"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhAccountListUsersTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compile_dir = tempfile.TemporaryDirectory()
        cls.binaries = {}
        for name, flags in (("acnh", ["-DGAME_ACNH"]), ("botw", ["-DGAME_BOTW"]), ("other", [])):
            binary = Path(cls.compile_dir.name) / f"account-list-{name}"
            result = subprocess.run(
                ["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                 str(PROBE), str(ACCOUNT), "-o", str(binary)],
                capture_output=True, text=True,
            )
            if result.returncode:
                raise AssertionError(result.stderr)
            cls.binaries[name] = binary

    @classmethod
    def tearDownClass(cls):
        cls.compile_dir.cleanup()

    @staticmethod
    def run_probe(binary, profile):
        result = subprocess.run(
            [str(binary)],
            env={**os.environ, "TOTK_PROFILE_DIR": str(profile)},
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_acnh_production_list_reports_one_persistent_user_with_capacity_and_canary_checks(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            binary = self.binaries["acnh"]
            self.run_probe(binary, profile)
            saved_uid = (profile / "account.uid").read_bytes()
            self.assertEqual(len(saved_uid), 16)
            self.run_probe(binary, profile)
            self.assertEqual((profile / "account.uid").read_bytes(), saved_uid)

    def test_botw_behavior_is_preserved_and_non_game_symbol_is_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp).resolve() / "profile"
            self.run_probe(self.binaries["botw"], profile)
            self.run_probe(self.binaries["other"], profile)


if __name__ == "__main__":
    unittest.main()
