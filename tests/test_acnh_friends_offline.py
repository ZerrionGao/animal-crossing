from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/friends_ipc_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhFriendsOfflineTests(unittest.TestCase):
    def test_create_friend_service_routes_and_closes_domain_object(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "friends-ipc-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "friends_offline.c"),
                    f"-I{RUNTIME}", "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)

            run_result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)
            self.assertIn("opened service friend:u", run_result.stderr)
            self.assertIn("cmd=0 type=6 domain=1 object=1", run_result.stderr)

    def test_unimplemented_friend_service_command_fails_without_success_response(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "friends-ipc-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "friends_offline.c"),
                    f"-I{RUNTIME}", "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)

            run_result = subprocess.run([str(binary), "child-fail"], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 42, run_result.stderr)
            self.assertIn("service=IFriendService cmd=0", run_result.stderr)
            self.assertIn("ACNH friend:u command is not implemented", run_result.stderr)

    def test_create_friend_service_rejects_unexpected_arguments(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "friends-ipc-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "friends_offline.c"),
                    f"-I{RUNTIME}", "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)

            run_result = subprocess.run([str(binary), "malformed-root"], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 42, run_result.stderr)
            self.assertIn("service-creator request shape is not supported", run_result.stderr)


if __name__ == "__main__":
    unittest.main()
