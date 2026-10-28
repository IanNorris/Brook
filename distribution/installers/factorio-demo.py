#!/usr/bin/env python3
"""Download the publisher's tested demo into the guest user's private home.

No game files are bundled. Run inside Brook, as its normal desktop user:
  python3 /path/to/factorio-demo.py --accept-terms
The installed command preflights X11/PulseAudio; those services are optional
and are not supplied by the small software-rendered base image.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

VERSION = '2.1.23'
URL = 'https://factorio.com/get-download/' + VERSION + '/demo/linux64'
SHA256 = '66e1e4bcc892b97b145fb57f20952329a884764848fc261aad6ffa89604e5f22'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accept-terms', action='store_true', help='Accept https://www.factorio.com/terms-of-service for your own installation')
    args = parser.parse_args()
    if not args.accept_terms:
        parser.error('Review publisher terms; --accept-terms is required. No download performed.')
    home = Path.home()
    destination = home / '.local/opt/factorio-demo'
    if destination.exists():
        parser.error('Destination already exists; existing installation preserved')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.factorio-', dir=destination.parent) as temporary:
        temporary = Path(temporary)
        archive = temporary / 'download.tar.xz'
        digest, size = hashlib.sha256(), 0
        with urllib.request.urlopen(URL, timeout=60) as source, archive.open('xb') as output:
            while True:
                chunk = source.read(1024 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                if size > 2 * 1024**3:
                    raise ValueError('Download exceeds 2 GiB limit')
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != SHA256:
            raise ValueError('Publisher download differs from tested version; nothing installed')
        unpack = temporary / 'unpack'
        unpack.mkdir()
        with tarfile.open(archive) as tar:
            for item in tar.getmembers():
                path = Path(item.name)
                if path.is_absolute() or '..' in path.parts or item.issym() or item.islnk() or not (item.isdir() or item.isfile()):
                    raise ValueError('Unsafe archive member: ' + item.name)
            tar.extractall(unpack, filter='data')
        (unpack / 'factorio').rename(destination)
    bindir = home / '.local/bin'
    bindir.mkdir(parents=True, exist_ok=True)
    wrapper = bindir / 'factorio-demo'
    if wrapper.exists():
        raise ValueError('Existing launcher preserved: ' + str(wrapper))
    wrapper.write_text('''#!/nix/profile/bin/bash
set -eu
if [ -z "${DISPLAY:-}" ] || [ -z "${PULSE_SERVER:-}" ]; then
  echo 'Factorio needs the optional Xwayland + PulseAudio compatibility services.' >&2
  echo 'See docs/three-games-checkpoint-20261026.md; base-image launch is not accepted yet.' >&2
  exit 69
fi
export SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=pulseaudio
exec "$HOME/.local/opt/factorio-demo/bin/x64/factorio" "$@"
''')
    wrapper.chmod(0o755)
    print('Demo installed privately:', destination)
    print('Launcher:', wrapper, '(optional runtime/services still required)')


if __name__ == '__main__':
    main()
