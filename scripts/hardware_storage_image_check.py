"""Read-only checks of a closed, private MBR/FAT QEMU image; no host devices."""
import hashlib, os, stat, struct, subprocess
from pathlib import Path

def verify_image(disk,meta):
    disk=Path(disk).resolve(strict=True)
    if not stat.S_ISREG(disk.stat().st_mode):raise ValueError('Expected a private regular image file')
    unchanged=[];media=str(disk)+'@@1048576'
    for name,row in meta['files'].items():
        if name in meta['mutable_files']:continue
        # '-' is binary stdout. Stream directly into the digest: no SSD copy,
        # large Python result, or overwrite prompt for an inherited RAM fd.
        p=subprocess.Popen(['mcopy','-i',media,'::'+name,'-'],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        digest=hashlib.sha256();count=0
        try:
            while block:=p.stdout.read(1048576):
                count+=len(block)
                if count>row['bytes']:raise ValueError('Unexpected file size: '+name)
                digest.update(block)
            error=p.stderr.read();assert p.wait(timeout=10)==0,error.decode(errors='replace')
            assert count==row['bytes'] and digest.hexdigest()==row['sha256'],name
        finally:
            if p.poll() is None:p.terminate();p.wait(timeout=10)
            p.stdout.close();p.stderr.close()
        unchanged.append(name)
    with disk.open('rb') as source:
        mbr=source.read(512);assert mbr[510:]==b'\x55\xaa'
        start,sectors=struct.unpack_from('<II',mbr,446+8)
        length=sectors*512
        assert start==2048 and 0<length<=512*1024*1024 and start*512+length<=disk.stat().st_size
        source.seek(start*512);fd=os.memfd_create('brook-private-fat-check',os.MFD_CLOEXEC)
        try:
            remaining=length
            while remaining:
                block=source.read(min(1048576,remaining));assert block
                written=0
                while written<len(block):written+=os.write(fd,block[written:])
                remaining-=len(block)
            checked=subprocess.run(['fsck.fat','-n',f'/proc/self/fd/{fd}'],pass_fds=(fd,),capture_output=True,text=True,timeout=60)
            assert checked.returncode==0,checked.stdout+checked.stderr
        finally:os.close(fd)
    return {'unchanged_files':unchanged,'fat_consistent':True,'fat_check_log':checked.stdout+checked.stderr}
