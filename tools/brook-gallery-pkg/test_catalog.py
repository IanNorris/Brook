"""Exercise the Gallery's production parser without connecting to a display."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('BROOK_GALLERY_BIN', ROOT/'build/brook-gallery/bin/brook-gallery'))


@unittest.skipUnless(BINARY.exists(), 'Build tools/brook-gallery-pkg to build/brook-gallery first')
class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='brook-catalog-')
        self.addCleanup(self.directory.cleanup)
        self.index = Path(self.directory.name)/'index'
        self.manifest = Path(self.directory.name)/'manifest'
        self.manifest.write_text('')

    def check(self, index, manifest='', ok=True, contains=b''):
        self.index.write_bytes(index)
        if manifest is not None:
            self.manifest.write_text(manifest)
        result = subprocess.run([str(BINARY),'--check-catalog',str(self.index),str(self.manifest)],capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        self.assertIn(contains,result.stdout+result.stderr)

    def test_index_with_blank_separators_comments_and_duplicate_names(self):
        self.check(b'# local additions\neditor\t1\thash-editor\tEditor\n\neditor\t2\thash-editor-new\tEditor again\n',contains=b'total=1 available=1 local=0')

    def test_local_installed_package_is_discovered(self):
        self.check(b'editor\t1\thash-editor\tEditor\n', 'local-app\t1\thash-local\neditor\t1\thash-editor\n', contains=b'total=2 available=1 local=1')

    def test_torn_oversized_and_malformed_input_reports_failure(self):
        for index in (b'editor\t1\thash-editor\tEditor', b'bad-row\n',b'a'*17000+b'\t1\thash\tDescription\n',b'../name\t1\thash\tDescription\n'):
            self.check(index,ok=False,contains=b'CATALOGCHECK FAIL')
        self.check(b'editor\t1\thash\tEditor\n','torn-row',ok=False,contains=b'CATALOGCHECK FAIL')

    def test_absent_manifest_is_an_empty_profile(self):
        self.manifest.unlink()
        self.check(b'editor\t1\thash\tEditor\n',None,contains=b'total=1 available=1 local=0')

    def test_optional_main_program_column(self):
        self.check(b'mesen\t2.1.1\thash-mesen\tMulti-system emulator\tMesen\nrg\t1\thash-rg\tSearch\n',
                   contains=b'total=2 available=2 local=0')

    def hint(self, text):
        path = Path(self.directory.name)/'classify'
        path.write_text(text)
        result = subprocess.run([str(BINARY),'--check-hint',str(path)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        return result.stdout

    def test_post_install_guidance(self):
        # Desktop entry: the launcher icon is the way in; no extra buttons.
        out=self.hint('package=qalculate-gtk\nkind=gui\nlauncher=package\nlauncher_name=Qalculate!\nprogram=qalculate-gtk\ncommand=/nix/profile/bin/qalculate-gtk\n')
        self.assertIn('Open “Qalculate!” from the launcher. open=0 add=0',out)
        # GUI without an entry: Open plus an optional Add to launcher.
        out=self.hint('package=mesen\nkind=probably-gui\nlauncher=none\nprogram=Mesen\ncommand=/nix/profile/bin/Mesen\nreason=desktop UI runtime present (Avalonia.X11.dll)\n')
        self.assertIn('Mesen looks like a desktop program without a launcher entry',out)
        self.assertIn('open=1 add=1',out)
        out=self.hint('package=mesen\nkind=gui\nlauncher=added\nprogram=Mesen\ncommand=/nix/profile/bin/Mesen\n')
        self.assertIn('open=1 add=0',out)
        # Terminal programs get the command, never a launcher button.
        out=self.hint('package=ripgrep\nkind=cli\nlauncher=none\nprogram=rg\ncommand=/nix/profile/bin/rg\n')
        self.assertIn('Installed: run `rg` from Terminal. open=0 add=0',out)
        out=self.hint('package=zsnes\nkind=unsupported\nlauncher=package\nlauncher_name=zsnes\nprogram=zsnes\ncommand=/nix/profile/bin/zsnes\n')
        self.assertIn('not a 64-bit x86 program',out)
        self.assertIn('no programs to run',self.hint('package=fonts\nkind=none\nlauncher=none\n'))
        # Only profile programs can be opened.
        self.assertIn('open=0',self.hint('package=x\nkind=gui\nlauncher=none\nprogram=x\ncommand=/tmp/x\n'))
        self.assertIn('open=0',self.hint('package=x\nkind=gui\nlauncher=none\nprogram=x\ncommand=/nix/profile/bin/../x\n'))
        bad=Path(self.directory.name)/'bad';bad.write_text('kind=gui\n')
        self.assertNotEqual(subprocess.run([str(BINARY),'--check-hint',str(bad)],capture_output=True).returncode,0)

    def test_real_repository_index(self):
        result=subprocess.run([str(BINARY),'--check-catalog',str(ROOT/'tools/nix-index/packages.idx'),str(self.manifest)],capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(b'CATALOGCHECK PASS',result.stdout)


if __name__=='__main__':
    unittest.main()
