#!/usr/bin/env python3
"""在本项目 work 目录构建固定版本、带边界检查的 BKTR 工具。"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def prepare_shaders():
    source = ROOT / 'third_party/macnativegames/games/totk/prebuilt/macos-arm64/totk_xlate'
    expected = source.with_name('totk_xlate.sha256').read_text().strip()
    if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
        raise RuntimeError('内置 shader 翻译器哈希不匹配')
    cross = shutil.which('spirv-cross')
    if cross is None:
        raise RuntimeError('缺少 spirv-cross；请先提供该工具')
    base = ROOT / 'work/acnh/3.0.0'
    files = {source: base / 'build/shader-service/totk_xlate',
             Path(cross): base / 'build/spirv-cross/spirv-cross'}
    report = {}
    for original, destination in files.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix='.tool-', dir=destination.parent)
        os.close(fd)
        try:
            shutil.copy2(original, temporary)
            os.replace(temporary, destination)
        finally:
            Path(temporary).unlink(missing_ok=True)
        report[str(destination.relative_to(ROOT))] = hashlib.sha256(destination.read_bytes()).hexdigest()
    (base / 'shader-tools.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'shader_tools': report}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shader-tools-only', action='store_true', help='准备本机 shader 服务，不重建 hactool')
    args = parser.parse_args()
    if args.shader_tools_only:
        prepare_shaders()
        return
    module_path = ROOT / 'third_party/macnativegames/build_app.py'
    spec = importlib.util.spec_from_file_location('mng_build_tools', module_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    work = ROOT / 'work/acnh/toolchain'
    work.mkdir(parents=True, exist_ok=True)
    binary = module.build_hactool(work / 'tree', work / 'logs')
    report = {
        'upstream': {'repository': module.HACTOOL[0], 'commit': module.HACTOOL[1]},
        'patch_provider': 'MacNativeGames dfa7ec426f8b37798073a108ede1504c827d8af0 build_hactool',
        'patch_provider_sha256': hashlib.sha256(module_path.read_bytes()).hexdigest(),
        'binary': str(binary.relative_to(ROOT)),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    }
    (work / 'hactool-build.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
