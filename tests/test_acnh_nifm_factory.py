from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/ipc_domain_probe.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhNifmFactoryTests(unittest.TestCase):
    def test_factory_object_routes_and_closes_through_production_domain_dispatch(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "nifm-factory-probe"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(RUNTIME / "ipc.c"), str(RUNTIME / "nifm_offline.c"),
                    f"-I{RUNTIME}", "-pthread", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)

            valid = subprocess.run([str(binary), "factory-valid"], capture_output=True, text=True)
            self.assertEqual(valid.returncode, 0, valid.stderr)
            self.assertIn("RequirementPreset=2", valid.stderr)
            self.assertIn("GetSystemEventReadableHandles", valid.stderr)
            for option in range(1, 6):
                self.assertIn(f"SetConnectionConfirmationOption={option}", valid.stderr)
            self.assertNotIn("unsupported cmd", valid.stderr)

            lifecycle = subprocess.run([str(binary), "factory-submit"], capture_output=True, text=True)
            self.assertEqual(lifecycle.returncode, 0, lifecycle.stderr)
            self.assertIn("GetRequestState=1", lifecycle.stderr)
            self.assertIn("GetResult=0x26e6e", lifecycle.stderr)
            self.assertIn("Submit: offline failure 0x8986e", lifecycle.stderr)
            self.assertIn("GetResult=0x8986e", lifecycle.stderr)
            self.assertNotIn("accepted", lifecycle.stderr.lower())

            for mode, expected in (
                ("factory-preset", "RequirementPreset=3"),
                ("factory-shape", "pid=1"),
                ("factory-type", "type=4"),
                ("factory-data", "data=8"),
                ("factory-token", "token=1"),
                ("factory-desc", "buffers=x1"),
                ("factory-unknown", "IRequest unsupported cmd=3"),
            ):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected, result.stderr)

            applet_info = subprocess.run([str(binary), "factory-applet-info"], capture_output=True, text=True)
            self.assertEqual(applet_info.returncode, 0, applet_info.stderr)
            self.assertIn("GetAppletInfo: no system applet offline (0x26e6e)", applet_info.stderr)
            self.assertIn("GetAppletInfo: no system applet offline (0x8986e)", applet_info.stderr)

            applet_info_shape = subprocess.run([str(binary), "factory-applet-info-shape"], capture_output=True, text=True)
            self.assertEqual(applet_info_shape.returncode, 0, applet_info_shape.stderr)
            self.assertIn("GetAppletInfo rejected shape", applet_info_shape.stderr)
            self.assertIn("buffers=x0/a0/b0", applet_info_shape.stderr)

            rejected_submit = subprocess.run([str(binary), "factory-submit-option"], capture_output=True, text=True)
            self.assertNotEqual(rejected_submit.returncode, 0)
            self.assertIn("Submit rejected confirmation option=1", rejected_submit.stderr)

            for mode, expected in (
                ("factory-lifecycle-shape-0x", "GetRequestState rejected shape"),
                ("factory-lifecycle-shape-1p", "GetResult rejected shape"),
                ("factory-lifecycle-shape-4x", "Submit rejected shape"),
            ):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(expected, result.stderr)

            bad_child_shape = subprocess.run([str(binary), "factory-child-shape"], capture_output=True, text=True)
            self.assertEqual(bad_child_shape.returncode, 0, bad_child_shape.stderr)
            self.assertIn("GetSystemEventReadableHandles rejected shape", bad_child_shape.stderr)

            for mode, expected in (
                ("factory-option-invalid-0", "rejected value=0"),
                ("factory-option-invalid-6", "rejected value=6"),
                ("factory-option-invalid-255", "rejected value=255"),
                ("factory-option-type", "rejected shape: type=4"),
                ("factory-option-data", "rejected shape: type=6"),
                ("factory-option-token", "rejected shape: type=6"),
                ("factory-option-pid", "rejected shape: type=6"),
                ("factory-option-desc", "rejected shape: type=6"),
            ):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("SetConnectionConfirmationOption", result.stderr)
                    self.assertIn(expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
