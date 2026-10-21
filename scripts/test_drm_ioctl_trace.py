#!/usr/bin/env python3
"""Exercise the real preload probes against a deterministic fake libdrm/SDL."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from workloads.analyze_drm_trace import analyze


class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='brook-drm-probe-')
        cls.root = Path(cls.temp.name)
        repo = Path(__file__).resolve().parents[1]
        def compile(output, *args):
            subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                            *map(str, args), '-o', str(cls.root/output)], check=True)
        compile('drm.so', '-shared', '-fPIC', repo/'tools/profiling/drm_ioctl_trace.c', '-ldl')
        compile('swap.so', '-shared', '-fPIC', repo/'tools/profiling/sdl_swap_trace.c', '-ldl')
        mock = cls.root/'mock.c'
        mock.write_text('''
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
int mock_started;
int SDL_GL_GetSwapInterval(void) { return -1; }
int drmIoctl(int fd, unsigned long request, void *arg) {
 (void)fd; (void)arg;
 if(request==0x12345678) { __atomic_store_n(&mock_started,1,__ATOMIC_RELEASE); usleep(1000000); }
 usleep(50);
 if (request==0x20) { errno=EBUSY; return -1; }
 errno=EAGAIN; return 7;
}
void SDL_GL_SwapWindow(void *w) {
 (void)w;
 int (*invoke)(int,unsigned long,void*)=dlsym(RTLD_DEFAULT,"drmIoctl");
 int r=invoke?invoke(1,0x20,0):drmIoctl(1,0x20,0);
 if(r!=-1 || errno!=EBUSY) _exit(90);
}
''')
        compile('mock.so', '-shared', '-fPIC', mock)
        compile('libdrm.so.2', '-shared', '-fPIC', mock)
        main = cls.root/'main.c'
        main.write_text('''
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
extern int drmIoctl(int,unsigned long,void*);
extern void SDL_GL_SwapWindow(void*);
extern int mock_started;
static void *worker(void *arg) {
 (void)arg;
 for(int i=0;i<100;i++) if(drmIoctl(1,0x30,0)!=7 || errno!=EAGAIN) _exit(91);
 return 0;
}
static void *blocked(void *arg) { (void)arg; drmIoctl(1,0x12345678,0); return 0; }
int main(int argc,char **argv) {
 (void)argc;
 if(argv[1][0]=='s') {
  for(int i=0;i<5;i++) {
   if(drmIoctl(1,0x10,0)!=7 || errno!=EAGAIN) return 92;
   SDL_GL_SwapWindow(0);
  }
 } else if(argv[1][0]=='t') {
  pthread_t t[16];
  for(int i=0;i<16;i++) if(pthread_create(&t[i],0,worker,0)) return 93;
  for(int i=0;i<16;i++) if(pthread_join(t[i],0)) return 94;
 } else if(argv[1][0]=='i') {
  pthread_t t;
  if(pthread_create(&t,0,blocked,0)) return 95;
  while(!__atomic_load_n(&mock_started,__ATOMIC_ACQUIRE)) usleep(100);
 } else {
  for(unsigned i=1;i<=130;i++) drmIoctl(1,i,0);
 }
 return 0;
}
''')
        compile('main', main, cls.root/'mock.so', '-pthread')
        local = cls.root/'local.c'
        local.write_text('''
#include <dlfcn.h>
int main(int argc,char **argv) {
 (void)argc;
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!lib) return 96;
 void (*swap)(void*)=(void (*)(void*))dlsym(lib,"SDL_GL_SwapWindow");
 if(!swap) return 97;
 for(int i=0;i<5;i++) swap(0);
 dlclose(lib);
 return 0;
}
''')
        compile('local', local, '-ldl')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, mode, enabled=True):
        path = self.root/(self._testMethodName+'.csv')
        env = dict(os.environ, LD_PRELOAD=str(self.root/'swap.so')+':'+str(self.root/'drm.so'),
                   BROOK_DRM_TRACE_SKIP_SWAPS='2')
        env.pop('BROOK_SWAP_TRACE', None)
        env.pop('BROOK_DRM_TRACE', None)
        if enabled:
            env['BROOK_DRM_TRACE'] = str(path)
        subprocess.run([str(self.root/'main'), mode], env=env, check=True, timeout=15)
        return path

    def test_swap_phases_and_errno(self):
        report = analyze(self.run_probe('swap').read_text())
        rows = {(v['phase'], v['request']): v for v in report['rows']}
        self.assertEqual({k: v['calls'] for k, v in rows.items()},
                         {(0, 0x10): 3, (0, 0x20): 2, (1, 0x10): 2, (2, 0x20): 3})
        self.assertEqual(sum(v['errors'] for v in rows.values()), 5)
        for row in rows.values():
            self.assertLessEqual(row['p50_bucket_ns'][0], row['p50_bucket_ns'][1])

    def test_concurrent_threads_remain_unscoped(self):
        report = analyze(self.run_probe('threads').read_text())
        self.assertEqual(len(report['rows']), 16)
        self.assertEqual(len({v['tid'] for v in report['rows']}), 16)
        self.assertTrue(all(v['phase']==0 and v['calls']==100 for v in report['rows']))

    def test_late_local_library(self):
        path = self.root/'local.csv'
        env = dict(os.environ, LD_PRELOAD=str(self.root/'drm.so'), BROOK_DRM_TRACE=str(path))
        subprocess.run([str(self.root/'local'), str(self.root/'libdrm.so.2')],
                       env=env, check=True, timeout=15)
        rows = analyze(path.read_text())['rows']
        self.assertEqual(len(rows), 1)
        self.assertEqual((rows[0]['phase'], rows[0]['calls'], rows[0]['errors']), (0, 5, 5))

    def test_actual_swap_interval_metadata(self):
        path = self.root/'interval.json'
        env = dict(os.environ, LD_PRELOAD=str(self.root/'swap.so'),
                   BROOK_SWAP_TRACE=str(self.root/'interval-swaps.csv'), BROOK_SWAP_SETTINGS=str(path))
        subprocess.run([str(self.root/'main'), 'swap'], env=env, check=True, timeout=15)
        self.assertEqual(json.loads(path.read_text()), dict(sdl_abi=2, getter_available=True,
                         queries=5, interval_min=-1, interval_max=-1, incomplete=0, dropped=0))

    def test_slot_overflow_rejected(self):
        with self.assertRaisesRegex(ValueError, 'overflowed'):
            analyze(self.run_probe('overflow').read_text())

    def test_disabled_preserves_calls_and_writes_nothing(self):
        self.assertFalse(self.run_probe('swap', enabled=False).exists())

    def test_corrupt_histogram_rejected(self):
        text = self.run_probe('swap').read_text()
        lines = text.splitlines()
        fields = lines[2].split(',')
        fields[-1] = ';'.join(['0']*64)
        lines[2] = ','.join(fields)
        with self.assertRaises(ValueError):
            analyze('\n'.join(lines))

    def test_incomplete_capture_rejected(self):
        text = self.run_probe('inflight').read_text()
        self.assertIn('incomplete=1', text.splitlines()[0])
        with self.assertRaises(ValueError):
            analyze(text)


if __name__ == '__main__':
    unittest.main()
