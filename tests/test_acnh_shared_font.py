import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests/acnh_shared_font_probe.c"
SOURCE = ROOT / "third_party/macnativegames/games/totk/runtime/shared_font.c"
RUNTIME = ROOT / "third_party/macnativegames/games/totk/runtime"
DECODED_MAGIC = 0x7F9A0218


def make_font(kind, payload_size=None, header_size=None):
    size = payload_size if payload_size is not None else 8 + kind * 4
    payload = b"" if size == 0 else b"\x00\x01\x00\x00" + bytes([ord("A") + kind]) * (size - 4)
    key = 0x12345678 + kind
    declared_size = size if header_size is None else header_size
    header = struct.pack(">II", key ^ DECODED_MAGIC, declared_size ^ key)
    encrypted = b"".join(
        struct.pack(">I", struct.unpack(">I", payload[offset:offset + 4])[0] ^ key)
        for offset in range(0, size, 4)
    )
    return header + encrypted


def write_config(path, folder, *, mismatch=False):
    rows = []
    for kind in range(6):
        file_path = folder / f"font-{kind}.bfttf"
        data = file_path.read_bytes()
        font_size = len(data) - 8 - (4 if mismatch and kind == 0 else 0)
        rows.append(
            f"{{{json.dumps(str(file_path))},{len(data)}ULL,{font_size}ULL,"
            f"{json.dumps(hashlib.sha256(data).hexdigest())}}}"
        )
    path.write_text(
        "#define SHARED_FONT_COUNT 6\n"
        "#define SHARED_FONT_ROWS " + ",".join(rows) + "\n"
    )


def compile_probe(binary, config_dir):
    result = subprocess.run(
        [
            "xcrun", "clang", "-DGAME_ACNH", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
            str(PROBE), str(SOURCE), f"-I{config_dir}", f"-I{RUNTIME}", "-pthread", "-o", str(binary),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise AssertionError(result.stderr)


@unittest.skipUnless(shutil.which("xcrun"), "requires the macOS compiler")
class AcnhSharedFontTests(unittest.TestCase):
    def test_request_load_decodes_verified_fonts_and_rejects_bad_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fonts = root / "fonts"
            fonts.mkdir()
            for kind in range(6):
                (fonts / f"font-{kind}.bfttf").write_bytes(make_font(kind))

            config = root / "config"
            config.mkdir()
            write_config(config / "shared_fonts_config.h", fonts)
            binary = root / "shared-font-probe"
            compile_probe(binary, config)

            env = {**os.environ, "TOTK_SHARED_FONTS": str(fonts)}
            for mode in ("request-first", "address-first", "count-first"):
                result = subprocess.run([str(binary), mode], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

            corrupted = root / "corrupted"
            shutil.copytree(fonts, corrupted)
            damaged = corrupted / "font-3.bfttf"
            changed = bytearray(damaged.read_bytes())
            changed[-1] ^= 0x01
            damaged.write_bytes(changed)
            corrupt_env = {**env, "TOTK_SHARED_FONTS": str(corrupted), "PROBE_EXPECT_FATAL": "1"}
            result = subprocess.run([str(binary), "request-first"], env=corrupt_env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 70, result.stderr)

            bad_config = root / "bad-config"
            bad_config.mkdir()
            write_config(bad_config / "shared_fonts_config.h", fonts, mismatch=True)
            bad_binary = root / "shared-font-size-probe"
            compile_probe(bad_binary, bad_config)
            bad_env = {**env, "PROBE_EXPECT_FATAL": "1"}
            result = subprocess.run([str(bad_binary), "request-first"], env=bad_env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 70, result.stderr)

            header_fonts = root / "header-mismatch-fonts"
            header_fonts.mkdir()
            for kind in range(6):
                size = 8 + kind * 4
                header_size = size - 4 if kind == 0 else None
                (header_fonts / f"font-{kind}.bfttf").write_bytes(make_font(kind, header_size=header_size))
            header_config = root / "header-mismatch-config"
            header_config.mkdir()
            write_config(header_config / "shared_fonts_config.h", header_fonts)
            header_binary = root / "shared-font-header-size-probe"
            compile_probe(header_binary, header_config)
            header_env = {**env, "TOTK_SHARED_FONTS": str(header_fonts), "PROBE_EXPECT_FATAL": "1"}
            result = subprocess.run([str(header_binary), "request-first"], env=header_env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 70, result.stderr)

            small_fonts = root / "small-fonts"
            small_fonts.mkdir()
            for kind in range(6):
                size = 0 if kind == 0 else None
                (small_fonts / f"font-{kind}.bfttf").write_bytes(make_font(kind, size))
            small_config = root / "small-config"
            small_config.mkdir()
            write_config(small_config / "shared_fonts_config.h", small_fonts)
            small_binary = root / "shared-font-small-payload-probe"
            compile_probe(small_binary, small_config)
            small_env = {**env, "TOTK_SHARED_FONTS": str(small_fonts), "PROBE_EXPECT_FATAL": "1"}
            result = subprocess.run([str(small_binary), "request-first"], env=small_env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 70, result.stderr)


if __name__ == "__main__":
    unittest.main()
