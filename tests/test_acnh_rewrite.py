import struct
import os
import subprocess
import sys
import tempfile
import unittest
from io import StringIO
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
HELPER_DIR = ROOT / 'engine/tools/nso-rewriter'
sys.path.insert(0, str(HELPER_DIR))
sys.path.insert(0, str(ROOT / 'games/acnh/scripts'))

from dynsym import assert_call_targets_safe, symbol_count
from rewrite import patcher_environment, replace_directory


class DynsymFallbackTests(unittest.TestCase):
    def setUp(self):
        self.image = bytearray(512)
        self.header = bytearray(0x100)
        self.ro_off = 0x100
        self.symtab = 0x120
        self.target = 0x88
        struct.pack_into('<IIII', self.header, 0x90, 0, 0, 0x20, 48)
        self.tags = {6: self.symtab, 11: 24}
        # One undefined and one defined symbol.  GNU-only modules have no DT_HASH.
        struct.pack_into('<IBBHQQ', self.image, self.symtab, 0, 0, 0, 0, 0, 0)
        struct.pack_into('<IBBHQQ', self.image, self.symtab + 24, 0, 0x12, 0, 1, 0x90, 4)

    def test_gnu_only_dynsym_count_uses_bounded_nso_extent(self):
        self.assertEqual(symbol_count(self.image, self.header, self.ro_off, self.tags), 2)
        call_targets = {self.target}
        assert_call_targets_safe(
            self.image, self.tags, 2, call_targets, lambda *_: [], 0, len(self.image)
        )

    def test_gnu_only_defined_symbol_into_call_word_is_rejected(self):
        struct.pack_into('<IBBHQQ', self.image, self.symtab + 24, 0, 0x12, 0, 1, self.target, 4)
        with self.assertRaisesRegex(ValueError, 'dynamic symbol enters'):
            assert_call_targets_safe(
                self.image, self.tags, 2, {self.target}, lambda *_: [], 0, len(self.image)
            )

    def test_gnu_only_relr_pointer_into_call_word_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'RELR pointer enters'):
            assert_call_targets_safe(
                self.image, self.tags, 2, {self.target}, lambda *_: [(0x40, self.target)], 0, len(self.image)
            )

    def test_gnu_only_relative_rela_into_call_word_is_rejected(self):
        self.tags.update({7: 0x180, 8: 24})
        struct.pack_into('<QQq', self.image, 0x180, 0x30, 1027, self.target)
        with self.assertRaisesRegex(ValueError, 'relative pointer enters'):
            assert_call_targets_safe(
                self.image, self.tags, 2, {self.target}, lambda *_: [], 0, len(self.image)
            )

    def test_gnu_only_invalid_or_mismatched_nso_extent_is_rejected(self):
        bad_header = bytearray(self.header)
        struct.pack_into('<I', bad_header, 0x9C, 47)
        with self.assertRaisesRegex(ValueError, 'dynsym size'):
            symbol_count(self.image, bad_header, self.ro_off, self.tags)
        with self.assertRaisesRegex(ValueError, 'offset does not match'):
            symbol_count(self.image, self.header, self.ro_off + 1, self.tags)

    def test_patcher_environment_disables_inherited_python_optimization(self):
        with patch.dict(os.environ, {'PYTHONOPTIMIZE': '1'}):
            env = patcher_environment()
        self.assertEqual(env['PYTHONOPTIMIZE'], '0')
        result = subprocess.run(
            [sys.executable, '-c', "assert False, 'assert must stay enabled'"],
            env=env, capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('AssertionError', result.stderr)

    def test_backup_cleanup_failure_keeps_committed_output_and_backup(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder)
            destination = parent / 'rewritten'
            staged = parent / '.stage'
            destination.mkdir()
            (destination / 'old.txt').write_text('old')
            staged.mkdir()
            (staged / 'new.txt').write_text('new')
            with patch('rewrite.shutil.rmtree', side_effect=OSError('cleanup denied')):
                with patch('sys.stderr', new=StringIO()) as stderr:
                    replace_directory(staged, destination)
            self.assertIn('backup preserved', stderr.getvalue())
            self.assertTrue((destination / 'new.txt').is_file())
            self.assertFalse((destination / 'old.txt').exists())
            backups = list(parent.glob('.rewritten-backup-*'))
            self.assertEqual(len(backups), 1)
            self.assertTrue((backups[0] / 'old.txt').is_file())

    def test_directory_rename_failure_restores_previous_output(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder)
            destination = parent / 'rewritten'
            staged = parent / '.stage'
            destination.mkdir()
            (destination / 'old.txt').write_text('old')
            staged.mkdir()
            (staged / 'new.txt').write_text('new')
            real_replace = os.replace
            calls = 0

            def fail_stage_rename(source, target):
                nonlocal calls
                calls += 1
                if calls == 2:
                    raise OSError('stage rename failed')
                return real_replace(source, target)

            with patch('rewrite.os.replace', side_effect=fail_stage_rename):
                with self.assertRaisesRegex(OSError, 'stage rename failed'):
                    replace_directory(staged, destination)
            self.assertTrue((destination / 'old.txt').is_file())
            self.assertFalse((destination / 'new.txt').exists())


if __name__ == '__main__':
    unittest.main()
