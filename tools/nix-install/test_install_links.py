"""Public install/repair results with real private profile and store trees.

The fetch command is a successful no-op: these tests cover cached-profile
publication, not network/extractor behavior (covered by test_cancel.py).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent
STORE_NAME='a'*32+'-fixture'


class InstallLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='brook-install-links-')
        cls.root=Path(cls.build.name)/'fixture'
        cls.binary=Path(cls.build.name)/'nix'
        paths=dict(STORE_DIR='store',INDEX_PATH='index',PROFILE_DIR='profile',
                   PROFILE_BIN='profile/bin',PROFILE_LIB='profile/lib',
                   PROFILE_APPS='profile/share/applications',MANIFEST_PATH='profile/manifest.tsv',
                   APPS_DIR='apps',APPS_IDX='apps/applications.idx',APPS_ICONS_DIR='apps/icons',BROOK_BIN='wrappers')
        definitions=[f'-D{name}="{cls.root/path}"' for name,path in paths.items()]
        definitions.append(f'-DNIX_FETCH_PATH="{shutil.which("true")}"')
        subprocess.run([os.environ.get('HOST_CC','cc'),'-O2',*definitions,str(ROOT/'nix-install.c'),'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        if self.root.exists():shutil.rmtree(self.root)
        self.root.mkdir()
        self.package=self.root/'store'/STORE_NAME
        for directory in ['bin','lib','share/applications']:
            (self.package/directory).mkdir(parents=True)
        (self.package/'bin/tool').write_text('#!/bin/sh\nprintf "profile fixture executable is healthy\\n"\n'+('# padding\n'*8))
        (self.package/'bin/tool').chmod(0o755)
        (self.package/'lib/libfixture.so').write_text('owned fixture library')
        (self.package/'share/applications/fixture.desktop').write_text('[Desktop Entry]\nName=Fixture\nExec=tool\n')
        (self.root/'index').write_text(f'fixture\t1\t{STORE_NAME}\tOwned fixture\n')
        self.manifest=self.root/'profile/manifest.tsv'

    def install(self,success=True):
        result=subprocess.run([str(self.binary),'install','fixture'],capture_output=True,timeout=15)
        self.assertEqual(result.returncode==0,success,result.stdout+result.stderr)
        self.assertFalse((self.root/'profile/.mutation-lock').exists())
        self.assertEqual(list(self.root.rglob('.brook-link.*')),[])
        return result

    def links(self):
        return [self.root/'profile'/p for p in ['bin/tool','lib/libfixture.so','share/applications/fixture.desktop']]

    def test_cached_install_and_repair_keep_complete_links(self):
        self.install()
        self.assertEqual(self.manifest.read_text(),f'fixture\t1\t{STORE_NAME}\n')
        for link in self.links():self.assertTrue(link.is_file(),link)
        inodes=[link.lstat().st_ino for link in self.links()]
        self.install()
        self.assertEqual([link.lstat().st_ino for link in self.links()],inodes)
        self.links()[0].unlink();self.install()
        self.assertTrue(self.links()[0].is_file())

    def test_each_link_class_failure_is_reported_and_retry_repairs(self):
        for relative in ['bin/tool','lib/libfixture.so','share/applications/fixture.desktop']:
            with self.subTest(relative=relative):
                self.setUp()
                blocked=self.root/'profile'/relative
                blocked.mkdir(parents=True);(blocked/'keep').write_text('unchanged')
                result=self.install(success=False)
                self.assertIn(b'incomplete profile links',result.stderr)
                self.assertNotIn(b'Installed fixture',result.stdout)
                self.assertEqual((blocked/'keep').read_text(),'unchanged')
                # Metadata precedes links in the current protocol: this must be
                # explicit rather than falsely asserting whole-profile rollback.
                self.assertEqual(self.manifest.read_text(),f'fixture\t1\t{STORE_NAME}\n')
                shutil.rmtree(blocked);self.install()
                for link in self.links():self.assertTrue(link.is_file(),link)


if __name__=='__main__':
    unittest.main()
