import contextlib
import gzip
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "games/acnh/scripts/ngc_assets.py"
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("ngc_assets", SCRIPT)
ngc_assets = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(ngc_assets)


def synthetic_resources() -> dict[str, bytes]:
    resources = {"version.dat": (3).to_bytes(4, "big")}
    for name in ngc_assets.EXPECTED_NAMES - {"version.dat"}:
        resources[name] = gzip.compress((f"synthetic payload for {name}\n".encode()) * 2)
    return resources


class NgcAssetTests(unittest.TestCase):
    def test_plain_romfs_prepares_manifest_with_compressed_and_decompressed_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source, output = root / "romfs", root / "prepared"
            source.mkdir()
            for name, data in synthetic_resources().items():
                (source / name).write_bytes(data)

            manifest = ngc_assets.prepare(source, [], output)

            self.assertEqual(manifest["source_integrity"], "decrypted-romfs-unverified-title-and-integrity")
            self.assertEqual(manifest["resource_count"], len(ngc_assets.EXPECTED_NAMES))
            row = next(item for item in manifest["resources"] if item["name"] == "ac_0_not_b_nx")
            raw = (source / row["name"]).read_bytes()
            plain = gzip.decompress(raw)
            self.assertEqual(row["compressed_size"], len(raw))
            self.assertEqual(row["compressed_sha256"], ngc_assets._sha256(raw))
            self.assertEqual(row["decompressed_size"], len(plain))
            self.assertEqual(row["decompressed_sha256"], ngc_assets._sha256(plain))
            self.assertEqual((output / f"{row['name']}.decompressed").read_bytes(), plain)
            self.assertEqual(manifest["version_dat"], {"format": "big-endian-u32", "content_version": 3})
            self.assertNotIn("synthetic payload", (output / "manifest.json").read_text())

    def test_unknown_missing_and_nested_resources_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "romfs"
            source.mkdir()
            for name, data in synthetic_resources().items():
                (source / name).write_bytes(data)
            (source / "unexpected.bin").write_bytes(b"unknown")
            with self.assertRaisesRegex(ngc_assets.AssetError, "未知资源名"):
                ngc_assets.prepare(source, [], root / "out-unknown")

            (source / "unexpected.bin").unlink()
            (source / "ac_0_b1_nx").unlink()
            with self.assertRaisesRegex(ngc_assets.AssetError, "资源缺失"):
                ngc_assets.prepare(source, [], root / "out-missing")

            nested = root / "nested"
            nested.mkdir()
            inner = nested / "romfs"
            inner.mkdir()
            (inner / "ac_0_b1_nx").write_bytes(gzip.compress(b"sample"))
            with self.assertRaisesRegex(ngc_assets.AssetError, "必须位于 RomFS 根目录"):
                ngc_assets._collect_directory(nested)

    def test_duplicate_nca_entries_fail(self):
        data = b"x" * 32
        image = b"".join([data, data])
        entries = [{"path": "ac_0_b1_nx", "offset": 0, "size": 32},
                   {"path": "ac_0_b1_nx", "offset": 32, "size": 32}]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "romfs.bin"
            path.write_bytes(image)
            class FakeExtractor:
                def nca_header(self, _stream, _keys):
                    return fake_header()
                def nca_romfs(self, _source, _keys, output):
                    output.write_bytes(image)
                def index_romfs(self, _image):
                    return entries
            @contextlib.contextmanager
            def fake_font_module(_keys):
                yield FakeExtractor()
            nca = Path(temporary) / "fake.nca"
            nca.write_bytes(b"x" * 0xC00)
            keyfile = Path(temporary) / "keys.txt"
            keyfile.write_text("header_key=" + "00" * 32 + "\n")
            with patch.object(ngc_assets, "upstream_font_module", fake_font_module):
                with self.assertRaisesRegex(ngc_assets.AssetError, "资源重复"):
                    ngc_assets._extract_nca(nca, [keyfile], Path(temporary))

    def test_nca_title_content_size_and_ivfc_failure_paths(self):
        resources = synthetic_resources()
        data = b"".join(resources[name] for name in sorted(resources))
        offsets = {}
        cursor = 0
        for name in sorted(resources):
            offsets[name] = (cursor, len(resources[name]))
            cursor += len(resources[name])

        class FakeExtractor:
            title_id = int(ngc_assets.NGWORD2_TITLE_ID, 16)
            content_type = 4
            ivfc_error = False
            bad_size = False
            def nca_header(self, stream, _keys):
                actual_size = len(data) if self.bad_size else Path(stream.name).stat().st_size
                return fake_header(self.title_id, self.content_type, actual_size + (1 if self.bad_size else 0))
            def nca_romfs(self, _source, _keys, output):
                if self.ivfc_error:
                    raise ngc_assets.AssetError("IVFC level 2 block failed")
                output.write_bytes(data)
            def index_romfs(self, _image):
                return [{"path": name, "offset": offsets[name][0], "size": offsets[name][1]}
                        for name in sorted(resources)]

        extractor = FakeExtractor()
        @contextlib.contextmanager
        def fake_font_module(_keys):
            yield extractor

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            nca = root / "0823.nca"
            nca.write_bytes(b"x" * len(data))
            keys = root / "keys.txt"
            sentinel = "00" * 32
            keys.write_text(f"header_key={sentinel}\n")
            with patch.object(ngc_assets, "upstream_font_module", fake_font_module):
                extractor.title_id = 0x0100000000000806
                with self.assertRaisesRegex(ngc_assets.AssetError, "title ID"):
                    ngc_assets._extract_nca(nca, [keys], root)
                extractor.title_id = int(ngc_assets.NGWORD2_TITLE_ID, 16)
                extractor.content_type = 0
                with self.assertRaisesRegex(ngc_assets.AssetError, "content type"):
                    ngc_assets._extract_nca(nca, [keys], root)
                extractor.content_type = 4
                extractor.bad_size = True
                with self.assertRaisesRegex(ngc_assets.AssetError, "header size"):
                    ngc_assets._extract_nca(nca, [keys], root)
                extractor.bad_size = False
                extractor.ivfc_error = True
                with self.assertRaisesRegex(ngc_assets.AssetError, "IVFC level"):
                    ngc_assets._extract_nca(nca, [keys], root)
                extractor.ivfc_error = False
                files = ngc_assets._extract_nca(nca, [keys], root)
                self.assertEqual(set(files), ngc_assets.EXPECTED_NAMES)
            self.assertNotIn(sentinel, repr(files))

    def test_xci_public_data_uses_verified_update_partition_nca(self):
        resources = synthetic_resources()
        image = b"".join(resources[name] for name in sorted(resources))
        entries = []
        cursor = 0
        for name in sorted(resources):
            data = resources[name]
            entries.append({"path": name, "offset": cursor, "size": len(data)})
            cursor += len(data)

        class FakeSlice:
            def __init__(self, path, base):
                self.path, self.base = path, base
                self.stream = path.open("rb")
            def __enter__(self):
                return self
            def __exit__(self, *_args):
                self.stream.close()

        class FakeExtractor:
            Slice = FakeSlice
            def read_at(self, stream, offset, length):
                stream.seek(offset)
                return stream.read(length)
            def hfs0_entries(self, _stream, offset):
                if offset == 0x200:
                    return [("update", 0x300, 176640 + 0xC00)]
                if offset == 0x300:
                    return [("8ed8fffa1a740fd1f85d863bc68f9321.nca", 0xC00, 176640)]
                raise AssertionError("unexpected HFS offset")
            def nca_header(self, stream, _keys):
                if getattr(stream, "base", 0) == 0xC00:
                    return fake_header(content_type=5, size=176640)
                return fake_header()
            def nca_romfs(self, _source, _keys, output):
                with _source:
                    output.write_bytes(image)
            def index_romfs(self, _image):
                return entries

        @contextlib.contextmanager
        def fake_font_module(_keys):
            yield FakeExtractor()

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            xci = root / "owned.xci"
            with xci.open("wb") as stream:
                stream.truncate(0xC00 + 176640)
                stream.seek(0x100)
                stream.write(b"HEAD")
                stream.seek(0x130)
                stream.write(struct.pack("<Q", 0x200))
            keys = root / "keys.txt"
            keys.write_text("header_key=" + "00" * 32 + "\n")
            output = root / "prepared"
            with patch.object(ngc_assets, "upstream_font_module", fake_font_module):
                manifest = ngc_assets.prepare(xci, [keys], output)
            self.assertEqual(manifest["content_type"], "public-data")
            self.assertIn("from-xci-update", manifest["source_integrity"])

    def test_version_and_gzip_payload_formats_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "romfs"
            source.mkdir()
            for name, data in synthetic_resources().items():
                (source / name).write_bytes(data)
            (source / "version.dat").write_bytes(b"bad")
            with self.assertRaisesRegex(ngc_assets.AssetError, "version.dat"):
                ngc_assets.prepare(source, [], root / "bad-version")
            (source / "version.dat").write_bytes((3).to_bytes(4, "big"))
            (source / "ac_0_b1_nx").write_bytes(b"unknown compressed format")
            with self.assertRaisesRegex(ngc_assets.AssetError, "gzip"):
                ngc_assets.prepare(source, [], root / "bad-gzip")

    def test_gzip_bomb_and_corrupt_stream_fail_without_unbounded_output(self):
        bomb = gzip.compress(b"a" * 10000)
        with self.assertRaisesRegex(ngc_assets.AssetError, "超过大小上限"):
            ngc_assets._decompress_gzip_bounded(bomb, "synthetic", limit=128)
        with self.assertRaisesRegex(ngc_assets.AssetError, "损坏|未完整结束"):
            ngc_assets._decompress_gzip_bounded(b"\x1f\x8b\x08broken", "synthetic")

    def test_symlink_and_existing_outputs_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source, output = root / "romfs", root / "prepared"
            source.mkdir()
            for name, data in synthetic_resources().items():
                (source / name).write_bytes(data)
            output.mkdir()
            with self.assertRaisesRegex(ngc_assets.AssetError, "已存在"):
                ngc_assets.prepare(source, [], output)
            linked_output = root / "linked"
            linked_output.symlink_to(root / "absent", target_is_directory=True)
            with self.assertRaisesRegex(ngc_assets.AssetError, "符号链接"):
                ngc_assets.prepare(source, [], linked_output)


def fake_header(title_id=0x0100000000000823, content_type=4, size=0xC00):
    header = bytearray(0xC00)
    header[0x200:0x204] = b"NCA3"
    header[0x205] = content_type
    struct.pack_into("<Q", header, 0x208, size)
    struct.pack_into("<Q", header, 0x210, title_id)
    return bytes(header)


if __name__ == "__main__":
    unittest.main()
