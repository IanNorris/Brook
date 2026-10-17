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

    def test_real_repository_index(self):
        result=subprocess.run([str(BINARY),'--check-catalog',str(ROOT/'tools/nix-index/packages.idx'),str(self.manifest)],capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(b'CATALOGCHECK PASS',result.stdout)


if __name__=='__main__':
    unittest.main()
