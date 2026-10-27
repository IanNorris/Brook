import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('stamp',Path(__file__).with_name('write_build_info.py'))
stamp=importlib.util.module_from_spec(spec);spec.loader.exec_module(stamp)
class BuildStamp(unittest.TestCase):
    def test_incremental_and_untracked(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);out=root/'ignored'/'stamp.cpp'
            def git(*args):return subprocess.check_output(['git','-C',str(root),*args],stderr=subprocess.DEVNULL)
            git('init','-q');git('config','user.name','Build test');git('config','user.email','build@example.invalid')
            (root/'.gitignore').write_text('ignored/\n');(root/'source').write_text('one');git('add','.');git('commit','-qm','initial')
            stamp.write_build_info(root,out);first=out.read_text();mtime=out.stat().st_mtime_ns
            stamp.write_build_info(root,out);self.assertEqual(mtime,out.stat().st_mtime_ns)
            (root/'new-header').write_text('two');stamp.write_build_info(root,out);second=out.read_text();self.assertNotEqual(first,second)
            (root/'new-header').write_text('three');stamp.write_build_info(root,out);self.assertNotEqual(second,out.read_text())
            git('add','.');git('commit','-qm','next');stamp.write_build_info(root,out);self.assertIn(git('rev-parse','--short','HEAD').decode().strip(),out.read_text())
if __name__=='__main__':unittest.main()
