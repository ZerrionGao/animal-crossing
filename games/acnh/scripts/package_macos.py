#!/usr/bin/env python3
"""把已校验的 ACNH 3.0.0 打成本机离线 Mac App 原型。不含 keys、XCI/NSP，不签名上架。"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
VENDOR_RUNTIME = ROOT / 'engine/runtime'
VENDOR_NVN = ROOT / 'engine/graphics/nvn'
PROFILE_PATH = ROOT / 'games/acnh/profiles/3.0.0.json'
APP_NAME = 'Animal Crossing New Horizons'
BUNDLE_ID = 'org.macnativegames.acnh'
EXECUTABLE = 'ACNH'
DISPLAY_NAME = '集合啦！动物森友会'
FORBIDDEN_NAMES = {
    'prod.keys', 'title.keys', 'prod.keys.txt', 'title.keys.txt',
}
FORBIDDEN_SUFFIXES = ('.xci', '.nsp', '.tik', '.cert')
USER_PROFILE_KEEP = (
    'account.uid', 'cache', 'device-save', 'pseudo-device-id', 'pso_user.tsv', 'save',
)

_libc = None


class PackageError(Exception):
    pass


def say(message: str) -> None:
    print(f'[{time.strftime("%H:%M:%S")}] {message}', flush=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PackageError(message)


def clonefile(src: Path, dst: Path) -> bool:
    global _libc
    if _libc is None:
        _libc = ctypes.CDLL(None, use_errno=True)
        _libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
    return _libc.clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0


def clone(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() or dst.is_symlink():
        if dst.is_dir() and not dst.is_symlink():
            shutil.rmtree(dst)
        else:
            dst.unlink()
    if not clonefile(src, dst):
        if src.is_dir():
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)


def remove_tree(path: Path) -> None:
    for _ in range(5):
        try:
            shutil.rmtree(path)
            return
        except OSError as error:
            if error.errno != 66 or not path.exists():
                raise
            time.sleep(0.2)
    shutil.rmtree(path)


def run(command: list, log: Path, cwd: Path | None = None) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as stream:
        result = subprocess.run([str(item) for item in command], cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = '\n'.join(log.read_text(errors='replace').splitlines()[-20:])
        raise PackageError(f'{command[0]} 失败，日志 {log}\n{tail}')


def profile() -> dict:
    return json.loads(PROFILE_PATH.read_text())


def work_dir(args: argparse.Namespace) -> Path:
    return Path(args.work)


def assert_inputs(work: Path) -> dict:
    expected = profile()
    binary = work / 'build/acnh-native-boot'
    require(binary.is_file() and os.access(binary, os.X_OK), f'缺少启动二进制：{binary}')
    rewritten = work / 'rewritten'
    for name in ('main', 'sdk', 'subsdk0'):
        path = rewritten / f'{name}.patched.bin'
        require(path.is_file(), f'缺少改写模块：{path}')
    romfs = work / 'romfs/romfs.bin'
    require(romfs.is_file(), f'缺少 RomFS：{romfs}')
    require(sha256(romfs) == expected['romfs']['sha256'], 'RomFS 不是已锁定的 3.0.0 镜像')
    require((work / 'romfs/runtime-index.bin').is_file(), '缺少 RomFS runtime-index.bin')
    fonts = work / 'shared-fonts/manifest.json'
    require(fonts.is_file() and json.loads(fonts.read_text()).get('status') == 'pass', '共享字体清单未通过')
    ngc = Path(args_ngc(work))
    require((ngc / 'ac_common_not_b_nx').is_file(), f'缺少 NgWord2：{ngc}')
    require((work / 'build/shader-service/totk_xlate').is_file(), '缺少 shader 翻译器')
    require((work / 'build/spirv-cross/spirv-cross').is_file(), '缺少 spirv-cross')
    return expected


def args_ngc(work: Path) -> Path:
    fallback = ROOT / 'work/research/ngword2-from-xci-20261023/romfs'
    env = os.environ.get('TOTK_NGC_DIR')
    if env:
        return Path(env)
    for candidate in (work / 'ngc', fallback):
        if (candidate / 'ac_common_not_b_nx').is_file():
            return candidate
    return fallback


def compile_launcher(output: Path, log: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    run([
        'xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror',
        '-DGAME_ACNH',
        '-I', str(VENDOR_RUNTIME),
        '-I', str(VENDOR_NVN),
        ROOT / 'games/acnh/app/launcher.m',
        '-framework', 'AppKit',
        '-o', output,
    ], log)


def extract_icon(output: Path, log: Path) -> bool:
    nsp = ROOT / 'Animal Crossing New Horizons v3.0.0.nsp'
    keys = ROOT / 'prod.keys'
    script = ROOT / 'engine/tools/game-data-extractors/extract_app_icon.py'
    if not nsp.is_file() or not keys.is_file() or not script.is_file():
        return False
    env = dict(os.environ)
    env['PYTHONPATH'] = str(script.parent)
    with log.open('w') as stream:
        result = subprocess.run(
            [sys.executable, str(script), str(nsp), '--keys', str(keys),
             '--title', '01006F8002326000', '--output', str(output)],
            cwd=str(script.parent), env=env, stdout=stream, stderr=subprocess.STDOUT)
    return result.returncode == 0 and output.is_file()


def seed_user_data(work: Path) -> None:
    home = Path.home() / 'Library/Application Support/MacNativeGames/ACNH'
    profile_dst = home / 'profile'
    cache_dst = home / 'shader-cache'
    source_profile = work / 'test-profile'
    source_cache = work / 'shader-cache'
    if not profile_dst.exists() and source_profile.is_dir():
        say('复制本机已有存档到用户目录（仅首次）')
        profile_dst.parent.mkdir(parents=True, exist_ok=True)
        profile_dst.mkdir(parents=True, exist_ok=True)
        for name in USER_PROFILE_KEEP:
            src = source_profile / name
            if src.exists():
                clone(src, profile_dst / name)
    if not cache_dst.exists() and source_cache.is_dir():
        say('复制本机着色器缓存到用户目录（仅首次）')
        clone(source_cache, cache_dst)


def scan_forbidden(app: Path) -> None:
    for path in app.rglob('*'):
        if not path.is_file():
            continue
        name = path.name.lower()
        require(name not in FORBIDDEN_NAMES, f'App 内出现禁止文件：{path.name}')
        require(not name.endswith(FORBIDDEN_SUFFIXES), f'App 内出现禁止后缀：{path.name}')


def data_bundle(work: Path, logs: Path) -> Path:
    """把 10 GB 的 romfs.bin 做成单独签名的 bundle，App 密封只记录其签名。"""
    image = work / 'romfs/romfs.bin'
    bundle = work / 'build/data-bundles/GameData-3.0.0.bundle'
    stamp = bundle.with_name(bundle.name + '.json')
    st = image.stat()
    want = dict(format=1, size=st.st_size, mtime_ns=st.st_mtime_ns, inode=st.st_ino)
    try:
        if json.loads(stamp.read_text()) == want and (bundle / 'Contents/_CodeSignature/CodeResources').is_file():
            return bundle
    except (OSError, ValueError):
        pass
    say('签名游戏数据（首次约一两分钟，之后复用）')
    tmp = bundle.with_name(bundle.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    clone(image, tmp / 'Contents/Resources/romfs.bin')
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier=f'{BUNDLE_ID}.data', CFBundleName='Game data', CFBundlePackageType='BNDL',
        CFBundleShortVersionString=profile()['display_version'], CFBundleVersion='1')))
    run(['codesign', '--force', '--sign', '-', str(tmp)], logs / 'sign-data.log')
    stamp.unlink(missing_ok=True)
    if bundle.exists():
        shutil.rmtree(bundle)
    tmp.rename(bundle)
    write_stamp = stamp.with_name(stamp.name + '.tmp')
    write_stamp.write_text(json.dumps(want))
    write_stamp.replace(stamp)
    return bundle


def place_data_bundle(bundle: Path, app: Path, res: Path) -> None:
    helper = app / 'Contents/Helpers' / bundle.name
    clone(bundle, helper)
    romfs = res / 'romfs'
    romfs.mkdir(parents=True, exist_ok=True)
    link = romfs / 'romfs.bin'
    if link.exists() or link.is_symlink():
        link.unlink()
    link.symlink_to(os.path.relpath(helper / 'Contents/Resources/romfs.bin', link.parent))


def place_resources(work: Path, res: Path, app: Path, logs: Path) -> None:
    clone(work / 'build/acnh-native-boot', res / 'build/acnh-native-boot')
    clone(work / 'rewritten', res / 'rewritten')
    place_data_bundle(data_bundle(work, logs), app, res)
    for name in ('runtime-index.bin', '.romfs-verified-v1'):
        src = work / 'romfs' / name
        if src.is_file():
            clone(src, res / 'romfs' / name)
    clone(work / 'shared-fonts', res / 'shared-fonts')
    clone(args_ngc(work), res / 'ngc')
    clone(work / 'build/shader-service/totk_xlate', res / 'build/shader-service/totk_xlate')
    clone(work / 'build/spirv-cross/spirv-cross', res / 'build/spirv-cross/spirv-cross')
    seed = work / 'build/shader-seed'
    if seed.is_dir() and any(seed.iterdir()):
        clone(seed, res / 'shader-seed')
    else:
        (res / 'shader-seed').mkdir(parents=True, exist_ok=True)


def write_plist(app: Path, expected: dict, icon: bool) -> None:
    info = dict(
        CFBundleDevelopmentRegion='zh-Hans',
        CFBundleExecutable=EXECUTABLE,
        CFBundleIdentifier=BUNDLE_ID,
        CFBundleName=APP_NAME,
        CFBundleDisplayName=DISPLAY_NAME,
        CFBundlePackageType='APPL',
        CFBundleShortVersionString=expected['display_version'],
        CFBundleVersion='1',
        LSMinimumSystemVersion='26.0',
        NSHighResolutionCapable=True,
        LSApplicationCategoryType='public.app-category.simulation-games',
        LSSupportsGameMode=True,
        GCSupportsControllerUserInteraction=True,
    )
    if icon:
        info['CFBundleIconFile'] = 'AppIcon'
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    (app / 'Contents/PkgInfo').write_bytes(b'APPL????')


def sign_app(app: Path, log_dir: Path) -> None:
    run(['codesign', '--force', '--sign', '-', str(app)], log_dir / 'sign-app.log')
    run(['codesign', '--verify', '--deep', '--strict', str(app)], log_dir / 'verify-app-signature.log')


def assemble(work: Path, out: Path, expected: dict, *, skip_icon: bool, skip_user_data: bool) -> Path:
    logs = work / 'package-logs'
    logs.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        remove_tree(tmp)
    res = tmp / 'Contents/Resources/acnh'
    say('编译启动器')
    compile_launcher(work / 'build/app-launcher', logs / 'launcher.log')
    say('复制已校验资源（APFS 克隆）')
    place_resources(work, res, tmp, logs)
    clone(work / 'build/app-launcher', tmp / f'Contents/MacOS/{EXECUTABLE}')
    have_icon = False
    if not skip_icon:
        say('尝试从本机更新包提取图标')
        cached_icon = work / 'build/AppIcon.icns'
        have_icon = extract_icon(cached_icon, logs / 'app-icon.log')
        if not have_icon and cached_icon.is_file() and cached_icon.stat().st_size > 0:
            say('复用已有图标')
            have_icon = True
        if have_icon:
            clone(cached_icon, tmp / 'Contents/Resources/AppIcon.icns')
        else:
            say('无图标，使用系统默认')
    write_plist(tmp, expected, have_icon)
    scan_forbidden(tmp)
    say('Ad-hoc 签名')
    sign_app(tmp, logs)
    if out.exists():
        remove_tree(out)
    tmp.rename(out)
    if not skip_user_data:
        seed_user_data(work)
    check = subprocess.run([str(out / f'Contents/MacOS/{EXECUTABLE}'), '--check'], capture_output=True, text=True, timeout=120)
    require(check.returncode == 0 and check.stdout.startswith('ok:'), f'--check 失败：{check.stdout}{check.stderr}')
    say(f'完成：{out}')
    say(f'存档目录：{Path.home() / "Library/Application Support/MacNativeGames/ACNH/profile"}')
    say(f'日志：{Path.home() / "Library/Logs/MacNativeGames/acnh.log"}')
    return out


def package(args: argparse.Namespace) -> Path:
    work = work_dir(args)
    expected = assert_inputs(work)
    return assemble(work, Path(args.output), expected, skip_icon=args.skip_icon, skip_user_data=args.skip_user_data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'work/acnh/3.0.0')
    parser.add_argument('--output', type=Path, default=ROOT / 'work/acnh/3.0.0/Animal Crossing New Horizons.app')
    parser.add_argument('--skip-user-data', action='store_true', help='不向用户目录复制已有存档/着色器缓存')
    parser.add_argument('--skip-icon', action='store_true')
    args = parser.parse_args()
    package(args)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, PackageError) as error:
        print(f'错误：{error}', file=sys.stderr)
        raise SystemExit(1)
