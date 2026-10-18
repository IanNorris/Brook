#!/usr/bin/env python3
"""Owned cached-profile removal probe; requires a matching private-path build.

This exercises real guest directory permissions, not disk-full, transactions,
downloads, or shared-provider ownership. Never point it at a live profile.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

root=Path('/data/home/regression-input/gallery-remove')
binary=Path(sys.argv[1]).resolve()
assert binary.name=='nix', 'The installer dispatches subcommands by argv[0]'
assert os.getuid()==1000, 'Run as Brook'
assert not root.exists(), 'Refusing to reuse a pre-existing fixture'
store_name='a'*32+'-fixture'
exports=['bin/tool','lib/libfixture.so','share/applications/fixture.desktop']

def command(action):
    result=subprocess.run([str(binary),action,'fixture'],capture_output=True,timeout=120)
    assert not (root/'profile/.mutation-lock').exists()
    return result

for blocked_name in [None,*exports]:
    package=root/'store'/store_name
    for directory in ['bin','lib','share/applications']:
        (package/directory).mkdir(parents=True)
    (package/'bin/tool').write_text('#!/bin/sh\nprintf "owned fixture\\n"\n'+('# padding\n'*12))
    (package/'bin/tool').chmod(0o755)
    (package/'lib/libfixture.so').write_text('owned fixture library')
    (package/'share/applications/fixture.desktop').write_text('[Desktop Entry]\nName=Fixture\nExec=tool\n')
    (root/'index').write_text(f'fixture\t1\t{store_name}\tOwned fixture\n')
    result=command('install');assert result.returncode==0,result.stdout+result.stderr
    links=[root/'profile'/p for p in exports]
    if blocked_name is None:
        for link in links:
            target=link.resolve();link.unlink();link.symlink_to(target)
        foreign=root/'profile/lib/foreign.so';foreign.symlink_to('/unrelated/owned/library')
        result=command('remove');assert result.returncode==0,result.stdout+result.stderr
        assert all(not p.is_symlink() for p in links)
        assert foreign.is_symlink()
    else:
        blocked=root/'profile'/blocked_name
        blocked.parent.chmod(0o555)
        try:result=command('remove')
        finally:blocked.parent.chmod(0o755)
        assert result.returncode!=0 and b'incomplete profile removal' in result.stderr,result.stdout+result.stderr
        assert b'Removed fixture' not in result.stdout and blocked.is_symlink()
        # Registration is already removed in the current protocol. Restore the
        # fixture via install before retrying; this is explicitly not rollback.
        result=command('install');assert result.returncode==0,result.stdout+result.stderr
        assert all(p.is_file() for p in links)
        result=command('remove');assert result.returncode==0,result.stdout+result.stderr
        assert all(not p.is_symlink() for p in links)
    assert (package/'bin/tool').is_file() and (package/'lib/libfixture.so').is_file()
    assert not list(root.rglob('.brook-link.*'))
    print('GALLERY_REMOVE_CASE_PASS',blocked_name or 'absolute-exports',flush=True)
    shutil.rmtree(root) # Only this invocation's newly created tree.
print('GALLERY_REMOVE_CHECK_PASS uid='+str(os.getuid()),flush=True)
