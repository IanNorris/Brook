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
    subprocess.run([sys.argv[1],str(image)],check=True,timeout=30)
    subprocess.run(['e2fsck','-fn',str(image)],check=True,timeout=30)
