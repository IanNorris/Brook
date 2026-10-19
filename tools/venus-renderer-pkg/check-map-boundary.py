"""Compile actual renderer map code with instrumented map backends."""
import json
from pathlib import Path
import resource
import shlex
import subprocess
import sys

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
commands=json.loads(subprocess.check_output(['ninja','-t','compdb'],text=True))
entry=next(row for row in commands if row['file'].endswith('/virglrenderer.c'))
base=shlex.split(entry['command'])
source=entry['file']
original=Path(source).read_text()

def compile_source(path,output):
    argv=list(base)
    argv[argv.index(source)]=str(path)
    argv[argv.index('-o')+1]=output
    argv += ['-ffunction-sections','-fdata-sections']
    subprocess.run(argv,check=True)

compile_source(source,'boundary-renderer.o')
compile_source(sys.argv[1],'boundary-test.o')
link=[base[0],'-Wl,--gc-sections','-Wl,--wrap=mmap','-Wl,--wrap=mmap64','-Wl,--wrap=munmap',
      'boundary-renderer.o','boundary-test.o','-o','boundary-test']
subprocess.run(link,check=True)
subprocess.run(['./boundary-test'],check=True)
# Prove this test detects omission of the size check without a GPU or VM.
mutant=Path('boundary-renderer-mutant.c')
needle='if (declared_size && map_size != declared_size)'
assert original.count(needle)==1
mutant.write_text(original.replace(needle,'if (0 && declared_size && map_size != declared_size)'))
compile_source(mutant,'boundary-renderer.o')
subprocess.run(link,check=True)
with Path('boundary-negative.log').open('wb') as log:
    result=subprocess.run(['./boundary-test'],stdout=log,stderr=log)
assert result.returncode!=0,'negative control unexpectedly passed'
print('RENDERER_MAP_BOUNDARY_NEGATIVE_CONTROL_PASS',flush=True)
