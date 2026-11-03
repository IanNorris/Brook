import importlib.util
import stat
import tempfile
import unittest
from unittest import mock
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('bootstrap', Path(__file__).with_name('bootstrap.py'))
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class BootstrapTests(unittest.TestCase):
    def test_runtime_opt_in_requires_the_staged_64_bit_client(self):
        with tempfile.TemporaryDirectory() as tmp:
            client = Path(tmp)
            with self.assertRaises(ValueError):
                bootstrap.enable_steamrt64(client)
            runtime = client / 'steamrt64'; runtime.mkdir()
            (runtime / 'steam').write_bytes(b'fixture')
            bootstrap.enable_steamrt64(client)
            self.assertEqual((client / '.steam-enable-steamrt64-client').read_bytes(), b'')

    def test_gnutls_uses_matching_nettle_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); libs = root / 'hostlibs'; libs.mkdir()
            dependency = root / 'nettle'; dependency.mkdir()
            profile = root / 'profile'; profile.mkdir()
            first = root / 'gnutls-first'; first.write_bytes(b'fixture')
            second = root / 'gnutls-second'; second.write_bytes(b'fixture2')
            (profile / 'libgnutls.so.30').symlink_to(first)
            (libs / 'libgnutls.so.30').symlink_to(profile / 'libgnutls.so.30')
            for name in ('libhogweed.so.6', 'libnettle.so.8'):
                (dependency / name).write_bytes(b'matching')
            with mock.patch.object(bootstrap.subprocess, 'check_output', return_value=str(dependency)):
                bootstrap.pair_gnutls_dependencies(libs, '/bin/patchelf')
                for name in ('libhogweed.so.6', 'libnettle.so.8'):
                    self.assertEqual((libs / name).resolve(), dependency / name)
                (profile / 'libgnutls.so.30').unlink()
                (profile / 'libgnutls.so.30').symlink_to(second)
                self.assertEqual((libs / 'libgnutls.so.30').resolve(), first)
                (dependency / 'libnettle.so.8').unlink()
                with self.assertRaises(ValueError):
                    bootstrap.pair_gnutls_dependencies(libs, '/bin/patchelf')

    def test_manifest_requires_safe_hashed_64_bit_packages(self):
        text = '"steam_steamrt64" { "file" "steam.zip.123" "size" "12" "sha2" "' + 'a' * 64 + '" }'
        self.assertEqual(bootstrap.parse_manifest(text)[0][1:3], ('steam.zip.123', 12))
        for bad in (text.replace('steam.zip.123', '../bad'), text.replace('a' * 64, 'bad'), ''):
            with self.assertRaises(ValueError):
                bootstrap.parse_manifest(bad)

    def test_extract_normalizes_paths_and_preserves_internal_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'client'; root.mkdir()
            archive = Path(tmp) / 'steam.zip'
            link = zipfile.ZipInfo('steamrt64/link')
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('steamrt64\\steam', b'payload')
                z.writestr(link, 'steam')
                z.writestr('linux32/skip', b'32-bit')
            bootstrap.extract(archive, root)
            self.assertEqual((root / 'steamrt64/link').read_bytes(), b'payload')
            self.assertFalse((root / 'linux32').exists())

    def test_extract_rejects_traversal_and_escaping_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'client'; root.mkdir()
            archive = Path(tmp) / 'bad.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('../outside', b'bad')
            with self.assertRaises(ValueError):
                bootstrap.extract(archive, root)
            link = zipfile.ZipInfo('escape'); link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr(link, '../outside')
            with self.assertRaises(ValueError):
                bootstrap.extract(archive, root)

    def test_publish_never_replaces_existing_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp) / 'stage'; stage.mkdir()
            home = Path(tmp) / 'home'; home.mkdir()
            with self.assertRaises(FileExistsError):
                bootstrap.publish(stage, home)
            self.assertTrue(stage.is_dir())
            self.assertTrue(home.is_dir())
            home.rmdir()
            bootstrap.publish(stage, home)
            self.assertTrue(home.is_dir())
            self.assertFalse(stage.exists())

    def test_rejects_excessive_expansion_before_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'client'; root.mkdir()
            archive = Path(tmp) / 'large.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('too-large', b'12345678901')
            with mock.patch.object(bootstrap, 'MAX_EXPANDED', 10):
                with self.assertRaises(ValueError):
                    bootstrap.extract(archive, root)
            self.assertFalse((root / 'too-large').exists())


if __name__ == '__main__':
    unittest.main()
