from pathlib import Path
import subprocess
import sys
import tempfile
import struct
with tempfile.TemporaryDirectory(prefix='brook-fat-backend-') as temporary:
    for bits,megabytes in ((16,16),(32,64)):
        image=Path(temporary)/('fixture-fat'+str(bits)+'.img')
        with image.open('wb') as output: output.truncate(megabytes*1024*1024)
        subprocess.run(['mkfs.fat','-F',str(bits),str(image)],check=True)
        subprocess.run([sys.argv[1],str(image)],check=True,timeout=30)
        subprocess.run(['fsck.fat','-n',str(image)],check=True,timeout=30)
    # Exercise the same code with a FAT16 volume behind a nonzero MBR offset.
    # diskio sectors are relative to the bound disk, not to the partition.
    volume=Path(temporary)/'offset-volume.img'
    with volume.open('wb') as output: output.truncate(16*1024*1024)
    subprocess.run(['mkfs.fat','-F','16',str(volume)],check=True)
    disk=Path(temporary)/'offset-disk.img'
    mbr=bytearray(512);mbr[446+4]=0x06
    struct.pack_into('<II',mbr,446+8,2048,16*1024*1024//512)
    mbr[510:512]=b'\x55\xaa'
    with disk.open('wb') as output, volume.open('rb') as source:
        output.write(mbr);output.seek(2048*512)
        while block:=source.read(1024*1024): output.write(block)
    subprocess.run([sys.argv[1],str(disk)],check=True,timeout=30)
    with disk.open('rb') as source, volume.open('wb') as output:
        source.seek(2048*512)
        while block:=source.read(1024*1024): output.write(block)
    subprocess.run(['fsck.fat','-n',str(volume)],check=True,timeout=30)
