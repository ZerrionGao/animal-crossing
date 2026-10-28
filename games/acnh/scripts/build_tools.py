#!/usr/bin/env python3
"""在本项目 work 目录构建固定版本、带边界检查的 BKTR 工具。"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from hactool import HACTOOL, build_hactool

ROOT = Path(__file__).resolve().parents[3]


def prepare_shaders():
    cross = shutil.which('spirv-cross')
    if cross is None:
        raise RuntimeError('缺少 spirv-cross；请先提供该工具')
    base = ROOT / 'work/acnh/3.0.0'
    translator = base / 'build/shader-service/totk_xlate'
    translator.parent.mkdir(parents=True, exist_ok=True)
    script = ROOT / 'engine/tools/shader-translator/publish_native.sh'
    result = subprocess.run([str(script), str(translator)], cwd=ROOT)
    if result.returncode:
        raise RuntimeError(f'着色器翻译器构建失败，退出码 {result.returncode}')
    files = {translator: translator, Path(cross): base / 'build/spirv-cross/spirv-cross'}
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
    work = ROOT / 'work/acnh/toolchain'
    work.mkdir(parents=True, exist_ok=True)
    binary = build_hactool(work, work / 'logs')
    module_path = ROOT / 'games/acnh/scripts/hactool.py'
    report = {
        'source': {'repository': HACTOOL[0], 'commit': HACTOOL[1]},
        'patch_script': str(module_path.relative_to(ROOT)),
        'patch_script_sha256': hashlib.sha256(module_path.read_bytes()).hexdigest(),
        'binary': str(binary.relative_to(ROOT)),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    }
    (work / 'hactool-build.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
