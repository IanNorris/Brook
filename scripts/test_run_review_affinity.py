#!/usr/bin/env python3
"""Verify review-launch affinity inheritance using real threads and a child."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ReviewAffinityTest(unittest.TestCase):
    def test_launch_threads_and_child_inherit_selected_cpu(self):
        script = Path(__file__).with_name('run_review.sh').resolve()
        selected = min(os.sched_getaffinity(0))
        with tempfile.TemporaryDirectory() as folder:
            review = Path(folder)
            launcher = review / 'launch.sh'
            launcher.write_text('''#!/usr/bin/env python3
import json, os, subprocess, sys, threading
from pathlib import Path
result = {'main': sorted(os.sched_getaffinity(0)),
          'tmpdir_exists': Path(os.environ['TMPDIR']).is_dir()}
def worker():
    result['thread'] = sorted(os.sched_getaffinity(0))
thread = threading.Thread(target=worker)
thread.start()
thread.join()
result['child'] = json.loads(subprocess.check_output([
    sys.executable, '-c',
    'import json,os;print(json.dumps(sorted(os.sched_getaffinity(0))))']))
print(json.dumps(result))
sys.exit(37)
''')
            launcher.chmod(0o755)
            env = dict(os.environ, BROOK_REVIEW_CPUSET=str(selected))
            result = subprocess.run([str(script), folder], env=env,
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 37, result.stderr)
            value = json.loads(result.stdout)
            self.assertTrue(value['tmpdir_exists'])
            for kind in ('main', 'thread', 'child'):
                self.assertEqual(value[kind], [selected], kind)


if __name__ == '__main__':
    unittest.main()
