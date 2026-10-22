#!/usr/bin/env python3
"""Synthetic directory-listing regressions using the production RomFS parser."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'third_party/macnativegames/games/totk'
REAL_ARGS = []
if '--real-romfs' in sys.argv:
    pos = sys.argv.index('--real-romfs')
    REAL_ARGS = sys.argv[pos:pos + 4]
    del sys.argv[pos:pos + 4]


class RomfsDirectoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='romfs-directory-')
        tmp = Path(cls.temp.name)
        image_size = Path(REAL_ARGS[1]).stat().st_size if REAL_ARGS else 1 << 40
        (tmp / 'romfs_config.h').write_text(
            f'#define ROMFS_IMAGE_SIZE {image_size}ULL\n'
            '#define ROMFS_IMAGE_SHA256 ""\n'
            '#define ROMFS_INDEX_SHA256 ""\n'
        )
        cls.exe = tmp / 'romfs-directory-test'
        subprocess.run([
            'clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-ffunction-sections', '-fdata-sections',
            '-DGAME_ACNH=1', '-I', str(tmp), '-I', str(VENDOR / 'runtime'),
            '-I', str(VENDOR / 'third_party/d2r_gfx/gfx/nvn/include'),
            str(ROOT / 'tests/romfs_directory_parser_test.c'),
            str(VENDOR / 'runtime/romfs_directory.c'), '-Wl,-dead_strip',
            '-framework', 'CoreFoundation', '-o', str(cls.exe),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_synthetic_romfs_metadata_and_listing(self):
        subprocess.run([str(self.exe)], check=True)

    @unittest.skipUnless(REAL_ARGS, 'pass --real-romfs IMAGE --runtime-index INDEX for the verified local asset probe')
    def test_real_rumble_directory(self):
        args = iter(REAL_ARGS)
        next(args)
        image = Path(next(args))
        next(args)
        index = Path(next(args))
        result = subprocess.run([str(self.exe), str(image), str(index)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"rumble_returned":33', result.stdout)
        print(result.stdout, end='')


if __name__ == '__main__':
    unittest.main()
