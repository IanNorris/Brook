import os
import time
from pathlib import Path
p=Path('/data/tmp/many')
old=sorted(p.iterdir())
assert len(old)==800, len(old)
added=p/('added-'+str(time.time_ns()))
added.write_bytes(b'guest mutation')
old[0].unlink()
assert len(list(p.iterdir()))==800
assert added.read_bytes()==b'guest mutation'
os.sync()
print('HTREECHECK PASS: existing leaves readable; add/unlink persisted', flush=True)
