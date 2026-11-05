#!/usr/bin/env python3
"""Check guard provisioning primitives without touching a guest disk."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#define main guard_provision_main
#include "hardware_guard_provision.cpp"
#undef main
int main(int argc,char **argv) {
    if(argc!=2)return 1;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY);struct stat st;
    if(dir<0||fstat(dir,&st))return 2;
    uint8_t record[128]={};memcpy(record,"BRKRCV01",8);
    BrookUpdatePut64(record+8,1);BrookUpdatePut32(record+16,BROOK_RECOVERY_GUARD_CLEAN);
    for(unsigned i=24;i<72;++i)record[i]=(uint8_t)i;
    BrookUpdatePut32(record+124,BrookUpdateCrc(record,124));
    if(create_guard(dir,"GUARD0.BIN",record,st.st_dev))return 3;
    record[24]^=1;
    if(!create_guard(dir,"GUARD0.BIN",record,st.st_dev))return 4;
    int fd=openat(dir,"GUARD0.BIN",O_RDONLY);uint8_t saved[128];
    if(fd<0||read(fd,saved,sizeof(saved))!=128||saved[24]!=(uint8_t)24)return 5;
    close(fd);close(dir);
    if(!hex32("0123456789abcdef0123456789abcdef") || hex32("bad") ||
       !range("2048,4096") || range("2048,") || range("1,2,3"))return 6;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='brook-guard-provision-') as temporary:
    path=Path(temporary);(path/'test.cpp').write_text(HARNESS);(path/'target').mkdir()
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools'),
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test'),str(path/'target')],check=True)
print('guard provisioning primitives: PASS')
