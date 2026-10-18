"""Production link publication: old/new readers, failed syscalls and killed writer.

ENOSPC is injected at symlink, not claimed as a whole-disk-full simulation.
The stopped/killed writer uses real staged symlinks and rename on disk.
"""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class ProfileLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='brook-link-build-')
        cls.binary = str(Path(cls.build.name)/'link-driver')
        source = Path(cls.build.name)/'driver.c'
        source.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fail_stage,gate,fail_flush;
static int staged_symlink(const char *target,const char *path) {
    if(fail_stage) {errno=ENOSPC;return -1;}return symlink(target,path);
}
static int gated_rename(const char *from,const char *to) {
    if(gate) {puts("STAGED");fflush(stdout);if(getchar()==EOF)return -1;}
    return rename(from,to);
}
static int checked_fsync(int fd) {
    if(fail_flush) {errno=EIO;return -1;}return fsync(fd);
}
#define symlink staged_symlink
#define rename gated_rename
#define fsync checked_fsync
#include "profile_link.h"
#undef symlink
#undef rename
#undef fsync
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    fail_stage=!strcmp(argv[3],"enospc");gate=!strcmp(argv[3],"gate");
    fail_flush=!strcmp(argv[3],"flush");
    int result=0;
    if(!strcmp(argv[3],"loop")) {
        for(int i=0;i<500;++i) {
            if(profile_link_replace(i%2?"old":"new",argv[2])<0)return 1;
        }
    } else result=profile_link_replace(argv[1],argv[2]);
    if(result<0) {perror("link");return 1;}printf("CHANGED=%d\n",result);return 0;
}
''')
        subprocess.run([os.environ.get('HOST_CC','cc'),'-O2','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT),str(source),'-o',cls.binary],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='brook-link-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = self.root/'app'
        self.path.symlink_to('old')

    def run_link(self, mode='normal', target='new', success=True):
        result = subprocess.run([self.binary,target,str(self.path),mode],capture_output=True)
        self.assertEqual(result.returncode==0,success,result.stderr)
        self.assertEqual(list(self.root.glob('.brook-link.*')),[])
        return result

    def test_replace_and_same_target_noop(self):
        self.assertIn(b'CHANGED=1',self.run_link().stdout)
        inode=self.path.lstat().st_ino
        self.assertIn(b'CHANGED=0',self.run_link().stdout)
        self.assertEqual(self.path.lstat().st_ino,inode)

    def test_enospc_during_staging_preserves_existing_link(self):
        self.run_link('enospc',success=False)
        self.assertEqual(os.readlink(self.path),'old')

    def test_rename_failure_preserves_directory_and_cleans_staged_link(self):
        self.path.unlink();self.path.mkdir();(self.path/'keep').write_text('unchanged')
        self.run_link(success=False)
        self.assertEqual((self.path/'keep').read_text(),'unchanged')

    def test_flush_failure_reports_publication_already_happened(self):
        self.run_link('flush',success=False)
        self.assertEqual(os.readlink(self.path),'new')

    def test_long_destination_name(self):
        self.path=self.root/('a'*255)
        self.path.symlink_to('old')
        self.run_link()
        self.assertEqual(os.readlink(self.path),'new')

    def test_regular_destination_replaced_without_following_symlink(self):
        self.path.unlink();self.path.write_text('old contents')
        self.run_link()
        self.assertEqual(os.readlink(self.path),'new')

    def test_readers_only_observe_complete_old_or_new_link(self):
        process=subprocess.Popen([self.binary,'new',str(self.path),'loop'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            while process.poll() is None:
                self.assertIn(os.readlink(self.path),('old','new'))
            output=process.communicate(timeout=10)
            self.assertEqual(process.returncode,0,output)
        finally:
            if process.poll() is None:process.kill();process.communicate()
        self.assertEqual(list(self.root.glob('.brook-link.*')),[])

    def test_killed_staged_writer_preserves_live_link_and_retry(self):
        process=subprocess.Popen([self.binary,'new',str(self.path),'gate'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            self.assertEqual(process.stdout.readline(),b'STAGED\n')
            self.assertEqual(os.readlink(self.path),'old')
            staged=list(self.root.glob('.brook-link.*'));self.assertEqual(len(staged),1)
            process.kill();process.communicate(timeout=10)
            self.assertEqual(process.returncode,-signal.SIGKILL)
            self.assertEqual(os.readlink(self.path),'old')
            # A crash can leave its private temporary name. Retry must ignore it,
            # never interpret it as live publication or delete it indiscriminately.
            retry=subprocess.run([self.binary,'new',str(self.path),'normal'],capture_output=True)
            self.assertEqual(retry.returncode,0,retry.stderr)
            self.assertEqual(os.readlink(self.path),'new')
            self.assertEqual(list(self.root.glob('.brook-link.*')),staged)
        finally:
            if process.poll() is None:process.kill()
            process.communicate()


if __name__=='__main__':
    unittest.main()
