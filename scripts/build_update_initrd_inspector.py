#!/usr/bin/env python3
"""Compile a RAM-only initrd inspector with an isolated read-only FatFs config."""
import argparse,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def compile_inspector(output,compiler=None):
    output=Path(output).resolve();compiler=compiler or os.environ.get('MUSL_CC')
    if not compiler:raise ValueError('Run in nix-shell with MUSL_CC')
    output.mkdir(parents=True,exist_ok=False);vendor=output/'fatfs';vendor.mkdir()
    source=ROOT/'src/third_party/fatfs/source'
    for name in ['ff.c','ff.h','ffunicode.c','diskio.h']:shutil.copy2(source/name,vendor/name)
    shutil.copy2(ROOT/'tools/update_fatfs/ffconf.h',vendor/'ffconf.h')
    argv=[compiler,'-static','-O2','-Wall','-Wextra','-Werror','-I'+str(vendor),str(ROOT/'tools/update_initrd_inspect.c'),
        str(ROOT/'tools/update_initrd_reader.c'),str(vendor/'ff.c'),str(vendor/'ffunicode.c'),'-o',str(output/'INITRD-INSPECT')]
    subprocess.run(argv,check=True,cwd=ROOT);return output/'INITRD-INSPECT'
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(compile_inspector(a.output))
