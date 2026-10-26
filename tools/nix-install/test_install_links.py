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
        paths=dict(STORE_DIR='store',NARINFO_DIR='narinfo',INDEX_PATH='index',PROFILE_DIR='profile',
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

    def test_nonexecutable_elf_debug_artifact_does_not_trigger_repair(self):
        debug=self.package/'bin/tool.debug'
        # Debug-only ELF files retain load headers without their load data;
        # they are not corrupt executables and must not force a re-download.
        debug.write_bytes(b'\x7fELF'+bytes(124));debug.chmod(0o644)
        self.install()
        result=self.install()
        self.assertNotIn(b'corrupt ELF',result.stderr)
        self.assertNotIn(b'incomplete; repairing',result.stdout)
        debug.chmod(0o755)
        # The same damaged file *as an executable* still enters repair.
        self.assertIn(b'corrupt ELF',self.install().stderr)

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

    def references(self, names):
        cache=self.root/'narinfo';cache.mkdir(exist_ok=True)
        (cache/(STORE_NAME[:32]+'.narinfo')).write_text('References: '+' '.join(names)+'\n')

    def test_file_output_dependencies_do_not_require_lib_directories(self):
        name='b'*32+'-runtime.msi';link='c'*32+'-runtime-link'
        data=self.root/'store'/name;data.write_bytes(b'owned standalone store output')
        (self.root/'store'/link).symlink_to(name)
        self.references([name,link])
        self.install()
        for path in self.links():self.assertTrue(path.is_file(),path)
        self.assertEqual(data.read_bytes(),b'owned standalone store output')
        self.install()  # Exercise repair of the already-published registration.

    def test_malformed_dependency_lib_directory_remains_an_error(self):
        name='d'*32+'-broken';dependency=self.root/'store'/name;dependency.mkdir()
        (dependency/'lib').write_text('a file where the library directory belongs')
        self.references([name])
        self.assertIn(b'incomplete profile links',self.install(success=False).stderr)

    def test_regular_store_output_has_no_profile_subdirectories(self):
        shutil.rmtree(self.package);self.package.write_bytes(b'owned data-only output')
        self.install()
        self.assertEqual(self.package.read_bytes(),b'owned data-only output')

    def remove(self):
        result=subprocess.run([str(self.binary),'remove','fixture'],capture_output=True,timeout=15)
        self.assertFalse((self.root/'profile/.mutation-lock').exists())
        return result

    def test_remove_cleans_relative_and_absolute_exports_preserving_store(self):
        self.install()
        for link in self.links():
            target=link.resolve();link.unlink();link.symlink_to(target)
        foreign=self.root/'profile/lib/foreign.so'
        foreign.symlink_to('/unrelated/owned/library')
        result=self.remove()
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        for link in self.links():self.assertFalse(link.is_symlink(),link)
        self.assertTrue(foreign.is_symlink())
        self.assertTrue((self.package/'bin/tool').is_file())
        self.assertTrue((self.package/'lib/libfixture.so').is_file())

    @unittest.skipIf(os.geteuid()==0,'directory permission failure requires an unprivileged host')
    def test_remove_reports_each_export_unlink_failure(self):
        for relative in ['bin/tool','lib/libfixture.so','share/applications/fixture.desktop']:
            with self.subTest(relative=relative):
                self.setUp();self.install()
                blocked=self.root/'profile'/relative
                blocked.parent.chmod(0o555)
                try:result=self.remove()
                finally:blocked.parent.chmod(0o755)
                self.assertTrue(blocked.is_symlink())
                self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn(b'incomplete profile removal',result.stderr)
                self.assertNotIn(b'Removed fixture',result.stdout)
                # The current manifest-first protocol requires reinstall to
                # recover an interrupted removal; do not claim rollback.
                self.install()
                for link in self.links():self.assertTrue(link.is_file(),link)


if __name__=='__main__':
    unittest.main()
