#!/usr/bin/env python3
"""安全提取和合并《集合啦！动物森友会》1.0.0 与 3.0.0 资源。"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from contextlib import redirect_stdout
from html.parser import HTMLParser
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Iterator

from upstream import AssetError, BASE_TITLE_ID, REPO_ROOT, require, upstream_font_module, upstream_modules


class _TextOnly(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []

    def handle_data(self, data: str) -> None:
        self.parts.append(data)


def _key_text(path: Path) -> str:
    try:
        raw = path.read_bytes().decode("utf-8-sig", errors="replace")
    except OSError:
        raise AssetError(f"无法读取密钥文件: {path}") from None
    return raw


def read_keys(paths: list[Path]) -> dict[str, bytes]:
    """读取 plain keys 或网页保存文件，错误信息不包含密钥内容。"""
    require(bool(paths), "至少需要一个 --keys 文件")
    result: dict[str, bytes] = {}
    for path in paths:
        text = _key_text(path)
        found = 0
        html = "<" in text and ">" in text
        if html:
            parser = _TextOnly()
            try:
                parser.feed(text)
                parser.close()
            except Exception:
                raise AssetError(f"密钥文件格式无效: {path}") from None
            text = "\n".join(parser.parts)
            # HTML 页面可能把多个赋值放进同一个文本节点，或把名称与值放在相邻单元格。
            candidates = [(m.group(1), m.group(2), m.start()) for m in
                          re.finditer(r"\b([A-Za-z0-9_]+)\s*=\s*([0-9a-fA-F]{32,128})(?=\s|$)", text)]
        else:
            candidates = []
            for number, raw_line in enumerate(text.splitlines(), 1):
                line = raw_line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                name, value = (part.strip() for part in line.split("=", 1))
                candidates.append((name, value, number))
        for name, value, position in candidates:
            if not re.fullmatch(r"[A-Za-z0-9_]+", name):
                continue
            if not re.fullmatch(r"[0-9a-fA-F]{2,}", value) or len(value) % 2:
                if html:
                    continue
                # 页面/注释周边文字不应导致纯文本键文件失败；识别到密钥名时给出安全错误。
                if re.search(r"(?:key|source|kek)", name, re.I):
                    raise AssetError(f"密钥文件第 {position} 处编码无效")
                continue
            key, data = name.lower(), bytes.fromhex(value)
            if key in result and result[key] != data:
                raise AssetError("密钥文件之间存在冲突")
            result[key] = data
            found += 1
        require(found > 0, f"密钥文件中未找到可识别的键值: {path}")
    return result


@contextmanager
def normalized_keys(paths: list[Path]) -> Iterator[Path]:
    keys = read_keys(paths)
    with tempfile.TemporaryDirectory(prefix="acnh-keys-") as temp:
        target = Path(temp) / "prod.keys"
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            for name, value in sorted(keys.items()):
                stream.write(f"{name} = {value.hex()}\n")
        yield target


def _safe_input(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve(strict=True)
    require(resolved.is_file(), f"{label} 必须是普通文件")
    return resolved


def _new_output(path: Path) -> Path:
    target = path.expanduser()
    if not target.is_absolute():
        target = Path.cwd() / target
    # 检查从目标一路到根目录的每一级，含目标本身和悬空符号链接。
    cursor = target
    while True:
        require(not cursor.is_symlink(), "输出路径不能包含符号链接")
        if cursor == cursor.parent:
            break
        cursor = cursor.parent
    require(not target.exists(), "输出已存在；请指定新的输出路径")
    return target


@contextmanager
def _staging(output: Path) -> Iterator[Path]:
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}.acnh-", dir=output.parent))
    try:
        yield stage
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _run_main(module, argv: list[str]) -> None:
    old_argv = sys.argv
    try:
        sys.argv = [module.__file__, *argv]
        module.main()
    finally:
        sys.argv = old_argv


def extract(base: Path, update: Path, keys: list[Path], output: Path) -> None:
    base, update = _safe_input(base, "基础包"), _safe_input(update, "更新包")
    output = _new_output(output)
    key_map = read_keys(keys)
    with upstream_modules(key_map) as (exefs, _, _), _staging(output) as stage:
        for label, source, is_update in (("base", base, False), ("update", update, True)):
            destination = stage / label
            destination.mkdir()
            args = [str(source), "--keys", str(keys[0]), "--output", str(destination / "exefs")]
            if is_update:
                args.append("--update")
            _run_main(exefs, args)


def romfs(manifest: Path, keys: list[Path], output: Path) -> None:
    manifest = _safe_input(manifest, "提取清单")
    output = _new_output(output)
    key_map = read_keys(keys)
    with upstream_modules(key_map) as (_, extractor, _), _staging(output) as stage:
        result = stage / "romfs"
        _run_main(extractor, ["--manifest", str(manifest), "--keys", str(keys[0]), "--output", str(result)])
        for item in result.iterdir():
            item.rename(stage / item.name)
        result.rmdir()


def merge(base: Path, update: Path, keys: list[Path], hactool: Path, output: Path) -> None:
    base, update = _safe_input(base, "基础包"), _safe_input(update, "更新包")
    hactool = _safe_input(hactool, "hactool")
    output = _new_output(output)
    with normalized_keys(keys) as keyfile, upstream_modules() as (_, _, merger), _staging(output) as stage:
        result = stage / "merged"
        merger.merge(base, update, keyfile, hactool, result)
        for item in result.iterdir():
            item.rename(stage / item.name)
        result.rmdir()


def fonts(xci: Path, keys: list[Path], output: Path) -> None:
    xci = _safe_input(xci, "XCI")
    output = _new_output(output)
    key_map = read_keys(keys)
    with upstream_font_module(key_map) as extractor, _staging(output) as stage:
        result = stage / "fonts"
        try:
            with redirect_stdout(io.StringIO()):
                _run_main(extractor, ["--xci", str(xci), "--keys", str(keys[0]), "--output", str(result)])
        except ValueError as error:
            message = str(error).lower()
            if "font data missing" in message or "no system update partition" in message:
                raise AssetError("ACNH XCI 系统更新分区没有可用的共享字体 NCA") from None
            raise
        manifest_path = result / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for row in manifest.get("fonts", []):
            row["source_file"] = row["file"]
            row["file"] = f"{row['type']}-{row['name']}.bfttf"
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        for item in result.iterdir():
            item.rename(stage / item.name)
        result.rmdir()
    manifest = json.loads((output / "manifest.json").read_text())
    rows = manifest.get("fonts", [])
    print(json.dumps({"status": manifest.get("status"), "font_count": len(rows),
                      "fonts": [{"type": row["type"], "name": row["name"],
                                 "bfttf_bytes": row["bfttf_bytes"], "font_bytes": row["font_bytes"],
                                 "sha256": row["sha256"]} for row in rows]}, ensure_ascii=False))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("extract", help="提取基础包和更新包 ExeFS")
    p.add_argument("--base", required=True, type=Path)
    p.add_argument("--update", required=True, type=Path)
    p.add_argument("--keys", required=True, action="append", type=Path, help="可重复指定，例如 prod.keys 和 title.keys")
    p.add_argument("--output", required=True, type=Path)
    p = commands.add_parser("romfs", help="完整提取并验证基础包六层 IVFC")
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--keys", required=True, action="append", type=Path)
    p.add_argument("--output", required=True, type=Path)
    p = commands.add_parser("merge", help="通过 hactool BKTR 合并基础包与更新包 RomFS")
    p.add_argument("--base", required=True, type=Path)
    p.add_argument("--update", required=True, type=Path)
    p.add_argument("--keys", required=True, action="append", type=Path)
    p.add_argument("--hactool", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p = commands.add_parser("fonts", help="从本地 XCI 的系统更新分区验证并提取共享字体")
    p.add_argument("--xci", type=Path, default=REPO_ROOT / "Animal Crossing New Horizons.xci")
    p.add_argument("--keys", required=True, action="append", type=Path, help="可重复指定，例如 prod.keys 和 title.keys")
    p.add_argument("--output", type=Path, default=REPO_ROOT / "work/acnh/3.0.0/shared-fonts")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "extract":
            extract(args.base, args.update, args.keys, args.output)
        elif args.command == "romfs":
            romfs(args.manifest, args.keys, args.output)
        elif args.command == "fonts":
            fonts(args.xci, args.keys, args.output)
        else:
            merge(args.base, args.update, args.keys, args.hactool, args.output)
    except (AssetError, OSError, RuntimeError, ValueError) as error:
        print(f"错误: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
