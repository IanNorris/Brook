#!/usr/bin/env python3
"""Check first-time EFI staging primitives on a private host directory."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#define main stage_main
#include "hardware_efi_inventory_stage.c"
#undef main
int main(int argc,char **argv) {
    if(argc!=3)return 1;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY);struct stat st,src;
    int input=open(argv[2],O_RDONLY);
    if(dir<0||input<0||fstat(dir,&st)||fstat(input,&src)||src.st_size<=0)return 2;
    uint8_t *data=malloc((size_t)src.st_size),hash[32];
    if(!data||exact_read(input,data,(size_t)src.st_size)||!valid_efi(data,(size_t)src.st_size))return 3;
    if(decode_hash("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",hash)||
       !decode_hash("abc",hash))return 4;
    if(create_checked(dir,data,(size_t)src.st_size,st.st_dev))return 5;
    data[0]^=1;
    if(!create_checked(dir,data,(size_t)src.st_size,st.st_dev))return 6;
    int output=openat(dir,STAGE_EFI_NAME,O_RDONLY);uint8_t first;
    if(output<0||read(output,&first,1)!=1||first!='M')return 7;
    close(output);close(input);close(dir);free(data);return 0;
}
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('efi', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='brook-efi-stage-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(HARNESS)
    (path / 'target').mkdir()
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT/'tools'),
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test'), str(path/'target'),
                    str(args.efi.resolve(strict=True))], check=True)
    (path/'alternate').mkdir()
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DBROOK_EFI_GUARD_STAGE',
                    '-I', str(ROOT/'tools'),str(path/'test.c'), '-o', str(path/'test-alt')],check=True)
    subprocess.run([str(path/'test-alt'),str(path/'alternate'),str(args.efi.resolve(strict=True))],check=True)
print('EFI inventory stage primitives: PASS')
