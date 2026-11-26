import os
from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='brook-ext2-objects-') as temporary:
    root=Path(temporary)
    tree=root/'tree';(tree/'dir').mkdir(parents=True)
    (tree/'dir'/'marker').write_bytes(b'original')
    os.symlink('marker',tree/'dir'/'relative')
    image=root/'fixture.img'
    with image.open('wb') as output: output.truncate(16*1024*1024)
    subprocess.run(['mkfs.ext2','-q','-F','-b','1024','-I','256','-d',str(tree),str(image)],check=True)
    tagged=root/'tagged.img'
    with tagged.open('wb') as output: output.truncate(32*1024*1024)
    subprocess.run(['mkfs.ext2','-q','-F','-b','4096','-I','256',str(tagged)],check=True)
    # Large-file fixtures: 1 KiB blocks without large_file (set on first use,
    # triply indirect from 64 MiB), and BROOK_SYSTEM's 4 KiB/128-byte layout.
    large1k=root/'large1k.img'
    with large1k.open('wb') as output: output.truncate(16*1024*1024)
    subprocess.run(['mkfs.ext2','-q','-F','-b','1024','-I','128','-O','^large_file,^resize_inode,^dir_index,^ext_attr',str(large1k)],check=True)
    large4k=root/'large4k.img'
    with large4k.open('wb') as output: output.truncate(32*1024*1024)
    subprocess.run(['mkfs.ext2','-q','-F','-b','4096','-I','128','-O','large_file,^resize_inode,^dir_index,^ext_attr',str(large4k)],check=True)
    subprocess.run([sys.argv[1],str(image),str(tagged),str(large1k),str(large4k)],check=True,timeout=120)
    for output in [image,*sorted(root.glob('*.img.child*'))]:
        if output.name.endswith('.child-large'):
            stat=subprocess.run(['debugfs','-R','stat large',str(output)],check=True,
                                capture_output=True,text=True).stdout
            size=next(line for line in stat.splitlines() if 'Size:' in line and 'File ACL' not in line)
            print(output.name,size.strip())
            assert int(size.split('Size:')[1].split()[0])>4*1024**3,size
        subprocess.run(['e2fsck','-fn',str(output)],check=True,timeout=30)
