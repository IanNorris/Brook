#!/usr/bin/env python3
"""Run inside Brook: launch an installed 64-bit Steam client through SteamRT.

This is a diagnostic launcher, not a client installer or a login acceptance test.
Full append-mode logs stay in a unique guest directory. Authentication is manual.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def describe(path, include_tail=False):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        size = path.stat().st_size
        result = dict(path=str(path), bytes=size, sha256=digest)
        if include_tail:
            stream.seek(max(0, size - 3072))
            result['tail'] = stream.read().decode(errors='replace')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--home', type=Path)
    parser.add_argument('--log-dir', type=Path, required=True)
    parser.add_argument('--skip-bootstrap', action='store_true',
                        help='Diagnostic: use the already staged client without testing its updater')
    parser.add_argument('--launch-app', type=int,
                        help='Ask Steam to launch this installed numeric AppID after startup')
    parser.add_argument('--display', default=':0', help='An already running Xwayland display')
    parser.add_argument('--wayland-display', default='wayland-0', help='Guest Wayland socket for native dependencies')
    parser.add_argument('--pulse-server', help='Optional absolute guest Unix-socket endpoint, unix:/path')
    parser.add_argument('--pulse-cookie', type=Path,
                        help='Optional guest cookie file visible inside SteamRT; requires --pulse-server')
    parser.add_argument('--include-log-tail', action='store_true',
                        help='Opt in to exporting bounded raw log text; default reports paths and hashes only')
    args = parser.parse_args()
    if args.launch_app is not None and not 0 < args.launch_app < 2 ** 32:
        parser.error('--launch-app must be an AppID between 1 and 4294967295')
    if os.geteuid() == 0:
        parser.error('run as the desktop user')
    if args.pulse_server is not None and not args.pulse_server.startswith('unix:/'):
        parser.error('--pulse-server must name an absolute guest Unix-socket endpoint')
    if args.pulse_cookie is not None and args.pulse_server is None:
        parser.error('--pulse-cookie requires --pulse-server')
    client = args.client.resolve(strict=True)
    runtime = client / 'steamrt64/pv-runtime/steam-runtime-steamrt'
    client_container = runtime / 'pressure-vessel/libexec/steam-runtime-tools-0/srt-client-container'
    if not client_container.is_file():
        parser.error('installed SteamRT lacks srt-client-container, required for the companion game-launch service')
    platforms = list(runtime.glob('steamrt3c_platform_*/files'))
    if len(platforms) != 1:
        parser.error('expected exactly one installed SteamRT platform')
    platform = platforms[0]
    home = (args.home or client.parent / 'user').absolute()
    for path in [home / '.steam', home / '.local/share', home / '.config', home / '.cache']:
        path.mkdir(parents=True, exist_ok=True)
    for path in [home / '.steam/root', home / '.steam/steam', home / '.local/share/Steam']:
        if path.exists() or path.is_symlink():
            if path.resolve() != client:
                parser.error(f'{path} points to a different installation')
        else:
            path.symlink_to(client)
    space = os.statvfs(client)
    available = space.f_bavail * space.f_frsize
    if available < 1024 ** 3:
        parser.error('need at least 1 GiB unreserved free space for runtime copying and updates')
    corebin = Path('/nix/profile/bin/true').resolve(strict=True).parent
    host_libraries = ':'.join(map(str, [client / 'hostlibs', client / 'steamrt64', client / 'ubuntu12_64',
        platform / 'lib/x86_64-linux-gnu', platform / 'usr/lib/x86_64-linux-gnu']))
    client_libraries = ':'.join(map(str, [client / 'steamrt64', client / 'ubuntu12_64',
        client / 'hostlibs', platform / 'lib/x86_64-linux-gnu', platform / 'usr/lib/x86_64-linux-gnu']))
    # GLVND loads its vendor by SONAME. The staged exports can omit this
    # transitive Mesa output even though it is installed in the guest profile.
    mesa_vendor = Path('/nix/profile/lib/libGLX_mesa.so.0')
    if mesa_vendor.exists():
        client_libraries += ':' + str(mesa_vendor.resolve(strict=True).parent)
    env = dict(os.environ, HOME=str(home), DISPLAY=args.display,
               XDG_CONFIG_HOME=str(home / '.config'), XDG_CACHE_HOME=str(home / '.cache'),
               XDG_DATA_HOME=str(home / '.local/share'), XDG_RUNTIME_DIR='/tmp',
               WAYLAND_DISPLAY=args.wayland_display, SDL_VIDEODRIVER='x11', STEAM_RUNTIME_LOGGER='0',
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(client),
               PATH=str(corebin) + ':' + os.environ.get('PATH', ''),
               LD_LIBRARY_PATH=host_libraries)
    for key in ['STEAM_RUNTIME', 'STEAM_RUNTIME_STEAMRT', 'PRESSURE_VESSEL_PREFIX', 'LD_PRELOAD']:
        env.pop(key, None)
    # SteamRT replaces LD_LIBRARY_PATH. Prefer bundled client libraries inside
    # the container, passing paths as positional arguments rather than shell code.
    inner_environment = []
    if args.pulse_server is not None:
        inner_environment.append('PULSE_SERVER=' + args.pulse_server)
    if args.pulse_cookie is not None:
        inner_environment.append('PULSE_COOKIE=' + str(args.pulse_cookie.resolve(strict=True)))
    # The generic runtime runner starts the client but does not create the
    # companion launcher service. Games launched from inside steamrt3c use
    # steam-runtime-launch-client --alongside-steam to reach that service.
    command = [str(client_container), '--pv-entry-point=' + str(runtime / 'run'),
               '--pv-option=--batch', '--', '/bin/sh', '-c',
               'export LD_LIBRARY_PATH="$1:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; shift; exec "$@"',
               'steam-inner', client_libraries]
    # Set these after runtime construction, without shell interpolation or
    # rewriting guest socket paths. The caller provides a SteamRT-visible path.
    if inner_environment:
        command += ['/usr/bin/env'] + inner_environment
    command += [str(client / 'steamrt64/steam'),
               '-noverifyfiles', '-nobootstrapperupdate', '-srt-logger-opened',
               '-no-cef-sandbox', '-cef-disable-gpu', '-cef-disable-gpu-compositing']
    if args.skip_bootstrap:
        command.append('-skipinitialbootstrap')
    if args.launch_app is not None:
        command += ['-applaunch', str(args.launch_app)]
    args.log_dir.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='steam-client-', dir=args.log_dir))
    evidence.chmod(0o700)
    request = dict(argv=command, uid=os.getuid(), home=str(home), evidence=str(evidence),
                   available_bytes=available, default_runtime_copy=True, skip_bootstrap=args.skip_bootstrap,
                   companion_launcher_service=True,
                   launch_app=args.launch_app,
                   note='Existing staged client may contain prior ELF interpreter patches')
    (evidence / 'request.json').write_text(json.dumps(request, indent=2) + '\n')
    print(json.dumps(request), flush=True)
    started = time.monotonic()
    with (evidence / 'stdout.log').open('ab') as out, (evidence / 'stderr.log').open('ab') as err:
        result = subprocess.run(command, cwd=client, env=env, stdout=out, stderr=err)
    # Detached descendants can append after this wrapper exits. The VM job's
    # empty-group/EOF result is required before extracting final log files.
    outcome = dict(exit=result.returncode, elapsed_s=time.monotonic() - started,
                   log_snapshot='wrapper_exit; descendants may append',
                   stdout=describe(evidence / 'stdout.log', args.include_log_tail),
                   stderr=describe(evidence / 'stderr.log', args.include_log_tail))
    (evidence / 'result.json').write_text(json.dumps(outcome, indent=2) + '\n')
    print(json.dumps(outcome), flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
