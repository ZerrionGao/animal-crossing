#!/usr/bin/env python3
"""构建并签名 ACNH 3.0.0 的 iPhone/iPad 本地 IPA。"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[4]
WORK = ROOT / 'work/acnh/3.0.0'
VENDOR = ROOT / 'engine'
IOS = ROOT / 'games/acnh/ios'
BUNDLE_ID = 'org.macnativegames.acnh'
IDENTITY_RE = re.compile(r'^\s*\d+\) ([0-9A-F]{40}) "Apple Development: .* \(([A-Z0-9]+)\)"$')
FILE_RE = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib))$')


def run(args: list[str | Path], log: Path, *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as stream:
        result = subprocess.run([str(item) for item in args], cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = '\n'.join(log.read_text(errors='replace').splitlines()[-45:])
        raise RuntimeError(f'{args[0]} 失败，日志 {log}\n{tail}')


def copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    result = subprocess.run(['/bin/cp', '-c', str(src), str(dst)], capture_output=True)
    if result.returncode:
        shutil.copy2(src, dst)


def copy_tree(src: Path, dst: Path) -> None:
    if not src.is_dir():
        raise RuntimeError(f'缺少打包资源目录：{src}')
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(['/bin/cp', '-cR', str(src) + os.sep + '.', str(dst)], capture_output=True)
    if result.returncode:
        dst.mkdir(parents=True)
        for path in src.rglob('*'):
            target = dst / path.relative_to(src)
            if path.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            elif path.is_file():
                copy_file(path, target)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def prepare_app_icon(gen: Path, logs: Path) -> None:
    source = WORK / 'build/AppIcon.icns'
    if not source.is_file():
        raise RuntimeError(f'缺少本机游戏图标：{source}，请先执行 Mac 打包的图标提取步骤')
    catalog = gen / 'AppAssets.xcassets'
    icon = catalog / 'AppIcon.appiconset'
    icon.mkdir(parents=True, exist_ok=True)
    info = {'version': 1, 'author': 'xcode'}
    (catalog / 'Contents.json').write_text(json.dumps({'info': info}, indent=2) + '\n')
    # JPEG 中转去掉 ICNS 的透明通道，由 iOS 统一处理图标圆角。
    with tempfile.TemporaryDirectory(prefix='acnh-icon-') as temp:
        opaque = Path(temp) / 'icon.jpg'
        run(['sips', '-s', 'format', 'jpeg', '-z', '1024', '1024', source, '--out', opaque], logs / 'icon-decode.log')
        run(['sips', '-s', 'format', 'png', opaque, '--out', icon / 'AppIcon.png'], logs / 'icon-png.log')
    (icon / 'Contents.json').write_text(json.dumps({
        'images': [{'filename': 'AppIcon.png', 'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}],
        'info': info,
    }, indent=2) + '\n')


def compile_seed(seed: Path, normalizer: Path) -> None:
    policy = subprocess.run([str(normalizer), '--policy-id=acnh-metal-v1', '--metal-flags'],
                            capture_output=True, text=True, check=True).stdout.strip().split()
    if policy != ['-fmetal-math-mode=safe', '-fpreserve-invariance']:
        raise RuntimeError(f'ACNH Metal 编译参数不符合已验证策略：{policy}')
    files = sorted(seed.glob('*_b.metal'))
    if not files:
        raise RuntimeError('着色器种子里没有 MSL 源码')

    def compile_one(source: Path) -> Path:
        air = seed / (source.name + '.air')
        library = seed / (source.name.removesuffix('.metal') + '.metallib')
        with tempfile.TemporaryDirectory(prefix='acnh-msl-') as temp:
            normalized = Path(temp) / source.name
            normalized_result = subprocess.run([str(normalizer), '--policy-id=acnh-metal-v1', str(source), str(normalized)],
                                                capture_output=True, text=True)
            if normalized_result.returncode:
                raise RuntimeError(f'MSL 规范化失败：{source.name}\n{normalized_result.stderr[-3000:]}')
            result = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal', *policy, '-c', str(normalized), '-o', str(air)],
                                    capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'Metal 编译失败：{source.name}\n{result.stderr[-3000:]}')
        result = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metallib', str(air), '-o', str(library)],
                                capture_output=True, text=True)
        air.unlink(missing_ok=True)
        if result.returncode:
            raise RuntimeError(f'metallib 链接失败：{source.name}\n{result.stderr[-3000:]}')
        return library

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for index, _ in enumerate(pool.map(compile_one, files), 1):
            if index % 100 == 0 or index == len(files):
                print(f'已编译 {index}/{len(files)} 个 ACNH iOS Metal 库', flush=True)
    manifest_path = seed / 'seed-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['ios_metallibs'] = len(files)
    manifest['ios_metal_flags'] = ' '.join(policy)
    manifest['files'] = {name: sha256(seed / name) for name in sorted(manifest['files'])}
    manifest['files'].update({path.name: sha256(path) for path in sorted(seed.glob('*_b.metallib'))})
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')


def development_identity(preferred_team: str | None = None) -> tuple[str, str]:
    result = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'], capture_output=True, text=True, check=True)
    identities = []
    for line in result.stdout.splitlines():
        match = IDENTITY_RE.match(line)
        if match:
            identities.append((match.group(1), match.group(2)))
    if preferred_team is None and len({team for _, team in identities}) > 1:
        raise RuntimeError('检测到多个 Apple Development 团队，请通过 --team-id 明确选择')
    if preferred_team is not None:
        identities = [identity for identity in identities if identity[1] == preferred_team]
    if identities:
        return identities[0]
    raise RuntimeError(f'钥匙串里没有团队 {preferred_team or "(任意)"} 的 Apple Development 签名身份')


def signed_identity_sha1(app: Path) -> str:
    identities = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'],
                                capture_output=True, text=True, check=True)
    signature = subprocess.run(['codesign', '-dv', '--verbose=4', app],
                               capture_output=True, text=True, check=True)
    authority = next((line.removeprefix('Authority=') for line in signature.stderr.splitlines()
                      if line.startswith('Authority=Apple Development:')), None)
    if not authority:
        raise RuntimeError('签名产物没有 Apple Development 身份')
    for line in identities.stdout.splitlines():
        match = IDENTITY_RE.match(line)
        if match and f'"{authority}"' in line:
            return match.group(1)
    raise RuntimeError(f'钥匙串中找不到签名产物使用的身份：{authority}')


def prepare_seed(destination: Path, normalizer: Path, log_dir: Path) -> int:
    source = Path.home() / 'Library/Application Support/MacNativeGames/ACNH/shader-cache'
    if not source.is_dir():
        source = WORK / 'shader-cache'
    if not source.is_dir():
        raise RuntimeError('找不到已运行的 ACNH 着色器缓存，无法准备 iPhone 首轮着色器资源')
    header = (VENDOR / 'graphics/nvn/dmsh_chain.h').read_text()
    chain_match = re.search(r'#define DMSH_CHAIN "([^"]+)"', header)
    if not chain_match:
        raise RuntimeError('无法读取当前着色器翻译链标识')
    chain = chain_match.group(1)
    source_hash = hashlib.sha256()
    for index in sorted(source.glob('*.variants')):
        try:
            rows = json.loads(index.read_text())
        except (OSError, ValueError):
            continue
        for row in rows if isinstance(rows, list) else []:
            variant = row.get('variant') if isinstance(row, dict) else None
            if not isinstance(variant, str) or not re.fullmatch(r'[0-9a-f]{64}-[0-9a-f]{12}', variant):
                continue
            if not all((source / f'{variant}_b.{suffix}').is_file() for suffix in ('metal', 'json')):
                continue
            for path in (index, source / f'{variant}_b.metal', source / f'{variant}_b.json'):
                source_hash.update(path.name.encode())
                source_hash.update(bytes.fromhex(sha256(path)))
    source_fingerprint = source_hash.hexdigest()
    old_manifest_path = destination / 'seed-manifest.json'
    try:
        old_manifest = json.loads(old_manifest_path.read_text())
        old_files = old_manifest['files']
        valid = (old_manifest.get('acnh_source_cache_sha256') == source_fingerprint and
                 old_manifest.get('chain') == chain and old_manifest.get('shader_policy') == 'acnh-metal-v1' and
                 old_manifest.get('ios_metallibs', 0) == old_manifest.get('shaders') and
                 all((destination / name).is_file() and sha256(destination / name) == digest
                     for name, digest in old_files.items()) and
                 len(list((destination / f'b-{chain}').glob('*_b.metallib'))) == old_manifest.get('shaders'))
        if valid:
            return int(old_manifest['shaders'])
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    copied = 0
    for index in source.glob('*.variants'):
        try:
            rows = json.loads(index.read_text())
        except (OSError, ValueError):
            continue
        valid_rows = []
        for row in rows if isinstance(rows, list) else []:
            variant = row.get('variant') if isinstance(row, dict) else None
            if not isinstance(variant, str) or not re.fullmatch(r'[0-9a-f]{64}-[0-9a-f]{12}', variant):
                continue
            names = [f'{variant}_b.metal', f'{variant}_b.json']
            if not all((source / name).is_file() for name in names):
                continue
            for name in names:
                copy_file(source / name, destination / name)
            valid_rows.append(row)
        if valid_rows:
            (destination / index.name).write_text(json.dumps(valid_rows, sort_keys=True, separators=(',', ':')))
            copied += len(valid_rows)
    if copied == 0:
        raise RuntimeError('ACNH 着色器缓存中没有可封装的完整变体')
    files = {p.name: sha256(p) for p in sorted(destination.iterdir()) if p.is_file()}
    (destination / 'seed-manifest.json').write_text(json.dumps({
        'schema': 1, 'chain': chain, 'shaders': copied, 'shader_policy': 'acnh-metal-v1', 'files': files,
    }, ensure_ascii=False, indent=2) + '\n')
    compile_seed(destination, normalizer)
    metal_dir = destination / f'b-{chain}'
    metal_dir.mkdir()
    for library in destination.glob('*_b.metallib'):
        copy_file(library, metal_dir / library.name)
    (destination / 'shader_map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    # Include the shader map in the seed manifest only when the runtime's accepted filename set allows it.
    manifest_path = destination / 'seed-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['acnh_source_cache_sha256'] = source_fingerprint
    manifest['files'] = {p.name: sha256(p) for p in sorted(destination.iterdir())
                         if p.is_file() and p.name != 'seed-manifest.json' and FILE_RE.fullmatch(p.name)}
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    return copied


def package(args: argparse.Namespace) -> Path:
    output = args.output.resolve()
    logs = WORK / 'ios-package-logs'
    build_dir = WORK / 'build/ios-derived'
    gen = IOS / 'Gen'
    resources = gen / 'Resources/GameData'
    gen.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    prepare_app_icon(gen, logs)

    # Mac 端的预检会核对锁定的游戏版本、RomFS 哈希、字体和本地账户词库。
    run([ROOT / '.venv/bin/python', ROOT / 'games/acnh/scripts/prepare_runtime.py', 'tables', '--output', WORK / 'build/runtime_tables.h'],
        logs / 'runtime-tables.log')
    run([ROOT / '.venv/bin/python', ROOT / 'games/acnh/scripts/prepare_runtime.py', 'fonts-config'], logs / 'fonts-config.log')
    host_target = Path(os.path.relpath(WORK / 'build/acnh-native-boot', ROOT / 'games/acnh'))
    run(['make', '-C', ROOT / 'games/acnh', host_target], logs / 'mac-host-build.log')

    ngc = WORK / 'ngc'
    if not (ngc / 'ac_common_not_b_nx').is_file():
        ngc = ROOT / 'work/research/ngword2-from-xci-20261023/romfs'
    for required in (WORK / 'romfs/romfs.bin', WORK / 'romfs/runtime-index.bin', ngc / 'ac_common_not_b_nx'):
        if not required.is_file():
            raise RuntimeError(f'缺少已验证的打包输入：{required}')
    expected_hash = json.loads((ROOT / 'games/acnh/profiles/3.0.0.json').read_text())['romfs']['sha256']
    if sha256(WORK / 'romfs/romfs.bin') != expected_hash:
        raise RuntimeError('RomFS 哈希与 ACNH 3.0.0 锁定版本不符')

    export_dir = WORK / 'build/ios-export'
    export_dir.mkdir(parents=True, exist_ok=True)
    host = WORK / 'build/acnh-native-boot'
    run([host, WORK / 'rewritten', 'export-ios'], logs / 'export-ios.log',
        env=dict(os.environ, TOTK_EXPORT_IOS=str(export_dir)))
    run([sys.executable, VENDOR / 'tools/gen_embed.py', WORK / 'rewritten', gen / 'embed.S',
         '--text-dir', export_dir, 'main', 'subsdk0', 'sdk'], logs / 'embed.log')
    embed_linker_flags = (logs / 'embed.log').read_text().split()
    if not embed_linker_flags or any(not flag.startswith('-Wl,-segprot,') for flag in embed_linker_flags):
        raise RuntimeError('iOS 嵌入模块没有生成有效的段权限链接参数')
    for name in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h'):
        copy_file(WORK / 'build' / name, gen / name)

    deps = WORK / 'build/ios-deps'
    if not (deps / 'lib/libopus.a').is_file() or not (deps / 'lib/libvpx.a').is_file():
        run([sys.executable, IOS / 'tools/build_deps.py', deps], logs / 'ios-dependencies.log')
    copy_tree(deps / 'include', gen / 'deps/include')
    copy_file(deps / 'lib/libopus.a', gen / 'deps/lib/libopus.a')
    copy_file(deps / 'lib/libvpx.a', gen / 'deps/lib/libvpx.a')

    normalizer = WORK / 'build/acnh-msl-normalizer'
    if not normalizer.is_file():
        run(['make', '-C', ROOT / 'games/acnh', 'acnh-msl-normalizer'], logs / 'normalizer.log')
    seed = WORK / 'build/ios-shader-seed'
    shader_count = prepare_seed(seed, normalizer, logs)
    # 大体积只读资源置于 XcodeGen 的资源目录，签名在复制完成后由 Xcode 统一完成。
    if resources.exists():
        shutil.rmtree(resources)
    resources.parent.mkdir(parents=True, exist_ok=True)
    copy_tree(WORK / 'romfs', resources / 'romfs')
    copy_tree(WORK / 'shared-fonts', resources / 'shared-fonts')
    copy_tree(ngc, resources / 'ngc')
    copy_tree(seed, resources / 'shader-seed')

    xcconfig = gen / 'acnh.xcconfig'
    frameworks = ['UIKit', 'AVFAudio', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore', 'ImageIO', 'CoreGraphics',
                  'CoreText', 'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox', 'GameController',
                  'CoreHaptics', 'Security']
    ldflags = ' '.join(f'-framework {name}' for name in frameworks)
    ldflags += ' Gen/deps/lib/libopus.a Gen/deps/lib/libvpx.a ' + ' '.join(embed_linker_flags)
    xcconfig.write_text(f'OTHER_LDFLAGS = $(inherited) {ldflags}\n')
    run(['xcodegen', 'generate', '--spec', 'project.yml'], logs / 'xcodegen.log', cwd=IOS)
    env = dict(os.environ, DEVELOPER_DIR=subprocess.check_output(['xcode-select', '-p'], text=True).strip())
    identity = team = None
    if args.unsigned:
        destination = 'generic/platform=iOS'
        signing_settings = ['CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY=']
    else:
        if args.team_id:
            team = args.team_id
        else:
            identity, team = development_identity()
        destination = f'id={args.device}'
        signing_settings = ['-allowProvisioningUpdates', '-allowProvisioningDeviceRegistration',
                            'CODE_SIGN_STYLE=Automatic', 'CODE_SIGNING_ALLOWED=YES', 'CODE_SIGNING_REQUIRED=YES',
                            'CODE_SIGN_IDENTITY=Apple Development', f'DEVELOPMENT_TEAM={team}']
    command = ['xcodebuild', '-project', 'ACNH.xcodeproj', '-scheme', 'ACNH', '-configuration', 'Release',
               '-destination', destination, '-derivedDataPath', build_dir, *signing_settings, 'build']
    run(command, logs / 'xcodebuild-ios.log', cwd=IOS, env=env)
    app = build_dir / 'Build/Products/Release-iphoneos/ACNH.app'
    if not app.is_dir():
        raise RuntimeError(f'Xcode 未生成应用：{app}')

    # Gen/Resources/GameData 是 Xcode Copy Bundle Resources 的 folder reference；由 Xcode 在资源入包后统一签名。
    if not args.unsigned:
        if not (app / 'embedded.mobileprovision').is_file():
            raise RuntimeError('Xcode 构建产物没有开发者描述文件，无法安装到真机')
        identity = signed_identity_sha1(app)
        for name, path in [('app', app), ('executable', app / 'ACNH')]:
            run(['codesign', '--verify', '--deep', '--strict', path], logs / f'verify-{name}.log')
    app_info = plistlib.loads((app / 'Info.plist').read_bytes())
    icons = app_info.get('CFBundleIcons', {}).get('CFBundlePrimaryIcon', {})
    if icons.get('CFBundleIconName') != 'AppIcon' or not (app / 'Assets.car').is_file():
        raise RuntimeError('应用图标资源未正确编译入包')
    if app_info.get('CFBundleIdentifier') != BUNDLE_ID or app_info.get('MinimumOSVersion', '0') > '27.2':
        raise RuntimeError('IPA 的 bundle ID 或最低系统版本不匹配')

    payload = WORK / 'build/ios-payload'
    if payload.exists():
        shutil.rmtree(payload)
    (payload / 'Payload').mkdir(parents=True)
    copy_tree(app, payload / 'Payload/ACNH.app')
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
        for path in sorted((payload / 'Payload').rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(payload).as_posix())
    with zipfile.ZipFile(output) as archive:
        bad = archive.testzip()
        if bad:
            raise RuntimeError(f'IPA ZIP CRC 校验失败：{bad}')
        names = archive.namelist()
        if 'Payload/ACNH.app/ACNH' not in names or 'Payload/ACNH.app/GameData/romfs/romfs.bin' not in names:
            raise RuntimeError('IPA 缺少主程序或 ACNH RomFS')
    print(json.dumps({'ipa': str(output), 'bytes': output.stat().st_size, 'sha256': sha256(output),
                      'bundle_id': BUNDLE_ID, 'team_id': team, 'development_identity_sha1': identity,
                      'signed': not args.unsigned,
                      'shader_variants': shader_count, 'romfs_sha256': expected_hash}, ensure_ascii=False, indent=2))
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=WORK / 'ACNH-iPhone.ipa')
    parser.add_argument('--device', default='726E8C20-4F82-5E2E-A72E-0C8CD22FACBF')
    parser.add_argument('--team-id', help='在 Xcode 自动签名时指定已登录的 Apple 开发者团队')
    parser.add_argument('--unsigned', action='store_true', help='生成不可直接安装的 unsigned IPA，用于本机签名配置缺失时预检资源包')
    package(parser.parse_args())


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'错误：{error}', file=sys.stderr)
        raise SystemExit(1)
