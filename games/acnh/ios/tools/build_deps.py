#!/usr/bin/env python3
"""为 ACNH iOS 编译 arm64 Opus 与 libvpx 静态库。"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    subprocess.run(command, cwd=cwd, env=env, check=True)


def checkout(url: str, tag: str, destination: Path) -> None:
    if (destination / '.git').is_dir():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'clone', '--depth', '1', '--branch', tag, url, str(destination)])


def main() -> None:
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip())
    sdk_link = output / 'sdkroot/Platforms/iPhoneOS.platform/Developer/SDKs' / sdk.name
    sdk_link.parent.mkdir(parents=True, exist_ok=True)
    if sdk_link.is_symlink():
        sdk_link.unlink()
    if not sdk_link.exists():
        sdk_link.symlink_to(sdk)

    source = output / 'src'
    build = output / 'build'
    opus = source / 'opus'
    checkout('https://github.com/xiph/opus.git', 'v1.6.1', opus)
    opus_build = build / 'opus-make'
    if opus_build.exists():
        shutil.rmtree(opus_build)
    run(['cmake', '-G', 'Unix Makefiles', '-S', str(opus), '-B', str(opus_build),
         '-DCMAKE_SYSTEM_NAME=iOS', f'-DCMAKE_OSX_SYSROOT={sdk_link}',
         '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0',
         '-DOPUS_BUILD_SHARED_LIBRARY=OFF', '-DOPUS_BUILD_TESTING=OFF',
         '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF', '-DCMAKE_BUILD_TYPE=Release'])
    run(['cmake', '--build', str(opus_build), '--config', 'Release', '--parallel', str(os.cpu_count() or 4)])

    libvpx = source / 'libvpx'
    checkout('https://chromium.googlesource.com/webm/libvpx.git', 'v1.16.0', libvpx)
    vpx_build = build / 'libvpx'
    vpx_build.mkdir(parents=True, exist_ok=True)
    bin_dir = build / 'ios-bin'
    bin_dir.mkdir(parents=True, exist_ok=True)
    tools = Path(os.environ.get('DEVELOPER_DIR', subprocess.check_output(['xcode-select', '-p'], text=True).strip())) / 'Toolchains/XcodeDefault.xctoolchain/usr/bin'
    for tool in ('clang', 'clang++', 'ar', 'as', 'strip'):
        link = bin_dir / tool
        if link.is_symlink():
            link.unlink()
        if not link.exists():
            link.symlink_to(tools / tool)
    wrapper = bin_dir / 'xcrun'
    wrapper.write_text(
        '#!/usr/bin/env python3\n'
        'import os,sys\n'
        'os.execv("/usr/bin/xcrun", ["xcrun", *sys.argv[1:]])\n')
    wrapper.chmod(0o755)
    env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ.get('PATH', ''))
    run([str(libvpx / 'configure'), '--target=arm64-darwin-gcc', '--disable-examples', '--disable-tools',
         '--disable-docs', '--disable-unit-tests', '--disable-webm-io', '--enable-pic', '--disable-shared',
         '--enable-static', f'--extra-cflags=-target arm64-apple-ios26.0 -isysroot {sdk_link} -miphoneos-version-min=26.0'],
        cwd=vpx_build, env=env)
    run(['make', '-j', str(os.cpu_count() or 4), 'libvpx.a'], cwd=vpx_build, env=env)

    libraries = output / 'lib'
    headers = output / 'include'
    libraries.mkdir(exist_ok=True)
    headers.mkdir(exist_ok=True)
    shutil.copy2(opus_build / 'libopus.a', libraries / 'libopus.a')
    shutil.copy2(vpx_build / 'libvpx.a', libraries / 'libvpx.a')
    shutil.copytree(opus / 'include', headers, dirs_exist_ok=True)
    shutil.copytree(libvpx / 'vpx', headers / 'vpx', dirs_exist_ok=True)
    shutil.copy2(vpx_build / 'vpx_config.h', headers / 'vpx_config.h')
    print(f'IOS_DEPS={output}')


if __name__ == '__main__':
    main()
