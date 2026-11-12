#!/usr/bin/env python3
"""Exercise recovery promotion and rename failures on a private host tree."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static int fake_open(const char *path,int flags,...) {
    mode_t mode=0;if(flags&O_CREAT){va_list v;va_start(v,flags);mode=va_arg(v,int);va_end(v);}
    if(!strcmp(path,"/media/system"))path=getenv("TEST_ROOT");
    else if(!strcmp(path,"/tmp/hardware-update.lock"))path=getenv("TEST_LOCK");
    return open(path,flags,mode);
}
static int fake_ioctl(int fd,unsigned long request,...){(void)fd;(void)request;return 0;}
static int fake_unlink(const char *path){if(!strcmp(path,"/tmp/hardware-update.lock"))path=getenv("TEST_LOCK");return unlink(path);}
static int fake_renameat(int a,const char *b,int c,const char *d){static int count;
    if(++count==atoi(getenv("TEST_FAIL_RENAME"))){errno=EIO;return -1;}return renameat(a,b,c,d);}
#define open fake_open
#define ioctl fake_ioctl
#define renameat fake_renameat
#define unlink fake_unlink
#define geteuid() 0
#include "hardware_recovery_promote.c"
'''
pair=ROOT/'build/hardware/dell-torn-journal-recovery-candidate-20261105'
names=('KERNEL.ELF','INITRD.IMG','SELECT.BIN')
hashes=[hashlib.sha256((pair/n).read_bytes()).hexdigest() for n in names]
with tempfile.TemporaryDirectory(prefix='brook-recovery-promote-') as tmp:
    base=Path(tmp);(base/'test.c').write_text(HARNESS)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    for fail in (0,1,2):
        root=base/f'root-{fail}'
        for directory in ('RECOV2','RECOVNEW','RECOVERY'):
            target=root/directory;target.mkdir(parents=True)
            for name in names:shutil.copyfile(pair/name,target/name)
        lock=base/f'lock-{fail}'
        env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(lock),'TEST_FAIL_RENAME':str(fail)}
        run=subprocess.run([str(base/'test'),*(hashes*3)],env=env,capture_output=True,text=True)
        assert (run.returncode==0)==(fail==0),run.stderr
        assert (root/'RECOVERY/SELECT.BIN').read_bytes()==(pair/'SELECT.BIN').read_bytes()
        assert (root/'RECOVNEW').exists()==(fail!=0)
        assert (root/'RECOVBAK').exists()==(fail!=1)
        assert (root/'RECOV2').exists()==(fail!=2)
        assert lock.exists()==(fail!=0)
    root=base/'root-bad';root.mkdir()
    for directory in ('RECOV2','RECOVNEW','RECOVERY'):
        target=root/directory;target.mkdir()
        for name in names:shutil.copyfile(pair/name,target/name)
    env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(base/'lock-bad'),'TEST_FAIL_RENAME':'0'}
    bad=hashes*3;bad[3]='0'*64
    assert subprocess.run([str(base/'test'),*bad],env=env,capture_output=True).returncode!=0
    assert (root/'RECOV2').exists() and (root/'RECOVNEW').exists() and not (base/'lock-bad').exists()
print('recovery promotion host transaction: PASS')
