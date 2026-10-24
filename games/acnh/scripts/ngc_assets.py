#!/usr/bin/env python3
"""Prepare a private, integrity-checked inventory of local NgWord2 resources."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zlib

from assets import _new_output, _staging, read_keys
from upstream import AssetError, REPO_ROOT, require, upstream_font_module

NGWORD2_TITLE_ID = "0100000000000823"
SUPPORTED_CONTENT_TYPES = {4, 5}  # Data, PublicData
DEFAULT_OUTPUT = REPO_ROOT / "work/acnh/ngc-private"
MAX_SOURCE_FILE_SIZE = 64 * 1024 * 1024
MAX_DECOMPRESSED_FILE_SIZE = 64 * 1024 * 1024
MAX_TOTAL_OUTPUT_SIZE = 256 * 1024 * 1024
MAX_VERSION_SIZE = 4096
CHUNK_SIZE = 64 * 1024


def expected_names() -> set[str]:
    names = {"version.dat", "ac_common_not_b_nx", "ac_common_b1_nx", "ac_common_b2_nx",
             "ac_similar_form_nx", "table_similar_form_nx"}
    for index in range(16):
        names.update({f"ac_{index}_{kind}_nx" for kind in ("not_b", "b1", "b2")})
    return names


EXPECTED_NAMES = expected_names()


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _decompress_gzip_bounded(data: bytes, label: str, limit: int = MAX_DECOMPRESSED_FILE_SIZE) -> bytes:
    require(len(data) <= MAX_SOURCE_FILE_SIZE, f"压缩资源超过大小上限: {label}")
    require(data.startswith(b"\x1f\x8b"), f"资源不是受支持的 gzip 流: {label}")
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    output = bytearray()
    try:
        for offset in range(0, len(data), CHUNK_SIZE):
            chunk = data[offset:offset + CHUNK_SIZE]
            while chunk:
                room = limit + 1 - len(output)
                require(room > 0, f"解压资源超过大小上限: {label}")
                output.extend(decoder.decompress(chunk, room))
                require(len(output) <= limit, f"解压资源超过大小上限: {label}")
                chunk = decoder.unconsumed_tail
            require(not decoder.unused_data, f"gzip 资源包含尾随数据或多个成员: {label}")
        output.extend(decoder.flush())
    except (OSError, EOFError, zlib.error) as error:
        raise AssetError(f"gzip 资源损坏: {label}") from None
    require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
            f"gzip 资源未完整结束: {label}")
    require(len(output) <= limit, f"解压资源超过大小上限: {label}")
    return bytes(output)


def _check_resource_name(name: str) -> str:
    require(name in EXPECTED_NAMES, f"NgWord2 中存在未知资源名: {name}")
    return name


def _collect_directory(root: Path) -> dict[str, bytes]:
    require(root.is_dir() and not root.is_symlink(), "RomFS 输入必须是普通目录")
    files: dict[str, bytes] = {}
    total = 0
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), "RomFS 输入目录不能包含符号链接")
        if path.is_dir():
            continue
        require(path.is_file(), "RomFS 输入包含非普通文件")
        relative = PurePosixPath(path.relative_to(root).as_posix())
        require(len(relative.parts) == 1, f"资源必须位于 RomFS 根目录: {relative}")
        name = _check_resource_name(relative.name)
        require(name not in files, f"NgWord2 资源重复: {name}")
        require(path.stat().st_size <= MAX_SOURCE_FILE_SIZE, f"资源超过大小上限: {name}")
        total += path.stat().st_size
        require(total <= MAX_TOTAL_OUTPUT_SIZE, "NgWord2 输入超过总大小上限")
        files[name] = path.read_bytes()
    _require_complete(files)
    return files


def _require_complete(files: dict[str, bytes]) -> None:
    missing = sorted(EXPECTED_NAMES - files.keys())
    require(not missing, "NgWord2 资源缺失: " + ", ".join(missing))


def _extract_nca_source(nca_spec, source_size: int, key_map: dict[str, bytes], stage: Path) -> tuple[dict[str, bytes], int]:
    with upstream_font_module(key_map) as extractor:
        def open_nca():
            if isinstance(nca_spec, Path):
                return nca_spec.open("rb")
            xci_path, offset = nca_spec
            return extractor.Slice(xci_path, offset)

        with open_nca() as nca:
            header = extractor.nca_header(nca, key_map)
        require(header[0x200:0x204] == b"NCA3", "NgWord2 输入不是受支持的 NCA3")
        content_type = header[0x205]
        require(content_type in SUPPORTED_CONTENT_TYPES,
                "NgWord2 NCA content type 必须为 Data 或 PublicData")
        title_id = f"{struct.unpack_from('<Q', header, 0x210)[0]:016X}"
        require(title_id == NGWORD2_TITLE_ID, "NCA title ID 不是 NgWord2 (0100000000000823)")
        require(struct.unpack_from("<Q", header, 0x208)[0] == source_size,
                "NCA header size 与输入文件长度不一致")
        image_path = stage / ".ngword2-romfs.tmp"
        try:
            nca_for_extract = nca_spec if isinstance(nca_spec, Path) else open_nca()
            extractor.nca_romfs(nca_for_extract, key_map, image_path)
            entries = extractor.index_romfs(image_path)
            found: dict[str, bytes] = {}
            total = 0
            with image_path.open("rb") as image:
                for entry in entries:
                    path = PurePosixPath(entry["path"])
                    require(len(path.parts) == 1, f"资源必须位于 RomFS 根目录: {path}")
                    name = _check_resource_name(path.name)
                    require(name not in found, f"NgWord2 资源重复: {name}")
                    require(0 <= entry["offset"] <= entry["offset"] + entry["size"] <= image_path.stat().st_size,
                            f"RomFS 资源越界: {name}")
                    require(entry["size"] <= MAX_SOURCE_FILE_SIZE, f"资源超过大小上限: {name}")
                    total += entry["size"]
                    require(total <= MAX_TOTAL_OUTPUT_SIZE, "NgWord2 RomFS 资源超过总大小上限")
                    image.seek(entry["offset"])
                    data = image.read(entry["size"])
                    require(len(data) == entry["size"], f"RomFS 资源读取不完整: {name}")
                    found[name] = data
            _require_complete(found)
            return found, content_type
        finally:
            image_path.unlink(missing_ok=True)


def _extract_nca(nca_path: Path, key_paths: list[Path], stage: Path) -> dict[str, bytes]:
    key_map = read_keys(key_paths)
    files, _ = _extract_nca_source(nca_path, nca_path.stat().st_size, key_map, stage)
    return files


def _find_nca(folder: Path, key_paths: list[Path]) -> Path:
    require(folder.is_dir() and not folder.is_symlink(), "NCA 输入必须是普通目录")
    matches: list[Path] = []
    key_map = read_keys(key_paths)
    with upstream_font_module(key_map) as extractor:
        for path in sorted(folder.rglob("*")):
            require(not path.is_symlink(), "NCA 输入目录不能包含符号链接")
            if not path.is_file() or (path.suffix.lower() != ".nca" and path.name != "00"):
                continue
            with path.open("rb") as stream:
                header = extractor.nca_header(stream, key_map)
            if header[0x200:0x204] != b"NCA3":
                continue
            if f"{struct.unpack_from('<Q', header, 0x210)[0]:016X}" == NGWORD2_TITLE_ID:
                require(header[0x205] in SUPPORTED_CONTENT_TYPES,
                        "NgWord2 title 对应的 NCA content type 必须为 Data 或 PublicData")
                require(struct.unpack_from("<Q", header, 0x208)[0] == path.stat().st_size,
                        "NCA header size 与输入文件长度不一致")
                matches.append(path)
    require(len(matches) == 1, "NCA 目录中必须恰有一个 NgWord2 Data NCA")
    return matches[0]


def _read_nca_directory(source: Path, key_paths: list[Path], stage: Path) -> tuple[dict[str, bytes], str, int]:
    key_map = read_keys(key_paths)
    if source.is_file():
        nca = source
    else:
        nca = _find_nca(source, key_paths)
    files, content_type = _extract_nca_source(nca, nca.stat().st_size, key_map, stage)
    return files, "nca-fs-header-and-six-ivfc-levels-verified-rsa-unchecked", content_type


def _read_xci(source: Path, key_paths: list[Path], stage: Path) -> tuple[dict[str, bytes], str, int]:
    key_map = read_keys(key_paths)
    with upstream_font_module(key_map) as extractor:
        with source.open("rb") as card:
            require(extractor.read_at(card, 0x100, 4) == b"HEAD", "輸入不是有效的 XCI")
            root = struct.unpack("<Q", extractor.read_at(card, 0x130, 8))[0]
            partitions = extractor.hfs0_entries(card, root)
            updates = [entry for entry in partitions if entry[0] == "update"]
            require(len(updates) == 1, "XCI 必须恰有一个 update 分区")
            ncas = []
            for name, offset, size in extractor.hfs0_entries(card, updates[0][1]):
                if not name.lower().endswith(".nca") or name.lower().endswith(".cnmt.nca"):
                    continue
                with extractor.Slice(source, offset) as nca:
                    header = extractor.nca_header(nca, key_map)
                if header[0x200:0x204] != b"NCA3":
                    continue
                title_id = f"{struct.unpack_from('<Q', header, 0x210)[0]:016X}"
                if title_id == NGWORD2_TITLE_ID:
                    require(header[0x205] in SUPPORTED_CONTENT_TYPES,
                            "NgWord2 title 对应的 NCA content type 必须为 Data 或 PublicData")
                    require(struct.unpack_from("<Q", header, 0x208)[0] == size,
                            "NgWord2 NCA header size 与 XCI 分区记录不一致")
                    ncas.append((name, offset, size, header[0x205]))
        require(len(ncas) == 1, "XCI update 分区中必须恰有一个 NgWord2 Data 或 PublicData NCA")
        _, offset, size, _ = ncas[0]
    files, content_type = _extract_nca_source((source, offset), size, key_map, stage)
    return files, "nca-fs-header-and-six-ivfc-levels-verified-rsa-unchecked-from-xci-update", content_type


def _write_prepared(files: dict[str, bytes], source_integrity: str, output: Path,
                    content_type: int | None = None) -> dict[str, object]:
    _require_complete(files)
    rows: list[dict[str, object]] = []
    total = 0
    for name in sorted(files):
        compressed = files[name]
        require(len(compressed) <= MAX_SOURCE_FILE_SIZE, f"资源超过大小上限: {name}")
        if name.endswith("_nx"):
            plain = _decompress_gzip_bounded(compressed, name)
            encoding = "gzip"
        else:
            require(name == "version.dat" and len(compressed) == 4,
                    "version.dat 必须是受支持的 4 字节内容版本格式")
            require(len(compressed) <= MAX_VERSION_SIZE, "version.dat 超过大小上限")
            plain = compressed
            encoding = "big-endian-u32"
        total += len(compressed) + len(plain)
        require(total <= MAX_TOTAL_OUTPUT_SIZE, "NgWord2 总输出超过大小上限")
        (output / name).write_bytes(compressed)
        if name.endswith("_nx"):
            (output / f"{name}.decompressed").write_bytes(plain)
        rows.append({"name": name, "encoding": encoding,
                     "compressed_size": len(compressed), "compressed_sha256": _sha256(compressed),
                     "decompressed_size": len(plain), "decompressed_sha256": _sha256(plain)})
    manifest: dict[str, object] = {
        "status": "prepared-private-assets",
        "source_integrity": source_integrity,
        "title_id": NGWORD2_TITLE_ID if source_integrity.startswith("nca-") else None,
        "content_type": ({4: "data", 5: "public-data"}.get(content_type) if content_type is not None else None),
        "version_dat": {"format": "big-endian-u32", "content_version": int.from_bytes(files["version.dat"], "big")},
        "runtime_compatibility": "unverified; importer does not parse Aho-Corasick payloads",
        "resource_count": len(rows),
        "resources": rows,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return manifest


def prepare(source: Path, keys: list[Path], output: Path) -> dict[str, object]:
    source_arg = source.expanduser()
    require(source_arg.exists() and not source_arg.is_symlink(), "输入路径不存在或为符号链接")
    source_arg = source_arg.resolve(strict=True)
    output = _new_output(output)
    with _staging(output) as stage:
        if source_arg.is_file() and source_arg.suffix.lower() == ".xci":
            require(bool(keys), "XCI 解密需要 --keys")
            files, integrity, content_type = _read_xci(source_arg, keys, stage)
        elif source_arg.is_file() or any(path.is_file() and (path.suffix.lower() == ".nca" or path.name == "00")
                                         for path in source_arg.rglob("*")):
            require(bool(keys), "NCA 解密需要 --keys")
            files, integrity, content_type = _read_nca_directory(source_arg, keys, stage)
        elif source_arg.is_dir():
            files = _collect_directory(source_arg)
            integrity = "decrypted-romfs-unverified-title-and-integrity"
            content_type = None
        else:
            raise AssetError("输入必须是 Data NCA、含 NCA 的目录或解密 RomFS 目录")
        return _write_prepared(files, integrity, stage, content_type)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="本机自有 NgWord2 XCI、NCA、NCA 目录或解密 RomFS 目录")
    parser.add_argument("--keys", action="append", default=[], type=Path, help="NCA 解密密钥文件，可重复指定")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        manifest = prepare(args.source, args.keys, args.output)
        print(json.dumps({"status": manifest["status"], "resource_count": manifest["resource_count"],
                          "source_integrity": manifest["source_integrity"],
                          "output": str(args.output)}, ensure_ascii=False))
    except (AssetError, OSError, RuntimeError, ValueError) as error:
        print(f"错误: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
