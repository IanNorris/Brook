#!/nix/profile/bin/python3
"""Launch the Dell's installed Steam client in its software X display."""
import glob
import os
from pathlib import Path
import socket
import subprocess
import sys
import time


def main():
    home = Path('/data/home')
    brook = home / '.local/share/brook'
    install = home / 'steam-20261022'
    client = install / 'client'
    provider = brook / 'steam-provider-20261103'
    helper = brook / 'steam/steam-client-check.py'
    bwrap = client / ('steamrt64/pv-runtime/steam-runtime-steamrt/'
                      'pressure-vessel/libexec/steam-runtime-tools-0/srt-bwrap')
    loader = provider / 'ld-linux-x86-64.so.2'
    xephyr = Path('/nix/profile/bin/Xephyr')
    if not os.access(xephyr, os.X_OK):
        candidates = sorted(glob.glob('/nix/store/*-xorg-server-*/bin/Xephyr'))
        if len(candidates) != 1:
            raise RuntimeError('Steam needs one installed Xephyr executable')
        xephyr = Path(candidates[0])
    for path in (xephyr, bwrap, loader, client / 'steamrt64/steam'):
        if not os.access(path, os.X_OK):
            raise RuntimeError(f'Steam needs {path}')
    for path in (helper, provider / 'ld.so.cache', Path('/etc/RESOLV.CONF')):
        if not path.is_file():
            raise RuntimeError(f'Steam needs {path}')
    if sys.argv[1:] == ['--check']:
        print('Steam launcher prerequisites present')
        return 0

    etc = brook / 'steam-private-etc'
    etc.mkdir(mode=0o700, exist_ok=True)
    (etc / 'passwd').write_text('brook:x:1000:100:Brook:/data/home:/nix/profile/bin/sh\n')
    (etc / 'group').write_text('users:x:100:brook\n')
    display = ':14'
    address = '/tmp/.X11-unix/X14'
    if os.path.lexists(address) or os.path.lexists('/tmp/.X14-lock'):
        raise RuntimeError('Steam’s nested display is already in use')
    env = dict(os.environ, DISPLAY=':0', LIBGL_ALWAYS_SOFTWARE='1')
    child_env = dict(env, LD_LIBRARY_PATH=':'.join(
        str(client / p) for p in ('hostlibs', 'steamrt64', 'ubuntu12_64')))
    command = [str(bwrap), '--tmpfs', '/']
    for mount in ('/nix', '/data', '/tmp', '/proc', '/boot', '/usr', '/sys'):
        command += ['--dir', mount, '--bind', mount, mount]
    command += ['--dev-bind', '/dev', '/dev']
    for path, target in ((etc / 'passwd', '/etc/passwd'), (etc / 'group', '/etc/group'),
                         (provider / 'ld.so.cache', '/etc/ld.so.cache'),
                         (provider / 'ld.so.cache', '/var/cache/ldconfig/ld.so.cache'),
                         ('/etc/RESOLV.CONF', '/etc/resolv.conf'),
                         (loader, '/lib64/ld-linux-x86-64.so.2'),
                         ('/nix/profile/bin/sh', '/bin/sh'),
                         ('/nix/profile/bin/bash', '/bin/bash')):
        command += ['--bind', str(path), target]
    command += ['--', '/nix/profile/bin/python3', str(helper), '--client', str(client),
                '--home', str(install / 'user'), '--log-dir', str(home / 'steamrt-evidence'),
                '--skip-bootstrap', '--display', display]
    log = brook / 'steam-launcher.log'
    with log.open('a') as output:
        server = subprocess.Popen([str(xephyr), display, '-screen', '1280x800',
                                   '-resizeable', '-nolisten', 'tcp'],
                                  env=env, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline and server.poll() is None:
                with socket.socket(socket.AF_UNIX) as probe:
                    probe.settimeout(.5)
                    try:
                        probe.connect(address)
                    except OSError:
                        time.sleep(.1)
                    else:
                        return subprocess.call(command, env=child_env, stdout=output, stderr=output)
            raise RuntimeError(f'Steam display did not start; see {log}')
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f'Steam: {error}', file=sys.stderr)
        sys.exit(1)
