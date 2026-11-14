#!/usr/bin/env python3
"""Exercise the guarded-loader promotion on a private host filesystem."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static int fake_open(const char *path,int flags,...) {
    mode_t mode=0; if(flags&O_CREAT){va_list v;va_start(v,flags);mode=va_arg(v,int);va_end(v);}
    if(!strcmp(path,"/media/system"))path=getenv("TEST_ROOT");
    else if(!strcmp(path,"/tmp/hardware-update.lock"))path=getenv("TEST_LOCK");
    return open(path,flags,mode);
}
static int fake_ioctl(int fd,unsigned long request,...){(void)fd;(void)request;return 0;}
static int fake_unlink(const char *path){
    if(!strcmp(path,"/tmp/hardware-update.lock"))path=getenv("TEST_LOCK");
    return unlink(path);
}
static int fake_renameat(int a,const char *b,int c,const char *d){
    static unsigned count; ++count;
    const char *fail=getenv("TEST_FAIL_RENAME");
    if(fail && count==(unsigned)atoi(fail)){errno=EIO;return -1;}
    return renameat(a,b,c,d);
}
#define open fake_open
#define ioctl fake_ioctl
#define renameat fake_renameat
#define unlink fake_unlink
#define geteuid() 0
#include "hardware_guard_default_stage.c"
'''

def sha(data):
    return hashlib.sha256(data).hexdigest()

def fake_efi(tag):
    """Minimal bytes that pass the tools' PE32+ x86-64 shape check."""
    data=bytearray(512); data[0:2]=b'MZ'; data[0x3c:0x40]=(0x80).to_bytes(4,'little')
    data[0x80:0x84]=b'PE\0\0'; data[0x84:0x86]=b'\x64\x86'; data[0x98:0x9a]=b'\x0b\x02'
    data[0x100:0x100+len(tag)]=tag
    return bytes(data)

with tempfile.TemporaryDirectory(prefix='brook-default-stage-') as td:
    base = Path(td)
    (base/'test.c').write_text(HARNESS)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools'),
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    old,new=fake_efi(b'old'),fake_efi(b'new')
    assert old != new
    for fail in ('0','1','2'):
        root=base/f'root-{fail}'
        boot=root/'EFI/BOOT'; rec=root/'RECOV2'; legacy=root/'RECOVERY'
        for p in (boot,rec,legacy): p.mkdir(parents=True,exist_ok=True)
        values=[old,new,old,b'kernel',b'initrd',b'selector',b'g'*128,b'g'*128]
        paths=[boot/'BOOTX64.EFI',boot/'BROOKG2.EFI',boot/'BROOKF5.EFI',rec/'KERNEL.ELF',
               rec/'INITRD.IMG',rec/'SELECT.BIN',legacy/'GUARD0.BIN',legacy/'GUARD1.BIN']
        for p,v in zip(paths,values):p.write_bytes(v)
        env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(base/f'lock-{fail}'),'TEST_FAIL_RENAME':fail}
        run=subprocess.run([str(base/'test'),*(sha(v) for v in values)],env=env,capture_output=True,text=True)
        assert (run.returncode==0)==(fail=='0'),(fail,run.stderr,[(p.name,sha(p.read_bytes())) for p in boot.iterdir()])
        assert (boot/'BROOKF5.EFI').read_bytes()==old
        assert (boot/'BROOKG2.EFI').read_bytes()==new
        if fail=='0':
            assert (boot/'BOOTX64.EFI').read_bytes()==new
            assert (boot/'BROOKOLD.EFI').read_bytes()==old
            assert not (base/f'lock-{fail}').exists()
        elif fail=='1':
            assert (boot/'BOOTX64.EFI').read_bytes()==old
            assert (boot/'BROOKNEW.EFI').read_bytes()==new
            assert (base/f'lock-{fail}').exists()
        else:
            assert not (boot/'BOOTX64.EFI').exists()
            assert (boot/'BROOKOLD.EFI').read_bytes()==old
            assert (boot/'BROOKNEW.EFI').read_bytes()==new
            assert (base/f'lock-{fail}').exists()
    # Wrong payload hashes fail before any filesystem mutation.
    root=base/'root-bad';shutil.copytree(base/'root-1',root)
    (root/'EFI/BOOT/BROOKNEW.EFI').unlink()
    env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(base/'lock-bad')}
    bad=['0'*64]+[sha(v) for v in values[1:]]
    run=subprocess.run([str(base/'test'),*bad],env=env,capture_output=True,text=True)
    assert run.returncode!=0 and (root/'EFI/BOOT/BOOTX64.EFI').read_bytes()==old
    assert not (root/'EFI/BOOT/BROOKNEW.EFI').exists() and not (base/'lock-bad').exists()
    # A later promotion preserves the original backup and staged guard loader.
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DBROOK_GUARD_REPROMOTE',
                    '-I',str(ROOT/'tools'),str(base/'test.c'),'-o',str(base/'repromote')],check=True)
    for fail in ('0','1','2'):
        root=base/f'repromote-{fail}';boot=root/'EFI/BOOT';rec=root/'RECOV2';legacy=root/'RECOVERY'
        for p in (boot,rec,legacy):p.mkdir(parents=True,exist_ok=True)
        values=[new,old,old,b'kernel',b'initrd',b'selector',b'g'*128,b'g'*128,old,new]
        paths=[boot/'BOOTX64.EFI',boot/'BROOKV5.EFI',boot/'BROOKF5.EFI',rec/'KERNEL.ELF',
               rec/'INITRD.IMG',rec/'SELECT.BIN',legacy/'GUARD0.BIN',legacy/'GUARD1.BIN',
               boot/'BROOKOLD.EFI',boot/'BROOKG2.EFI']
        for p,v in zip(paths,values):p.write_bytes(v)
        lock=base/f'repromote-lock-{fail}'
        env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(lock),'TEST_FAIL_RENAME':fail}
        run=subprocess.run([str(base/'repromote'),*(sha(v) for v in values)],env=env,capture_output=True,text=True)
        assert (run.returncode==0)==(fail=='0'),(fail,run.stderr)
        assert (boot/'BROOKOLD.EFI').read_bytes()==old and (boot/'BROOKG2.EFI').read_bytes()==new
        assert (boot/'BROOKF5.EFI').read_bytes()==old and (boot/'BROOKV5.EFI').read_bytes()==old
        if fail=='0':
            assert (boot/'BOOTX64.EFI').read_bytes()==old and (boot/'BROOKV4.EFI').read_bytes()==new and not lock.exists()
        elif fail=='1':
            assert (boot/'BOOTX64.EFI').read_bytes()==new and (boot/'BROOKNEW.EFI').read_bytes()==old and lock.exists()
        else:
            assert not (boot/'BOOTX64.EFI').exists() and (boot/'BROOKV4.EFI').read_bytes()==new and lock.exists()
    root=base/'repromote-bad';shutil.copytree(base/'repromote-1',root)
    (root/'EFI/BOOT/BROOKNEW.EFI').unlink()
    env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(base/'repromote-lock-bad'),'TEST_FAIL_RENAME':'0'}
    hashes=[sha(v) for v in values];hashes[8]='0'*64
    run=subprocess.run([str(base/'repromote'),*hashes],env=env,capture_output=True,text=True)
    assert run.returncode!=0 and (root/'EFI/BOOT/BOOTX64.EFI').read_bytes()==new
    assert not (root/'EFI/BOOT/BROOKNEW.EFI').exists() and not (base/'repromote-lock-bad').exists()
    # The next generation: stage BROOKT6.EFI, keep the v5 default as BROOKP5.EFI, preserve v4/v5 copies.
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DBROOK_GUARD_REPROMOTE_NEXT',
                    '-I',str(ROOT/'tools'),str(base/'test.c'),'-o',str(base/'next')],check=True)
    v4,v5,t6=fake_efi(b'v4'),fake_efi(b'v5'),fake_efi(b't6')
    def next_root(name):
        root=base/name;boot=root/'EFI/BOOT';rec=root/'RECOV2';legacy=root/'RECOVERY'
        for p in (boot,rec,legacy):p.mkdir(parents=True,exist_ok=True)
        values=[v5,t6,old,b'kernel',b'initrd',b'selector',b'g'*128,b'g'*128,old,v4,v4,v5]
        paths=[boot/'BOOTX64.EFI',boot/'BROOKT6.EFI',boot/'BROOKF5.EFI',rec/'KERNEL.ELF',
               rec/'INITRD.IMG',rec/'SELECT.BIN',legacy/'GUARD0.BIN',legacy/'GUARD1.BIN',
               boot/'BROOKOLD.EFI',boot/'BROOKG2.EFI',boot/'BROOKV4.EFI',boot/'BROOKV5.EFI']
        for p,v in zip(paths,values):p.write_bytes(v)
        return root,boot,values
    def preserved(boot):
        assert (boot/'BROOKOLD.EFI').read_bytes()==old and (boot/'BROOKG2.EFI').read_bytes()==v4
        assert (boot/'BROOKV4.EFI').read_bytes()==v4 and (boot/'BROOKV5.EFI').read_bytes()==v5
        assert (boot/'BROOKF5.EFI').read_bytes()==old and (boot/'BROOKT6.EFI').read_bytes()==t6
    for fail in ('0','1','2'):
        root,boot,values=next_root(f'next-{fail}');lock=base/f'next-lock-{fail}'
        env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(lock),'TEST_FAIL_RENAME':fail}
        run=subprocess.run([str(base/'next'),*(sha(v) for v in values)],env=env,capture_output=True,text=True)
        assert (run.returncode==0)==(fail=='0'),(fail,run.stderr)
        preserved(boot)
        if fail=='0':
            assert (boot/'BOOTX64.EFI').read_bytes()==t6 and (boot/'BROOKP5.EFI').read_bytes()==v5
            assert not (boot/'BROOKNEW.EFI').exists() and not lock.exists() and 'GUARD_DEFAULT_OK' in run.stdout
        elif fail=='1':
            assert (boot/'BOOTX64.EFI').read_bytes()==v5 and (boot/'BROOKNEW.EFI').read_bytes()==t6
            assert not (boot/'BROOKP5.EFI').exists() and lock.exists() and 'mutated=1' in run.stderr
        else:
            assert not (boot/'BOOTX64.EFI').exists() and (boot/'BROOKP5.EFI').read_bytes()==v5
            assert (boot/'BROOKNEW.EFI').read_bytes()==t6 and lock.exists()
    # Each wrong hash (including the preserved v4/v5 copies) fails before mutation.
    for index in range(12):
        root,boot,values=next_root(f'next-bad-{index}');lock=base/f'next-bad-lock-{index}'
        hashes=[sha(v) for v in values];hashes[index]='0'*64
        env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(lock),'TEST_FAIL_RENAME':'0'}
        run=subprocess.run([str(base/'next'),*hashes],env=env,capture_output=True,text=True)
        assert run.returncode!=0 and 'mutated=0' in run.stderr,(index,run.stderr)
        assert (boot/'BOOTX64.EFI').read_bytes()==v5 and not (boot/'BROOKNEW.EFI').exists()
        assert not (boot/'BROOKP5.EFI').exists() and not lock.exists()
        preserved(boot)
    # An existing backup name, a leftover BROOKNEW.EFI, a missing v4 copy or differing guards refuse.
    for case in ('backup','leftover','missing-v4','guards'):
        root,boot,values=next_root(f'next-{case}');lock=base/f'next-lock-{case}'
        if case=='backup':(boot/'BROOKP5.EFI').write_bytes(v4)
        elif case=='leftover':(boot/'BROOKNEW.EFI').write_bytes(t6)
        elif case=='missing-v4':(boot/'BROOKV4.EFI').unlink()
        else:
            (root/'RECOVERY/GUARD1.BIN').write_bytes(b'h'*128);values[7]=b'h'*128
        env=os.environ|{'TEST_ROOT':str(root),'TEST_LOCK':str(lock),'TEST_FAIL_RENAME':'0'}
        run=subprocess.run([str(base/'next'),*(sha(v) for v in values)],env=env,capture_output=True,text=True)
        assert run.returncode!=0 and 'mutated=0' in run.stderr,(case,run.stderr)
        assert (boot/'BOOTX64.EFI').read_bytes()==v5 and not lock.exists(),case
print('guard default staging host transaction: PASS')
