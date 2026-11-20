from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='brook-fat-ext2-') as temporary:
    root=Path(temporary)
    media=root/'boot-media.img'
    witness=root/'witness'
    witness.write_bytes(b'APPS synthetic QA')
    with media.open('xb') as f:f.truncate(16*1024*1024)
    subprocess.run(['mkfs.fat','-F','16',str(media)],check=True)
    for directory in ('KERNEL','RECOVERY','RECOV2','UPDATES','UPDATES/A','UPDATES/B','ETC'):
        subprocess.run(['mmd','-i',str(media),'::'+directory],check=True)
    for name in ('BROOK.MNT','INITRD.IMG','KERNEL/INITRD.IMG','RECOVERY/INITRD.IMG','RECOV2/INITRD.IMG',
                 'UPDATES/A/INITRD.IMG','UPDATES/B/INITRD.IMG','ETC/hardware-update-key','APPS.BRK'):
        subprocess.run(['mcopy','-i',str(media),str(witness),'::'+name],check=True)
    unrelated=root/'unrelated-fat.img'
    with unrelated.open('xb') as f:f.truncate(16*1024*1024)
    subprocess.run(['mkfs.fat','-F','16',str(unrelated)],check=True)
    subprocess.run(['mcopy','-i',str(unrelated),str(witness),'::INITRD.IMG'],check=True)
    for bits in (16,32):
        inner=root/f'inner-{bits}.img';outer=root/f'outer-{bits}.img'
        with inner.open('xb') as f:f.truncate(16*1024*1024)
        subprocess.run(['mkfs.ext2','-q','-F','-b','1024','-I','256',str(inner)],check=True)
        with outer.open('xb') as f:f.truncate(64*1024*1024)
        subprocess.run(['mkfs.fat','-F',str(bits),str(outer)],check=True)
        subprocess.run(['mcopy','-i',str(outer),str(inner),'::STORE.IMG'],check=True)
        subprocess.run([sys.argv[1],str(outer),str(media),str(unrelated)],check=True,timeout=30)
        recovered=root/f'recovered-{bits}.img'
        subprocess.run(['mcopy','-i',str(outer),'::STORE.IMG',str(recovered)],check=True)
        subprocess.run(['fsck.fat','-n',str(outer)],check=True,timeout=30)
        subprocess.run(['e2fsck','-fn',str(recovered)],check=True,timeout=30)
