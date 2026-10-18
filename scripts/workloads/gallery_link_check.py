#!/usr/bin/env python3
"""Owned private-profile installer probe; use the matching fixture build.

The fixture installer must be compiled with profile/store/index paths below
/data/home/regression-input/gallery-links and a successful no-op fetch helper.
This checks publication/repair on cached files, not downloads or crash recovery.
"""
import os
import shutil
from pathlib import Path
import subprocess
import sys

root=Path('/data/home/regression-input/gallery-links')
binary=Path(sys.argv[1]).resolve()
assert not root.exists(), 'Refusing to reuse a pre-existing fixture directory'
assert os.getuid()==1000, 'Run the owned workload as Brook'
store_name='a'*32+'-fixture'
paths=['bin/tool','lib/libfixture.so','share/applications/fixture.desktop']

for blocked_name in [None,*paths]:
    root.mkdir()
    package=root/'store'/store_name
    for directory in ['bin','lib','share/applications']:
        (package/directory).mkdir(parents=True)
    (package/'bin/tool').write_text('#!/bin/sh\nprintf "profile fixture executable is healthy\\n"\n'+('# padding\n'*8))
    (package/'bin/tool').chmod(0o755)
    (package/'lib/libfixture.so').write_text('owned fixture library')
    (package/'share/applications/fixture.desktop').write_text('[Desktop Entry]\nName=Fixture\nExec=tool\n')
    (root/'index').write_text(f'fixture\t1\t{store_name}\tOwned fixture\n')
    if blocked_name:
        blocked=root/'profile'/blocked_name
        blocked.mkdir(parents=True);(blocked/'keep').write_text('unchanged')
    result=subprocess.run([str(binary),'fixture'],capture_output=True,timeout=120)
    assert (result.returncode==0)==(blocked_name is None),(blocked_name,result.stdout,result.stderr)
    assert (root/'profile/manifest.tsv').read_text()==f'fixture\t1\t{store_name}\n'
    assert not (root/'profile/.mutation-lock').exists()
    if blocked_name:
        assert b'incomplete profile links' in result.stderr
        assert b'Installed fixture' not in result.stdout
        assert (blocked/'keep').read_text()=='unchanged'
        shutil.rmtree(blocked)
        result=subprocess.run([str(binary),'fixture'],capture_output=True,timeout=120)
        assert result.returncode==0,result.stdout+result.stderr
    links=[root/'profile'/p for p in paths]
    for link in links:
        assert link.is_symlink() and link.is_file(),link
        assert link.resolve().is_relative_to(package),link
    before=[link.lstat().st_ino for link in links]
    result=subprocess.run([str(binary),'fixture'],capture_output=True,timeout=120)
    assert result.returncode==0,result.stdout+result.stderr
    assert [link.lstat().st_ino for link in links]==before
    assert not list(root.rglob('.brook-link.*'))
    assert not (root/'profile/.mutation-lock').exists()
    print('GALLERY_LINK_CASE_PASS',blocked_name or 'normal',flush=True)
    # This exact tree was created by this run; never clear an existing profile.
    shutil.rmtree(root)
print('GALLERY_LINK_CHECK_PASS uid='+str(os.getuid()),flush=True)
