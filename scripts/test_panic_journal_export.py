#!/usr/bin/env python3
"""Check the compiled guest journal exporter against independently made records."""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path
import zlib


def record(image, slot, sequence, payload):
    base=slot*129*512
    header=bytearray(512)
    header[:8]=b'BRKPAN01'
    struct.pack_into('<IIQI',header,8,1,len(payload),sequence,zlib.crc32(payload))
    struct.pack_into('<I',header,28,zlib.crc32(header))
    image[base:base+512]=header
    image[base+512:base+512+len(payload)]=payload


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exporter',type=Path)
    exe=parser.parse_args().exporter.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='brook-panic-export-') as tmp:
        root=Path(tmp);source=root/'journal';output=root/'packet'
        image=bytearray(258*512)
        record(image,0,1,b'older crash');record(image,1,2,b'newer crash')
        source.write_bytes(image)
        subprocess.run([exe,source,output],check=True,stdout=subprocess.PIPE)
        assert output.read_bytes()==b'newer crash' and source.read_bytes()==image
        # Never overwrite a previously exported packet.
        assert subprocess.run([exe,source,output],stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode
        assert output.read_bytes()==b'newer crash'
        output.unlink();image[129*512+512]^=1;source.write_bytes(image)
        subprocess.run([exe,source,output],check=True,stdout=subprocess.PIPE)
        assert output.read_bytes()==b'older crash'
        output.unlink();image=bytearray(258*512)
        record(image,0,2,b'one');record(image,1,2,b'two');source.write_bytes(image)
        assert subprocess.run([exe,source,output],stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode
        assert not output.exists() and source.read_bytes()==image
        source.write_bytes(image[:-1])
        assert subprocess.run([exe,source,output],stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode
        assert not output.exists()
    print('panic journal export: PASS')


if __name__=='__main__':
    main()
