from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='brook-writable-boot-') as temp:
    image=Path(temp)/'store.img'
    with image.open('xb') as stream:stream.truncate(16*1024*1024)
    subprocess.run(['mke2fs','-q','-t','ext2','-b','4096','-I','128','-m','0',
        '-O','none,filetype,sparse_super,large_file','-U','cf16f1ae-68c5-5fac-89e9-382b8af5cd5c',str(image)],check=True)
    for scenario in ('absent','bad-config','no-marker','bad-marker','occupied','slot','target',
        'lease','header','unclean','dirty-write','dirty-flush','register','bind','mount',
        'mount-cleanup','payload','unmount','home','ready','clean-write','clean-flush','system-ready','system-no-marker','system-absent'):
        subprocess.run([sys.argv[1],str(image),scenario],check=True,timeout=10)
