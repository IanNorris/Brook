#!/usr/bin/env python3
"""Check build.sh failure reporting using fake tools; never build a kernel."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BuildReporting(unittest.TestCase):
    def run_build(self, failure):
        with tempfile.TemporaryDirectory(prefix='brook-build-report-') as directory:
            root = Path(directory)
            for name in ('scripts', 'vendor/uefi-headers/Include',
                         'src/third_party/fatfs/source', 'fake-bin'):
                (root/name).mkdir(parents=True)
            (root/'vendor/uefi-headers/Include/Uefi.h').touch()
            (root/'src/third_party/fatfs/source/ff.c').touch()
            shutil.copy2(Path(__file__).with_name('build.sh'), root/'scripts/build.sh')
            cmake = root/'fake-bin/cmake'
            cmake.write_text('''#!/usr/bin/env bash
case " $* " in
  *host_tests*)
    if [[ "$1" == --build && "$BROOK_TEST_FAILURE" == build ]]; then
      echo HOST_BUILD_SENTINEL; exit 19
    elif [[ "$1" != --build && "$BROOK_TEST_FAILURE" == configure ]]; then
      echo HOST_CONFIGURE_SENTINEL; exit 18
    fi ;;
esac
exit 0
''')
            cmake.chmod(0o755)
            # Do not execute the real audit: this test covers script control flow.
            audit = root/'fake-bin/python3'
            audit.write_text('#!/usr/bin/env bash\nexit 0\n')
            audit.chmod(0o755)
            env = dict(os.environ, PATH=str(root/'fake-bin')+':'+os.environ['PATH'],
                       BROOK_TEST_FAILURE=failure)
            return subprocess.run(['bash', str(root/'scripts/build.sh'), 'Debug'],
                                  env=env, text=True, capture_output=True)

    def test_host_configure_failure_is_not_success(self):
        result = self.run_build('configure')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('HOST_CONFIGURE_SENTINEL', result.stderr)
        self.assertNotIn('Done. Artifacts', result.stdout)

    def test_host_build_failure_is_not_success(self):
        result = self.run_build('build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('HOST_BUILD_SENTINEL', result.stderr)
        self.assertNotIn('Done. Artifacts', result.stdout)

    def test_success_still_reports_completion(self):
        result = self.run_build('none')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Host tests built.', result.stdout)
        self.assertIn('Done. Artifacts', result.stdout)


if __name__ == '__main__':
    unittest.main()
