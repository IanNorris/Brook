from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='brook-fat-objects-') as temporary:
    for bits,megabytes in ((16,16),(32,64)):
        image=Path(temporary)/('fixture-fat'+str(bits)+'.img')
        with image.open('wb') as output: output.truncate(megabytes*1024*1024)
        subprocess.run(['mkfs.fat','-F',str(bits),str(image)],check=True)
        subprocess.run([sys.argv[1],str(image)],check=True,timeout=30)
        subprocess.run(['fsck.fat','-n',str(image)],check=True,timeout=30)
