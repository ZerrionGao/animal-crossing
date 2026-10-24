import importlib.util
import os
import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'games/acnh/scripts/package_macos.py'
spec = importlib.util.spec_from_file_location('acnh_package_macos', SCRIPT)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


@unittest.skipUnless(subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode == 0, '需要 Xcode 命令行工具')
class AcnhMacosPackageTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.work = self.root / 'work'
        self.app = self.root / 'Animal Crossing New Horizons.app'
        self._make_work()

    def _touch(self, path: Path, data: bytes = b'x') -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o755 if path.suffix == '' or path.name in {'totk_xlate', 'spirv-cross'} else 0o644)

    def _make_work(self) -> None:
        stub = b'#!/bin/sh\nexit 0\n'
        self._touch(self.work / 'build/acnh-native-boot', stub)
        (self.work / 'build/acnh-native-boot').chmod(0o755)
        for name in ('main', 'sdk', 'subsdk0'):
            self._touch(self.work / 'rewritten' / f'{name}.patched.bin')
        self._touch(self.work / 'romfs/romfs.bin', b'romfs')
        self._touch(self.work / 'romfs/runtime-index.bin')
        fonts = self.work / 'shared-fonts'
        fonts.mkdir(parents=True)
        (fonts / 'manifest.json').write_text('{"status":"pass"}\n')
        self._touch(fonts / '0-standard.bfttf')
        self._touch(self.work / 'ngc/ac_common_not_b_nx')
        xlate = self.work / 'build/shader-service/totk_xlate'
        self._touch(xlate, stub)
        xlate.chmod(0o755)
        cross = self.work / 'build/spirv-cross/spirv-cross'
        self._touch(cross, stub)
        cross.chmod(0o755)

    def test_scan_forbidden_rejects_keys(self):
        bad = self.root / 'bad.app'
        (bad / 'Contents').mkdir(parents=True)
        (bad / 'Contents/prod.keys').write_text('nope')
        with self.assertRaises(package.PackageError):
            package.scan_forbidden(bad)

    def test_assemble_check_and_signature(self):
        expected = {'display_version': '3.0.0'}
        os.environ['TOTK_NGC_DIR'] = str(self.work / 'ngc')
        self.addCleanup(os.environ.pop, 'TOTK_NGC_DIR', None)
        app = package.assemble(self.work, self.app, expected, skip_icon=True, skip_user_data=True)
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertEqual(info['CFBundleIdentifier'], 'org.macnativegames.acnh')
        self.assertEqual(info['CFBundleShortVersionString'], '3.0.0')
        self.assertEqual(info['CFBundleExecutable'], 'ACNH')
        self.assertTrue((app / 'Contents/Resources/acnh/romfs/romfs.bin').is_file())
        self.assertTrue((app / 'Contents/Resources/acnh/ngc/ac_common_not_b_nx').is_file())
        check = subprocess.run([str(app / 'Contents/MacOS/ACNH'), '--check'], capture_output=True, text=True, check=True)
        self.assertTrue(check.stdout.startswith('ok:'))
        self.assertEqual(subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)]).returncode, 0)
        source = (ROOT / 'games/acnh/app/launcher.m').read_text()
        self.assertIn('TOTK_GPU_ALLOW_SUBMIT', source)
        self.assertIn('TOTK_GPU_MIDCLEAR', source)
        self.assertNotIn('TOTK_GPU_WIDEN', source)
        self.assertNotIn('gh gist', source)


if __name__ == '__main__':
    unittest.main()
