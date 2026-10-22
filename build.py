#!/usr/bin/env python3
"""ACNH 3.0.0 Mac 原型：提取、合并、改写与验证的独立入口。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
SCRIPTS = ROOT / 'games/acnh/scripts'
WORK = ROOT / 'work/acnh'
PROFILE_PATH = ROOT / 'games/acnh/profiles/3.0.0.json'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def execute(command, log=None, environment=None):
    print('执行：' + ' '.join(map(str, command)), flush=True)
    environment = dict(os.environ if environment is None else environment, PYTHONOPTIMIZE='0')
    if log is None:
        result = subprocess.run(list(map(str, command)), cwd=ROOT, env=environment)
    else:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('w') as stream:
            result = subprocess.run(list(map(str, command)), cwd=ROOT, env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT)
    require(result.returncode == 0, f'步骤失败，退出码 {result.returncode}' + (f'；日志：{log}' if log else ''))


def profile():
    return json.loads(PROFILE_PATH.read_text())


def validate_exefs():
    for label in ('base', 'update'):
        folder = WORK / 'input' / label
        manifest = json.loads((folder / 'extraction-manifest.json').read_text())
        require(manifest['program']['title_id'] == '01006F8002326000', 'ExeFS 标题不匹配')
        for record in manifest['files']:
            name = record['name']
            require(Path(name).name == name and name not in ('.', '..'), 'ExeFS 清单含非法路径')
            path = folder / 'exefs' / name
            require(path.is_file() and path.stat().st_size == record['size'] and digest(path) == record['sha256'],
                    f'ExeFS 文件与已校验提取清单不一致：{label}/{name}')
    for name, expected in profile()['modules'].items():
        require(digest(WORK / 'input/update/exefs' / name) == expected['sha256'], f'不是已锁定的 3.0.0 模块：{name}')


def validate_romfs():
    folder = WORK / '3.0.0/romfs'
    report = json.loads((folder / 'index.json').read_text())
    expected = profile()['romfs']
    require(report['status'] == 'pass' and 'six IVFC' in report['integrity'], 'RomFS 未通过六层 IVFC 校验')
    require(len(report['files']) == expected['files'], 'RomFS 文件数量不匹配')
    require(digest(folder / 'romfs.bin') == report['romfs_sha256'] == expected['sha256'], '不是已锁定的 3.0.0 RomFS')


def validate_fonts():
    folder = WORK / '3.0.0/shared-fonts'
    report = json.loads((folder / 'manifest.json').read_text())
    require(report.get('status') == 'pass', '共享字体清单未通过')
    require({row['type'] for row in report['fonts']} == set(range(6)), '共享字体不完整')
    expected = json.loads((ROOT / 'games/acnh/profiles/shared-fonts.json').read_text())
    fixed = {row['type']: row for row in expected['fonts']}
    for row in report['fonts']:
        require(all(row.get(key) == value for key, value in fixed[row['type']].items()), '共享字体与已核验样本不匹配')
        name = row['file']
        require(not Path(name).is_absolute() and '..' not in Path(name).parts, '字体清单路径无效')
        require(digest(folder / name) == row['sha256'], f'共享字体哈希不匹配：{name}')


def prepare(args):
    key_args = [item for key in args.keys for item in ('--keys', key)]
    if not (WORK / 'input').exists():
        execute([sys.executable, SCRIPTS / 'assets.py', 'extract', '--base', args.base,
                 '--update', args.update, *key_args, '--output', WORK / 'input'])
    validate_exefs()
    if not (WORK / '3.0.0/romfs').exists():
        execute([sys.executable, SCRIPTS / 'build_tools.py'])
        execute([sys.executable, SCRIPTS / 'assets.py', 'merge', '--base', args.base,
                 '--update', args.update, *key_args, '--hactool', WORK / 'toolchain/src/hactool/hactool',
                 '--output', WORK / '3.0.0/romfs'])
    validate_romfs()
    if not (WORK / '3.0.0/shared-fonts').exists():
        execute([sys.executable, SCRIPTS / 'assets.py', 'fonts', '--xci', args.base, *key_args,
                 '--output', WORK / '3.0.0/shared-fonts'])
    validate_fonts()
    print('3.0.0 输入校验通过。', flush=True)


def rewrite(args):
    validate_exefs()
    execute([sys.executable, SCRIPTS / 'rewrite.py', '--exefs', WORK / 'input/update/exefs'])


def verify(args):
    rewritten = WORK / '3.0.0/rewritten'
    marker = WORK / '3.0.0/verification.json'
    marker.unlink(missing_ok=True)
    manifest = json.loads((rewritten / 'manifest.json').read_text())
    require(manifest['status'] == 'pass' and manifest['cuts'] == 0 and manifest['observations'] == 0,
            '改写清单未通过或包含未授权游戏补丁')
    vendor = ROOT / 'third_party/macnativegames/games/totk/third_party/d2r'
    tools = [vendor / name for name in ('patch_nso.py', 'dynsym.py', 'call_window.py', 'relr.py')]
    tools += [SCRIPTS / 'rewrite.py', PROFILE_PATH]
    require(set(manifest['tools']) == {str(path.relative_to(ROOT)) for path in tools}, '改写工具/配置清单不匹配')
    require(manifest.get('python_optimization') == 0, '改写清单没有确认启用 Python 安全断言')
    for path in tools:
        relative = str(path.relative_to(ROOT))
        require(manifest['tools'].get(relative) == digest(path), f'改写器或配置指纹改变：{relative}')
    env = dict(os.environ, LLVM_BIN=args.llvm_bin, DM_VCACHE=str(WORK / '3.0.0/verification/llvm-cache'))
    results = {}
    for name in ('main', 'subsdk0', 'sdk'):
        source = WORK / 'input/update/exefs' / name
        require(digest(source) == profile()['modules'][name]['sha256'], f'{name} 输入改变')
        for filename, metadata in manifest['modules'][name]['artifacts'].items():
            require(Path(filename).name == filename, '改写清单产物路径无效')
            path = rewritten / filename
            require(path.stat().st_size == metadata['size'] and digest(path) == metadata['sha256'], f'改写产物改变：{filename}')
        logs = WORK / '3.0.0/verification'
        structural = logs / f'{name}-structure.log'
        semantic = logs / f'{name}-equivalence.log'
        execute([sys.executable, vendor / 'structck.py', rewritten, name, source], structural, env)
        execute([sys.executable, vendor / 'equiv.py', rewritten, name, source, '--jobs', str(args.jobs),
                 '--min-eff', '2', '--max-extra', '16', '--mutate', '150', '--tsv', logs / f'{name}-equivalence.tsv'], semantic, env)
        require(re.search(rf'^STRUCT {name} OK ', structural.read_text(), re.M), f'{name} 缺少结构校验成功记录')
        match = re.search(rf'^EQUIV {name} OK .*变异=(\d+).*抓到=(\d+) 漏抓=0', semantic.read_text(), re.M)
        require(match and int(match[1]) > 0 and match[1] == match[2], f'{name} 等价校验或负向对照未通过')
        results[name] = {'structure_log_sha256': digest(structural), 'equivalence_log_sha256': digest(semantic)}
    marker.write_text(json.dumps({'status': 'pass', 'python_optimization': 0, 'rewrite_manifest_sha256': digest(rewritten / 'manifest.json'),
                                  'modules': results}, ensure_ascii=False, indent=2) + '\n')
    print('三模块结构与差分验证通过；这不等于游戏运行验收。', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('step', choices=('prepare', 'rewrite', 'verify', 'runtime', 'probe', 'boot'))
    parser.add_argument('--base', type=Path, default=ROOT / 'Animal Crossing New Horizons.xci')
    parser.add_argument('--update', type=Path, default=ROOT / 'Animal Crossing New Horizons v3.0.0.nsp')
    parser.add_argument('--keys', action='append', type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--llvm-bin', default='/opt/homebrew/opt/llvm/bin')
    args = parser.parse_args()
    if args.keys is None:
        args.keys = [ROOT / 'prod.keys', ROOT / 'title.keys']
    require(args.jobs > 0, '--jobs 必须为正数')
    if args.step in ('prepare', 'rewrite', 'verify'):
        globals()[args.step](args)
    else:
        if args.step == 'boot':
            validate_exefs()
            validate_romfs()
            validate_fonts()
            execute([sys.executable, SCRIPTS / 'build_tools.py', '--shader-tools-only'])
        # boot 由独立目标核对验证清单后启动；runtime/probe 不进入完整游戏。
        environment = None
        if args.step == 'boot':
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(('TOTK_', 'DM_'))}
        execute(['make', '-C', ROOT / 'games/acnh', args.step], environment=environment)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f'错误：{error}', file=sys.stderr)
        raise SystemExit(1)
