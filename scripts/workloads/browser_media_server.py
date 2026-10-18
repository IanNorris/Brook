#!/usr/bin/env python3
"""Serve the controlled media fixture and source-timestamped VM reports.

Use a new output directory below build/. Keep this process alive across VM
reboots to preserve the HTTP origin for read-only localStorage checks. Touch
OUTPUT/stop-server to stop; this never controls or shuts down a VM.
"""
import argparse
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import threading
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clip', type=Path, required=True)
    parser.add_argument('--resume', action='store_true',
                        help='Resume a stopped fixture on its recorded port, preserving its origin')
    args = parser.parse_args()
    output = args.output.resolve()
    clip = args.clip.resolve(strict=True)
    if not output.is_relative_to(ROOT / 'build') or (output.exists() and not args.resume):
        parser.error('--output must be new under build/, or explicitly resumed')
    if not clip.is_file():
        parser.error('--clip must be a media file')
    media = output / 'http'
    page = Path(__file__).with_name('browser_media_fixture.html')
    page_bytes = page.read_bytes()
    with clip.open('rb') as source:
        clip_sha = hashlib.file_digest(source, 'sha256').hexdigest()
    port = 0
    if args.resume:
        previous = json.loads((output / 'servers.json').read_text())
        if (not (output / 'stop-server').is_file()
                or previous['clip_sha256'] != clip_sha
                or previous['page_sha256'] != hashlib.sha256(page_bytes).hexdigest()
                or (media / 'index.html').read_bytes() != page_bytes
                or (media / 'clip.webm').resolve() != clip):
            parser.error('Resume requires an explicitly stopped fixture with matching media/page')
        port = previous['media_port']
    else:
        media.mkdir(parents=True)
        (media / 'clip.webm').symlink_to(clip)
        (media / 'index.html').write_bytes(page_bytes)
    lock = threading.Lock()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_POST(self):
            if self.path != '/state':
                self.send_error(404)
                return
            try:
                size = int(self.headers.get('Content-Length', '0'))
                # Ten minutes of 100 ms source samples exceed the old 256 KiB
                # limit. Retain a bound large enough for the long-run fixture.
                if not 0 < size <= 4 * 1024 * 1024:
                    raise ValueError('report size')
                state = json.loads(self.rfile.read(size))
                if not isinstance(state, dict):
                    raise ValueError('report object')
            except (ValueError, UnicodeError):
                self.send_error(400)
                return
            row = {'time': time.time(), 'monotonic': time.monotonic(), 'state': state}
            with lock, (output / 'media-state.jsonl').open('a') as log:
                log.write(json.dumps(row) + '\n')
            self.send_response(204)
            self.end_headers()

        def log_message(self, fmt, *values):
            with lock, (output / 'http.log').open('a') as log:
                log.write(fmt % values + '\n')

    server = http.server.ThreadingHTTPServer(
        ('127.0.0.1', port), functools.partial(Handler, directory=str(media)))
    if args.resume:
        (output / 'stop-server').unlink()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    (output / 'servers.json').write_text(json.dumps({
        'pid': os.getpid(), 'media_port': server.server_port,
        'clip': str(clip), 'clip_sha256': clip_sha,
        'page_sha256': hashlib.sha256(page_bytes).hexdigest(),
        'scope': 'loopback fixture; source timestamps, synthetic localStorage only',
    }, indent=2) + '\n')
    thread.start()
    print('READY', server.server_port, flush=True)
    try:
        while not (output / 'stop-server').exists():
            time.sleep(.2)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print('SERVER_EXIT_0', flush=True)


if __name__ == '__main__':
    main()
