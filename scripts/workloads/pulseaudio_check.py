#!/usr/bin/env python3
"""Run inside Brook: private, cookie-authenticated PulseAudio.

The default is a silent null sink; --output oss explicitly selects /dev/dsp.
Use a fresh runtime
directory visible to the intended client, including inside SteamRT if needed.
Stop the retained process normally when the test ends. Cookies stay in the guest.
"""
import argparse
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True,
                        help='Fresh private guest directory; its parent must already exist')
    parser.add_argument('--output', choices=['null', 'oss'], default='null',
                        help='Null diagnostic output, or audible Brook OSS output')
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('run as the desktop user')
    runtime = args.runtime.absolute()
    # Module arguments have their own whitespace-separated syntax.
    if any(c.isspace() for c in str(runtime)):
        parser.error('runtime path must not contain whitespace')
    program = Path('/nix/profile/bin/pulseaudio').resolve(strict=True)
    runtime.mkdir(mode=0o700, exist_ok=False)
    pulse = runtime / 'pulse'
    pulse.mkdir(mode=0o700)
    cookie = runtime / 'cookie'
    fd = os.open(cookie, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(os.urandom(256))
    socket = pulse / 'native'
    os.environ.update(XDG_RUNTIME_DIR=str(runtime), PULSE_RUNTIME_PATH=str(pulse),
                      PULSE_COOKIE=str(cookie))
    sink = ('module-null-sink sink_name=brook_workload_test' if args.output == 'null' else
            'module-oss device=/dev/dsp record=0 playback=1 sink_name=brook_workload_test '
            'format=s16le rate=44100 channels=2')
    command = [str(program), '--daemonize=no', '--use-pid-file=no',
               '--exit-idle-time=-1', '--high-priority=no', '--realtime=no',
               '--log-target=stderr', '-n',
               '--load=' + sink,
               '--load=module-native-protocol-unix socket=' + str(socket)
               + ' auth-cookie=' + str(cookie)]
    print(json.dumps(dict(uid=os.getuid(), server='unix:' + str(socket),
                          cookie_path=str(cookie),
                          output='null sink' if args.output == 'null' else 'OSS /dev/dsp')), flush=True)
    os.execv(str(program), command)


if __name__ == '__main__':
    main()
