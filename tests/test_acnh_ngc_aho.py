from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/ngc_aho_probe.c"
SOURCE = ROOT / "engine/runtime/ngc_aho.c"
ROMFS = ROOT / "work/research/ngword2-from-xci-20261023/romfs"


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhNgcAhoTests(unittest.TestCase):
    def test_real_dictionary_imports_and_matches_first_word_only(self):
        if not (ROMFS / "ac_common_not_b_nx").is_file():
            self.skipTest("本机 NgWord2 词库不在工作区")
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "ngc-aho"
            compile_result = subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(PROBE), str(SOURCE), "-lz", "-o", str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            for name in ("ac_common_not_b_nx", "ac_0_not_b_nx", "ac_common_b1_nx"):
                with self.subTest(name=name):
                    result = subprocess.run(
                        [str(binary), str(ROMFS / name)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("sentinel-hits=0", result.stderr)
                    self.assertRegex(result.stderr, r"first-word-hits=[1-9]")
                    self.assertNotRegex(result.stderr, r"[^\x00-\x7f]{3,}")


if __name__ == "__main__":
    unittest.main()
