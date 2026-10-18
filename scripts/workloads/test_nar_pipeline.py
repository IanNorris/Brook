import hashlib
from pathlib import Path
import tempfile
import unittest
from nar_pipeline import verify

class OutputOracle(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);(self.root/'sub').mkdir();self.file=self.root/'sub/data';self.file.write_bytes(b'owned bytes');self.file.chmod(0o644)
        self.expected={'sub/data':{'size':11,'sha256':hashlib.sha256(b'owned bytes').hexdigest(),'mode':0o644}}
    def test_exact_output_passes(self):self.assertEqual(verify(self.root,self.expected),1)
    def test_same_size_corruption_is_rejected(self):
        self.file.write_bytes(b'wrong bytes')
        with self.assertRaises(ValueError):verify(self.root,self.expected)
    def test_truncated_output_is_rejected(self):
        self.file.write_bytes(b'owned')
        with self.assertRaises(ValueError):verify(self.root,self.expected)
    def test_mode_change_is_rejected(self):
        self.file.chmod(0o755)
        with self.assertRaises(ValueError):verify(self.root,self.expected)
    def test_extra_empty_directory_is_rejected(self):
        (self.root/'extra').mkdir()
        with self.assertRaises(ValueError):verify(self.root,self.expected)
    def test_missing_file_is_rejected(self):
        self.file.unlink()
        with self.assertRaises(ValueError):verify(self.root,self.expected)
    def test_symlink_is_rejected(self):
        self.file.unlink();self.file.symlink_to('/dev/null')
        with self.assertRaises(ValueError):verify(self.root,self.expected)
if __name__=='__main__':unittest.main()
