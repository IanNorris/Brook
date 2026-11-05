#!/usr/bin/env python3
"""Exercise the recovery staging file primitives against a disposable directory."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/hardware_ssd_recovery_stage.c'
HARNESS = r'''
#define main stage_main
#include "hardware_ssd_recovery_stage.c"
#undef main
int main(int argc, char **argv) {
    if(argc!=3)return 10;
    uint8_t *data=NULL;size_t n=0;
    if(load(argv[1],1,1024,&data,&n))return 11;
    int d=open(argv[2],O_RDONLY|O_DIRECTORY);if(d<0)return 12;
    if(create_checked(d,"KERNEL.ELF",data,n))return 13;
    /* Existing files must never be overwritten. */
    if(!create_checked(d,"KERNEL.ELF",data,n))return 14;
    int fd=openat(d,"KERNEL.ELF",O_RDONLY);char buf[1024];
    if(fd<0||exact_read(fd,buf,n)||memcmp(buf,data,n))return 15;
    close(fd);close(d);free(data);return 0;
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path / 'test.c').write_text(HARNESS)
    (path / 'payload').write_bytes(b'Brook recovery staging\0' * 10)
    (path / 'target').mkdir()
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'tools'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test'), str(path / 'payload'), str(path / 'target')], check=True)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', str(SOURCE)], check=True)
print('recovery staging file primitives: PASS')
