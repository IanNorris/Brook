"""Run real installer/fetcher/extractor processes against a gated local cache."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import unittest
from cancel_fixture import Cache, ROOT_HASH, ROOT_NAME, DEP_NAME, PAYLOAD

ROOT = Path(__file__).resolve().parents[2]


class CancellationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='brook-cancel-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.profile = self.root/'profile'
        self.profile.mkdir()
        self.manifest = self.profile/'manifest.tsv'
        self.original = b'original\t1\tbrook-regression-missing-original\n'
        self.manifest.write_bytes(self.original)
        self.store = self.root/'store'
        self.store.mkdir()
        self.cache = Cache()
        self.thread = threading.Thread(target=self.cache.serve_forever, daemon=True)
        self.thread.start()
        def stop():
            self.cache.release.set()
            self.cache.shutdown(); self.cache.server_close(); self.thread.join()
        self.addCleanup(stop)
        self.environment = dict(os.environ,NIX_CACHE_URL=f'http://127.0.0.1:{self.cache.server_port}',
                                NIX_STORE=str(self.store),CURL_PATH=shutil.which('curl'),
                                XZ_PATH=shutil.which('xz'),NAR_UNPACK_PATH=str(self.root/'nar-unpack'))
        cc = os.environ.get('HOST_CC','cc')
        for name in ('nix-fetch','nar-unpack'):
            definitions = [f'-DNARINFO_CACHE_DIR="{self.root}/narinfos"'] if name=='nix-fetch' else []
            subprocess.run([cc,'-O2',*definitions,str(ROOT/'tools'/name/(name+'.c')),'-o',str(self.root/name)],check=True)
        mapping = dict(INDEX_PATH='index',PROFILE_DIR='profile',MANIFEST_PATH='profile/manifest.tsv',
                       PROFILE_BIN='profile/bin',PROFILE_LIB='profile/lib',PROFILE_APPS='profile/share/applications',
                       APPS_DIR='apps',APPS_IDX='apps/applications.idx',APPS_ICONS_DIR='apps/icons',NIX_FETCH_PATH='nix-fetch')
        definitions = [f'-D{name}="{self.root/path}"' for name,path in mapping.items()]
        self.installer = str(self.root/'nix')
        subprocess.run([cc,'-O2',*definitions,str(ROOT/'tools/nix-install/nix-install.c'),'-o',self.installer],check=True)
        (self.root/'index').write_text(f'fixture\t1\t{ROOT_NAME}\tCooperative cancellation fixture\n')

    def launch(self):
        p = subprocess.Popen([self.installer,'install','fixture'],env=self.environment,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        def clean():
            self.cache.release.set()
            if p.poll() is None:
                p.send_signal(signal.SIGUSR1)
            p.communicate(timeout=20)
        self.addCleanup(clean)
        return p

    def test_cancel_finishes_current_archive_preserves_profile_and_can_retry(self):
        p = self.launch()
        self.assertTrue(self.cache.started.wait(15),'download never reached archive gate')
        p.send_signal(signal.SIGUSR1)
        time.sleep(.2)
        self.assertIsNone(p.poll(),'cancellation killed an active extraction')
        self.assertEqual(self.manifest.read_bytes(),self.original)
        self.assertTrue((self.profile/'.mutation-lock').exists())
        self.cache.release.set()
        output = p.communicate(timeout=20)[0]
        self.assertEqual(p.returncode,130,output)
        self.assertFalse((self.profile/'.mutation-lock').exists())
        self.assertEqual(self.manifest.read_bytes(),self.original)
        self.assertEqual((self.store/DEP_NAME/'proof.txt').read_bytes(),PAYLOAD)
        self.assertFalse((self.store/ROOT_NAME).exists())
        self.assertEqual(list(self.store.glob('.*.tmp.*')),[])
        with self.cache.lock:
            self.assertEqual(self.cache.requests['/root.nar'],0)
        retry = subprocess.run([self.installer,'install','fixture'],env=self.environment,capture_output=True,timeout=20)
        self.assertEqual(retry.returncode,0,retry.stdout+retry.stderr)
        self.assertEqual((self.store/ROOT_NAME/'proof.txt').read_bytes(),PAYLOAD)
        self.assertEqual(self.manifest.read_bytes(),self.original+f'fixture\t1\t{ROOT_NAME}\n'.encode())
        with self.cache.lock:
            self.assertEqual(self.cache.requests['/dependency.nar'],1,'retry discarded the completed dependency')

    def test_request_while_final_archive_runs_still_prevents_profile_publication(self):
        self.cache.gated_path='/root.nar'
        p = self.launch()
        self.assertTrue(self.cache.started.wait(15))
        p.send_signal(signal.SIGUSR1);time.sleep(.2)
        self.assertIsNone(p.poll())
        self.cache.release.set();output=p.communicate(timeout=20)[0]
        self.assertEqual(p.returncode,130,output)
        self.assertEqual(self.manifest.read_bytes(),self.original)
        self.assertEqual((self.store/ROOT_NAME/'proof.txt').read_bytes(),PAYLOAD)
        self.assertFalse((self.profile/'.mutation-lock').exists())

    def test_request_pending_across_exec_is_handled_before_lock_or_network(self):
        def block_request():
            signal.pthread_sigmask(signal.SIG_BLOCK,{signal.SIGUSR1})
        p=subprocess.Popen([self.installer,'install','fixture'],env=self.environment,
                           stdout=subprocess.PIPE,stderr=subprocess.STDOUT,preexec_fn=block_request)
        p.send_signal(signal.SIGUSR1)
        output=p.communicate(timeout=20)[0]
        self.assertEqual(p.returncode,130,output)
        self.assertEqual(self.manifest.read_bytes(),self.original)
        self.assertFalse((self.profile/'.mutation-lock').exists())
        self.assertEqual(sum(self.cache.requests.values()),0)

    def test_invalid_control_descriptor_fails_before_network(self):
        result=subprocess.run([str(self.root/'nix-fetch'),ROOT_HASH],env=dict(self.environment,BROOK_NIX_CANCEL_FD='1'),capture_output=True)
        self.assertEqual(result.returncode,1)
        self.assertIn(b'cancellation channel',result.stderr)
        self.assertEqual(sum(self.cache.requests.values()),0)

    def test_install_with_closed_stdin_keeps_control_pipe_private(self):
        self.cache.release.set()
        result=subprocess.run([self.installer,'install','fixture'],env=self.environment,
                              stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                              preexec_fn=lambda: os.close(0),timeout=20)
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertEqual((self.store/ROOT_NAME/'proof.txt').read_bytes(),PAYLOAD)
        self.assertIn(f'fixture\t1\t{ROOT_NAME}\n'.encode(),self.manifest.read_bytes())
        self.assertFalse((self.profile/'.mutation-lock').exists())


class CommitBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory(prefix='brook-cancel-boundary-')
        cls.root=Path(cls.directory.name)
        source=cls.root/'boundary.c'
        source.write_text(r'''#define _GNU_SOURCE
#include <sys/wait.h>
#include "manifest.h"
#include "cancel.h"
int main(int argc,char **argv) {
    if(argc!=3||install_cancel_enable()<0)return 2;
    sigset_t previous;
    if(argv[2][0]=='b') {
        puts("READY");fflush(stdout);getchar();
    }
    int result=install_commit_begin(&previous);
    if(result)return install_cancel_status(result);
    puts("COMMIT");fflush(stdout);getchar();
    result=manifest_update(argv[1],"fixture","1","hash-fixture");
    return install_cancel_status(install_commit_end(&previous,result<0?1:0));
}
''')
        cls.binary=str(cls.root/'boundary')
        subprocess.run([os.environ.get('HOST_CC','cc'),'-O2','-I',str(ROOT/'tools/nix-install'),str(source),'-o',cls.binary],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_cancel_before_publication_preserves_manifest(self):
        path=self.root/'before.tsv';path.write_text('old\t1\thash-old\n')
        p=subprocess.Popen([self.binary,str(path),'before'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        self.assertEqual(p.stdout.readline(),b'READY\n')
        p.send_signal(signal.SIGUSR1)
        output=p.communicate(b'x',timeout=10)[0]
        self.assertEqual(p.returncode,130,output)
        self.assertEqual(path.read_text(),'old\t1\thash-old\n')

    def test_late_request_allows_started_publication_to_finish(self):
        path=self.root/'during.tsv';path.write_text('old\t1\thash-old\n')
        p=subprocess.Popen([self.binary,str(path),'during'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        self.assertIn(b'Updating package profile',p.stdout.readline())
        self.assertEqual(p.stdout.readline(),b'COMMIT\n')
        p.send_signal(signal.SIGUSR1)
        output=p.communicate(b'x',timeout=10)[0]
        self.assertEqual(p.returncode,0,output)
        self.assertEqual(path.read_text(),'old\t1\thash-old\nfixture\t1\thash-fixture\n')


if __name__=='__main__':
    unittest.main()
