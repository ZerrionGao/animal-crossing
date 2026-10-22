#!/usr/bin/env python3
"""按固定 SHA/Build ID 对 ACNH 3.0.0 模块执行离线 NSO 改写预检。"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PROFILE_DEFAULT = ROOT / 'games/acnh/profiles/3.0.0.json'
PATCHER = ROOT / 'third_party/macnativegames/games/totk/third_party/d2r/patch_nso.py'
TOOL_FILES = [
    PATCHER,
    PATCHER.with_name('dynsym.py'),
    PATCHER.with_name('call_window.py'),
    PATCHER.with_name('relr.py'),
]
ARTIFACT_SUFFIXES = [
    'patched.bin', 'fixups.tsv', 'gates.tsv', 'sites.tsv', 'cuts.tsv', 'obs.tsv', 'params.tsv'
]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def tool_hashes(profile_path):
    paths = [Path(__file__).resolve(), profile_path.resolve(), *TOOL_FILES]
    return {
        path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path): sha256(path)
        for path in paths if path.is_file()
    }


def patcher_environment():
    env = os.environ.copy()
    env['DM_RULESET'] = 'native'
    env['DM_PRIVATE_GATES'] = '0'
    env['PYTHONOPTIMIZE'] = '0'
    return env


def read_profile(path):
    profile = json.loads(path.read_text(encoding='utf-8'))
    if profile['display_version'] != '3.0.0' or profile['vendor']['commit'] != 'dfa7ec426f8b37798073a108ede1504c827d8af0':
        raise ValueError('profile does not pin ACNH 3.0.0 and the selected MacNativeGames source')
    if profile['rewrite'] != {
        'ruleset': 'native', 'private_gates': False, 'expected_cuts': 0,
        'expected_observations': 0, 'output': 'work/acnh/3.0.0/rewritten'
    }:
        raise ValueError('rewrite profile does not match the approved offline preflight settings')
    return profile


def count_rows(path):
    return sum(1 for line in path.read_text(encoding='utf-8').splitlines() if line and not line.startswith('#'))


def replace_directory(staged, destination):
    backup = None
    if destination.exists():
        backup = Path(tempfile.mkdtemp(prefix='.rewritten-backup-', dir=destination.parent))
        backup.rmdir()
        os.replace(destination, backup)
    try:
        os.replace(staged, destination)
    except BaseException:
        if backup is not None and backup.exists():
            os.replace(backup, destination)
        raise
    if backup is not None:
        try:
            shutil.rmtree(backup)
        except OSError as exc:
            try:
                print(f'WARNING: could not remove previous output; backup preserved at {backup}: {exc}', file=sys.stderr)
            except OSError:
                pass


def run(profile_path, exefs, output, logs):
    profile = None
    exefs = exefs.resolve()
    output = output.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    stdout_path, stderr_path = logs / 'rewrite.stdout.log', logs / 'rewrite.stderr.log'
    stdout_path.write_text('', encoding='utf-8')
    stderr_path.write_text('', encoding='utf-8')
    inputs, outputs = {}, {}
    temp_parent = None
    try:
        profile = read_profile(profile_path)
        output.parent.mkdir(parents=True, exist_ok=True)
        temp_parent = Path(tempfile.mkdtemp(prefix='.rewrite-stage-', dir=output.parent))
        staged = temp_parent / 'rewritten'
        staged.mkdir()
        missing_tools = [str(path) for path in TOOL_FILES if not path.is_file()]
        if missing_tools:
            raise FileNotFoundError('missing rewriter tools: ' + ', '.join(missing_tools))
        tools = tool_hashes(profile_path)
        for name, expected in profile['modules'].items():
            source = exefs / name
            if not source.is_file():
                raise FileNotFoundError(f'missing input module: {source}')
            digest = sha256(source)
            if digest != expected['sha256']:
                raise ValueError(f'{name}: source SHA-256 mismatch: {digest}')
            with source.open('rb') as stream:
                header = stream.read(0x50)
            if len(header) < 0x50 or header[:4] != b'NSO0':
                raise ValueError(f'{name}: invalid or truncated NSO header')
            build_id = header[0x40:0x50].hex()
            if build_id != expected['build_id']:
                raise ValueError(f'{name}: Build ID mismatch: {build_id}')
            inputs[name] = {'path': str(source), 'sha256': digest, 'build_id': build_id}

        with stdout_path.open('w', encoding='utf-8') as stdout, stderr_path.open('w', encoding='utf-8') as stderr:
            for name in profile['modules']:
                env = patcher_environment()
                command = [sys.executable, str(PATCHER), str(exefs / name), str(staged), name]
                result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                stdout.write(f'$ {" ".join(command)}\n{result.stdout}\n')
                stderr.write(result.stderr)
                stdout.flush()
                stderr.flush()
                if result.returncode:
                    raise RuntimeError(f'{name}: patcher exited {result.returncode}; see {stderr_path}')

        for name in profile['modules']:
            artifacts = {}
            for suffix in ARTIFACT_SUFFIXES:
                path = staged / f'{name}.{suffix}'
                if not path.is_file():
                    raise ValueError(f'{name}: missing output {path.name}')
                artifacts[path.name] = {'sha256': sha256(path), 'size': path.stat().st_size}
            cuts = count_rows(staged / f'{name}.cuts.tsv')
            observations = count_rows(staged / f'{name}.obs.tsv')
            if cuts != profile['rewrite']['expected_cuts'] or observations != profile['rewrite']['expected_observations']:
                raise ValueError(f'{name}: expected cuts/obs 0/0, got {cuts}/{observations}')
            outputs[name] = {
                'source_sha256': inputs[name]['sha256'],
                'build_id': inputs[name]['build_id'],
                'artifacts': artifacts,
                'cuts': cuts,
                'observations': observations,
            }

        manifest = {
            'status': 'pass',
            'title': profile['title'],
            'display_version': profile['display_version'],
            'title_id': profile['title_id'],
            'vendor': profile['vendor'],
            'ruleset': profile['rewrite']['ruleset'],
            'private_gates': profile['rewrite']['private_gates'],
            'python_optimization': 0,
            'source_exefs': str(exefs),
            'inputs': inputs,
            'tools': tools,
            'modules': outputs,
            'cuts': sum(item['cuts'] for item in outputs.values()),
            'observations': sum(item['observations'] for item in outputs.values()),
        }
        (staged / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        (logs / 'manifest.failure.json').unlink(missing_ok=True)
        replace_directory(staged, output)
        try:
            print(json.dumps({'status': 'pass', 'output': str(output), 'modules': list(outputs)}, ensure_ascii=False))
        except OSError:
            pass
        return 0
    except Exception as exc:
        failed = {
            'status': 'fail', 'title': profile.get('title') if profile else None,
            'display_version': profile.get('display_version') if profile else None,
            'source_exefs': str(exefs), 'inputs_verified': inputs,
            'tools': tool_hashes(profile_path),
        }
        failed['error'] = str(exc)
        (logs / 'manifest.failure.json').write_text(json.dumps(failed, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        with stderr_path.open('a', encoding='utf-8') as stderr:
            stderr.write(str(exc) + '\n')
        print(json.dumps({'status': 'fail', 'error': str(exc), 'logs': str(logs)}, ensure_ascii=False), file=sys.stderr)
        return 1
    finally:
        if temp_parent is not None:
            shutil.rmtree(temp_parent, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, default=PROFILE_DEFAULT)
    parser.add_argument('--exefs', type=Path, default=ROOT / 'work/acnh/input/update/exefs')
    parser.add_argument('--output', type=Path, default=ROOT / 'work/acnh/3.0.0/rewritten')
    parser.add_argument('--logs', type=Path, default=ROOT / 'work/acnh/3.0.0/logs')
    args = parser.parse_args()
    try:
        return run(args.profile.resolve(), args.exefs, args.output, args.logs)
    except Exception as exc:
        print(json.dumps({'status': 'fail', 'error': str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
