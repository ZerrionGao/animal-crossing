from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/acnh_audout_probe.c"
RUNTIME = ROOT / "engine/runtime"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhAudoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "acnh-audout-probe"
        result = subprocess.run(
            [
                "xcrun",
                "clang",
                "-DACNH_AUDOUT_TEST_CAPACITY",
                "-DGAME_ACNH",
                "-std=c11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(PROBE),
                "-pthread",
                "-framework",
                "AudioToolbox",
                "-o",
                str(cls.binary),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            cls.temp.cleanup()
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, mode, expected=0):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def test_nonzero_offset_uses_data_size_as_length(self):
        self.run_probe("offset")

    def test_valid_near_full_stop_and_flush_retain_every_tag(self):
        self.run_probe("stop-near-full")
        self.run_probe("flush-near-full")

    def test_append_rejects_bounds_overflow_and_frame_misalignment(self):
        for mode in (
            "append-bad-capacity",
            "append-bad-size-max",
            "append-bad-address-wrap",
            "append-bad-offset-wrap",
            "append-bad-frame-size",
            "append-bad-frame-offset",
        ):
            with self.subTest(mode=mode):
                self.run_probe(mode, expected=42)

    def test_append_rejects_when_outstanding_release_capacity_is_exhausted(self):
        self.run_probe("append-full", expected=42)

    def test_stop_and_flush_fail_loudly_instead_of_dropping_an_impossible_tag(self):
        self.run_probe("stop-full", expected=42)
        self.run_probe("flush-full", expected=42)


if __name__ == "__main__":
    unittest.main()
