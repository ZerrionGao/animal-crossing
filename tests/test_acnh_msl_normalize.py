import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "games/acnh/runtime/acnh_msl_normalize.c"
CLI_SOURCE = ROOT / "games/acnh/tools/acnh_msl_normalize.c"
CPU_TEST = ROOT / "tests/acnh_msl_normalize_test.c"


class AcnhMslNormalizerCliTest(unittest.TestCase):
    def test_cpu_cases_under_address_sanitizer(self):
        with tempfile.TemporaryDirectory(prefix="acnh-msl-test-") as temp:
            binary = Path(temp) / "acnh_msl_normalize_test"
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            f"-I{SOURCE.parent}", str(CPU_TEST), str(SOURCE), "-o", str(binary)], check=True)
            result = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("ACNH MSL normalizer CPU tests passed", result.stdout)

    def test_policy_flags_and_normalization_output(self):
        with tempfile.TemporaryDirectory(prefix="acnh-msl-test-") as temp:
            binary = Path(temp) / "acnh_msl_normalize"
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(CLI_SOURCE), str(SOURCE), "-o", str(binary)], check=True)
            flags = subprocess.check_output([binary, "--policy-id=acnh-metal-v1", "--metal-flags"], text=True).strip()
            self.assertEqual(flags, "-fmetal-math-mode=safe -fpreserve-invariance")
            self.assertNotEqual(subprocess.run([binary, "--metal-flags"], capture_output=True).returncode, 0)

            input_path, output_path = Path(temp) / "input.metal", Path(temp) / "output.metal"
            input_path.write_text("using namespace metal;\nfloat4 f() { return t.sample(s,c,uint(rint(-layer))); }\n")
            subprocess.run([binary, "--policy-id=acnh-metal-v1", input_path, output_path], check=True)
            normalized = output_path.read_text()
            self.assertIn("nvn_layer(uint(rint(-layer)), t.get_array_size())", normalized)
            self.assertIn("static inline uint nvn_layer(uint i, uint n)", normalized)

            output_path.write_text("keep existing output")
            input_path.write_text("t.sample(s,c,uint(rint(layer)")
            result = subprocess.run([binary, "--policy-id=acnh-metal-v1", input_path, output_path], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output_path.read_text(), "keep existing output")


if __name__ == "__main__":
    unittest.main()
