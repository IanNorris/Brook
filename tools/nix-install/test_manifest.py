"""Real file/rename/flush regressions for the installer's metadata writer.

The small driver calls the same implementation as nix-install. RLIMIT_FSIZE
forces an actual failed write without allocating a disk-full test volume.
"""
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='brook-manifest-build-')
        source = Path(cls.build.name) / 'driver.c'
        source.write_text('#define _GNU_SOURCE\n#include "manifest.h"\n'
                          'int main(int argc,char **argv) {\n'
                          ' if(argc!=3 && argc!=5)return 2;\n'
                          ' if(manifest_update(argv[1],argv[2],argc==5?argv[3]:NULL,argc==5?argv[4]:NULL)<0){perror("manifest");return 1;}\n'
                          ' return 0;\n}\n')
        cls.driver = str(Path(cls.build.name) / 'manifest-driver')
        subprocess.run([os.environ.get('HOST_CC', 'cc'), '-O2', '-Wall', '-Wextra',
                        '-Werror', '-I', str(ROOT), str(source), '-o', cls.driver], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='brook-manifest-')
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'manifest.tsv'

    def update(self, name, *entry, ok=True, preexec_fn=None):
        result = subprocess.run([self.driver, str(self.path), name, *entry],
                                capture_output=True, preexec_fn=preexec_fn)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        self.assertEqual(list(self.path.parent.glob('*.tmp.*')), [])
        return result

    def test_remove_preserves_more_than_256_packages_exactly(self):
        rows = [f'package-{i}\t1.0\thash-package-{i}\n'.encode() for i in range(1000)]
        self.path.write_bytes(b''.join(rows))
        self.update('package-500')
        self.assertEqual(self.path.read_bytes(), b''.join(rows[:500] + rows[501:]))

    def test_blank_separator_rows_are_preserved(self):
        original=b'editor\t1\thash-editor\n\n'
        self.path.write_bytes(original)
        self.update('viewer','2','hash-viewer')
        self.assertEqual(self.path.read_bytes(),original+b'viewer\t2\thash-viewer\n')

    def test_add_replace_and_case_insensitive_remove(self):
        self.update('Editor', '1', 'hash-editor')
        self.update('viewer', '2', 'hash-viewer')
        self.update('editor', '3', 'hash-editor-new')
        self.assertEqual(self.path.read_text(), 'viewer\t2\thash-viewer\neditor\t3\thash-editor-new\n')
        self.update('EDITOR')
        self.assertEqual(self.path.read_text(), 'viewer\t2\thash-viewer\n')

    def test_failed_write_preserves_old_manifest_and_removes_temporary(self):
        old = b'editor\t1\thash-editor\n' * 100
        self.path.write_bytes(old)
        def restrict():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (128, 128))
        self.update('viewer', '2', 'hash-viewer', ok=False, preexec_fn=restrict)
        self.assertEqual(self.path.read_bytes(), old)

    def test_invalid_or_missing_records_do_not_change_manifest(self):
        for bad in (b'editor\t1\thash-editor', b'malformed\n',
                    b'editor\t1\t\n', b'x'*17000+b'\t1\thash\n',
                    b'editor\t1\thash\0tail\n'):
            self.path.write_bytes(bad)
            self.update('viewer', '1', 'hash-viewer', ok=False)
            self.assertEqual(self.path.read_bytes(), bad)
        old = b'editor\t1\thash-editor\n'
        self.path.write_bytes(old)
        self.update('missing', ok=False)
        self.assertEqual(self.path.read_bytes(), old)

    def test_symlink_and_tsv_injection_are_refused(self):
        target = self.path.parent / 'original'
        target.write_text('editor\t1\thash-editor\n')
        self.path.symlink_to(target)
        self.update('viewer', '1', 'hash-viewer', ok=False)
        self.assertTrue(self.path.is_symlink())
        self.path.unlink()
        self.path.write_bytes(target.read_bytes())
        for name, version, store in [('bad\nname','1','hash'), ('valid','1\tbroken','hash'),
                                     ('valid','1','../elsewhere')]:
            self.update(name, version, store, ok=False)
        self.assertEqual(self.path.read_bytes(), target.read_bytes())

    def test_readers_observe_whole_old_or_new_file(self):
        old = b'editor\t1\thash-editor\n' * 500
        new = old + b'viewer\t1\thash-viewer\n'
        self.path.write_bytes(old)
        for _ in range(15):
            process = subprocess.Popen([self.driver, str(self.path), 'viewer', '1', 'hash-viewer'])
            while process.poll() is None:
                self.assertIn(self.path.read_bytes(), (old, new))
            self.assertEqual(process.wait(), 0)
            self.assertEqual(self.path.read_bytes(), new)
            self.update('viewer')
            self.assertEqual(self.path.read_bytes(), old)


class InstallerLockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='brook-installer-cli-')
        cls.root = Path(cls.directory.name)
        cls.profile = cls.root / 'profile'
        cls.profile.mkdir()
        mapping = dict(INDEX_PATH='index', PROFILE_DIR='profile',
                       MANIFEST_PATH='profile/manifest.tsv', PROFILE_BIN='profile/bin',
                       PROFILE_LIB='profile/lib', PROFILE_APPS='profile/share/applications',
                       APPS_DIR='apps', APPS_IDX='apps/applications.idx', APPS_ICONS_DIR='apps/icons')
        definitions = [f'-D{name}="{cls.root / path}"' for name, path in mapping.items()]
        cls.binary = str(cls.root / 'nix')
        subprocess.run([os.environ.get('HOST_CC','cc'), '-O2', *definitions,
                        str(ROOT / 'nix-install.c'), '-o', cls.binary], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_existing_lock_refuses_mutation_and_is_not_removed(self):
        path = self.profile / 'manifest.tsv'
        original = b'editor\t1\tbrook-regression-no-such-store-path\n'
        path.write_bytes(original)
        lock = self.profile / '.mutation-lock'
        lock.mkdir()
        self.addCleanup(lambda: lock.rmdir() if lock.exists() else None)
        result = subprocess.run([self.binary, 'remove', 'editor'], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Profile busy', result.stderr)
        self.assertTrue(lock.is_dir())
        self.assertEqual(path.read_bytes(), original)

    def test_public_remove_preserves_large_manifest_and_releases_own_lock(self):
        path = self.profile / 'manifest.tsv'
        rows = [f'package-{i}\t1\tbrook-regression-missing-{i}\n'.encode() for i in range(1000)]
        path.write_bytes(b''.join(rows))
        result = subprocess.run([self.binary, 'remove', 'package-500'], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"\x1b[",result.stdout)
        self.assertIn(b"Removed package-500",result.stdout)
        self.assertEqual(path.read_bytes(), b''.join(rows[:500] + rows[501:]))
        self.assertFalse((self.profile / '.mutation-lock').exists())

    def test_invalid_manifest_failure_releases_lock_and_preserves_bytes(self):
        path = self.profile / 'manifest.tsv'
        original = b'editor\t1\tbrook-regression-missing\nmalformed\n'
        path.write_bytes(original)
        result = subprocess.run([self.binary, 'remove', 'editor'], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(path.read_bytes(), original)
        self.assertFalse((self.profile / '.mutation-lock').exists())


if __name__ == '__main__':
    unittest.main()
