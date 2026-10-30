from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='brook-fat-ext2-') as temporary:
    root=Path(temporary)
    for bits in (16,32):
        inner=root/f'inner-{bits}.img';outer=root/f'outer-{bits}.img'
        with inner.open('xb') as f:f.truncate(16*1024*1024)
        subprocess.run(['mkfs.ext2','-q','-F','-b','1024','-I','256',str(inner)],check=True)
        with outer.open('xb') as f:f.truncate(64*1024*1024)
        subprocess.run(['mkfs.fat','-F',str(bits),str(outer)],check=True)
        subprocess.run(['mcopy','-i',str(outer),str(inner),'::STORE.IMG'],check=True)
        subprocess.run([sys.argv[1],str(outer)],check=True,timeout=30)
        recovered=root/f'recovered-{bits}.img'
        subprocess.run(['mcopy','-i',str(outer),'::STORE.IMG',str(recovered)],check=True)
        subprocess.run(['fsck.fat','-n',str(outer)],check=True,timeout=30)
        subprocess.run(['e2fsck','-fn',str(recovered)],check=True,timeout=30)
