#!/usr/bin/env python3
"""Check fixed-journal exclusive creation and zero readback on a private directory."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#define main provision_main
#include "hardware_panic_journal_provision.cpp"
#undef main
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY);struct stat st{};
    if(dir<0||fstat(dir,&st)||create_journal(dir,st.st_dev))return 2;
    if(!create_journal(dir,st.st_dev))return 3;
    int fd=openat(dir,"PANIC.BIN",O_RDONLY);struct stat file{};
    if(fd<0||fstat(fd,&file)||file.st_size!=258*512)return 4;
    unsigned char buf[4096];ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0)for(ssize_t i=0;i<n;++i)if(buf[i])return 5;
    if(n<0)return 6;
    close(fd);close(dir);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='brook-journal-provision-') as tmp:
    root=Path(tmp);(root/'test.cpp').write_text(SOURCE);(root/'target').mkdir()
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools'),
                    str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test'),str(root/'target')],check=True)
print('panic journal provisioning primitives: PASS')
