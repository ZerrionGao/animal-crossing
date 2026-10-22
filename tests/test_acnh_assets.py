import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

SCRIPT = Path(__file__).resolve().parents[1] / "games/acnh/scripts/assets.py"
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("acnh_assets", SCRIPT)
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)


class AssetInputTests(unittest.TestCase):
    def test_reads_plain_and_html_keys_without_exposing_values(self):
        secret = "ab" * 16
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            plain = root / "prod.keys"
            html = root / "saved.html"
            plain.write_text(f"header_key = {secret} # local\n")
            html.write_text(f"<html><body>保存页<pre>key_area_key_application_00 = {secret} header_key = {secret}</pre></body></html>")
            got = assets.read_keys([plain, html])
            self.assertEqual(got["header_key"], bytes.fromhex(secret))
            self.assertEqual(got["key_area_key_application_00"], bytes.fromhex(secret))
            self.assertEqual(assets.read_keys([html])["header_key"], bytes.fromhex(secret))

    def test_normalized_merge_keys_are_private_and_temporary(self):
        with tempfile.TemporaryDirectory() as temp:
            keys = Path(temp) / "keys"
            keys.write_text("header_key = " + "ef" * 16)
            with assets.normalized_keys([keys]) as normalized:
                self.assertEqual(normalized.stat().st_mode & 0o777, 0o600)
                self.assertEqual(len(normalized.read_text().splitlines()), 1)
            self.assertFalse(normalized.exists())

    def test_font_upstream_uses_in_memory_keys(self):
        keys = {"header_key": b"secret-test-key"}
        with assets.upstream_font_module(keys) as extractor:
            self.assertIs(extractor.load_keys("unused.keys"), keys)

    def test_missing_card_fonts_leave_no_success_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            xci = root / "card.xci"
            xci.write_bytes(b"synthetic card")
            keys = root / "keys"
            keys.write_text("header_key = " + "01" * 16)
            output = root / "shared-fonts"
            original = assets._run_main

            def missing(_module, argv):
                Path(argv[argv.index("--output") + 1]).mkdir(parents=True)
                raise ValueError("Shared font data missing from the card update partition")

            assets._run_main = missing
            try:
                with self.assertRaisesRegex(assets.AssetError, "没有可用的共享字体"):
                    assets.fonts(xci, [keys], output)
            finally:
                assets._run_main = original
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".shared-fonts.acnh-*")), [])

    def test_font_manifest_points_to_emitted_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            xci = root / "card.xci"
            xci.write_bytes(b"synthetic card")
            keys = root / "keys"
            keys.write_text("header_key = " + "02" * 16)
            output = root / "shared-fonts"
            original = assets._run_main

            def emit(_module, argv):
                result = Path(argv[argv.index("--output") + 1])
                result.mkdir(parents=True)
                (result / "0-standard.bfttf").write_bytes(b"font")
                (result / "manifest.json").write_text(json.dumps({"status": "pass", "fonts": [{
                    "type": 0, "name": "standard", "file": "inside-romfs.bfttf",
                    "bfttf_bytes": 4, "font_bytes": 4, "sha256": "a" * 64,
                }]}))

            assets._run_main = emit
            try:
                with redirect_stdout(io.StringIO()):
                    assets.fonts(xci, [keys], output)
            finally:
                assets._run_main = original
            manifest = json.loads((output / "manifest.json").read_text())
            row = manifest["fonts"][0]
            self.assertEqual(row["file"], "0-standard.bfttf")
            self.assertEqual(row["source_file"], "inside-romfs.bfttf")
            self.assertTrue((output / row["file"]).is_file())

    def test_invalid_keys_do_not_leak_values_to_cli_error(self):
        secret = "cd" * 32
        with tempfile.TemporaryDirectory() as temp:
            keys = Path(temp) / "bad.html"
            keys.write_text(f"<html><pre>header_key = {secret}xyz</pre></html>")
            err = io.StringIO()
            with redirect_stderr(err):
                result = assets.main(["extract", "--base", str(keys), "--update", str(keys), "--keys", str(keys), "--output", str(Path(temp) / "out")])
            self.assertEqual(result, 2)
            self.assertNotIn(secret, err.getvalue())

    def test_rejects_existing_output_and_symlink_parent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            source = root / "input.nsp"
            source.write_bytes(b"data")
            keys = root / "keys"
            keys.write_text("header_key = " + "00" * 32)
            existing = root / "already"
            existing.mkdir()
            with self.assertRaisesRegex(assets.AssetError, "已存在"):
                assets.extract(source, source, [keys], existing)
            outside = root / "outside"
            outside.mkdir()
            link = root / "link"
            link.symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(assets.AssetError, "符号链接"):
                assets._new_output(link / "out")
            (outside / "nested" / "deeper").mkdir(parents=True)
            with self.assertRaisesRegex(assets.AssetError, "符号链接"):
                assets._new_output(link / "nested" / "deeper" / "out")
            dangling = root / "dangling"
            dangling.symlink_to(root / "missing-target", target_is_directory=True)
            with self.assertRaisesRegex(assets.AssetError, "符号链接"):
                assets._new_output(dangling / "nested" / "out")

    def test_rejects_wrong_title_and_requires_update_metadata(self):
        def row(title, kind=0):
            return ({"title_id": title, "content_type": kind}, bytearray(0xE00))

        with self.assertRaisesRegex(assets.AssetError, "其他游戏"):
            with assets.upstream_modules() as (exefs, _, _):
                exefs.program_nca([row("0100F2C0115B6000")])
        with self.assertRaisesRegex(assets.AssetError, "Meta NCA"):
            with assets.upstream_modules() as (exefs, _, _):
                exefs.program_nca([row(assets.BASE_TITLE_ID)], update=True)

    def test_upstream_failure_leaves_no_success_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            source = root / "input.nsp"
            source.write_bytes(b"data")
            keys = root / "keys"
            keys.write_text("header_key = " + "00" * 32)
            output = root / "out"
            original = assets._run_main

            def fail(module, argv):
                Path(argv[argv.index("--output") + 1]).mkdir(parents=True)
                raise RuntimeError("synthetic failure")

            assets._run_main = fail
            try:
                with self.assertRaisesRegex(RuntimeError, "synthetic failure"):
                    assets.extract(source, source, [keys], output)
            finally:
                assets._run_main = original
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".out.acnh-*")), [])

    def test_romfs_index_rejects_parent_path(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "romfs.bin"
            header = struct.pack("<10Q", 80, 0, 0, 80, 25, 105, 0, 105, 35, 140)
            root = struct.pack("<6I", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0) + b"\0"
            entry = struct.pack("<IIQQII", 0, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 2) + b"..\0"
            image.write_bytes(header + root + entry)
            with assets.upstream_modules() as (_, romfs, _):
                with self.assertRaisesRegex(ValueError, "Unsafe path"):
                    romfs.index_romfs(image)


if __name__ == "__main__":
    unittest.main()
