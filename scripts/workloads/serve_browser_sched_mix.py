#!/usr/bin/env python3
"""Bounded loopback fixture for the VM browser scheduler workload.

Supply an existing generated video with continuous audio. No media is bundled.
Clock replies bracket source samples; input receipts include HTTP transport time.
"""
import argparse
import functools
import hashlib
import http.server
import json
import math
import os
from pathlib import Path
import shutil
import socket
import threading
import time
from urllib.parse import urlsplit


class FixtureServer(http.server.ThreadingHTTPServer):
    # A clean server exit must include every handler's terminal transfer record.
    daemon_threads = False
    block_on_close = True

    def __init__(self, *args, **kwargs):
        self.connections = set()
        self.connections_lock = threading.Lock()
        super().__init__(*args, **kwargs)

    def get_request(self):
        connection, address = super().get_request()
        with self.connections_lock:
            self.connections.add(connection)
        return connection, address

    def close_request(self, connection):
        try:
            super().close_request(connection)
        finally:
            with self.connections_lock:
                self.connections.discard(connection)

    def stop_requests(self):
        # Call after shutdown has stopped accepting/starting new requests.
        with self.connections_lock:
            connections = list(self.connections)
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass  # The handler may have completed and closed it already.


def copy_media_stream(source, output, record):
    """Account completed writes, not wire delivery; a failed send may be partial."""
    digest = hashlib.sha256()
    record.update(start_ns=time.monotonic_ns(), completed_write_bytes=0,
                  uncertain_write_bytes=0, complete=False)
    try:
        while block := source.read(64 * 1024):
            record['uncertain_write_bytes'] = len(block)
            written = output.write(block)
            if written != len(block):
                if isinstance(written, int) and 0 < written < len(block):
                    record['completed_write_bytes'] += written
                    digest.update(block[:written])
                    record['uncertain_write_bytes'] -= written
                raise OSError('incomplete media write')
            record['completed_write_bytes'] += written
            digest.update(block)
            record['uncertain_write_bytes'] = 0
        record['complete'] = True
    except Exception as error:
        record['error'] = repr(error)
        raise
    finally:
        record['end_ns'] = time.monotonic_ns()
        record['completed_write_sha256'] = digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clip', type=Path, required=True)
    parser.add_argument('--media-write-timeout', type=float, default=10,
                        help='Bound each media socket write in seconds (default: historical 10)')
    args = parser.parse_args()
    if not math.isfinite(args.media_write_timeout) or not 0 < args.media_write_timeout <= 300:
        parser.error('--media-write-timeout must be finite, positive and at most 300 seconds')
    if not args.clip.is_file():
        raise SystemExit('Supply an existing generated video fixture')
    root = args.output.resolve()
    root.mkdir(exist_ok=False)
    media = root / 'http'
    media.mkdir()
    (media / 'clip.mp4').symlink_to(args.clip.resolve())
    shutil.copyfile(Path(__file__).with_name('browser_sched_mix.html'), media / 'index.html')
    with args.clip.open('rb') as source:
        clip_sha = hashlib.file_digest(source, 'sha256').hexdigest()
    lock = threading.Lock()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(10)

        def do_GET(self):
            if self.path != '/clock':
                return super().do_GET()
            payload = json.dumps({'hostMonotonicNs': time.monotonic_ns()}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def copyfile(self, source, outputfile):
            if urlsplit(self.path).path != '/clip.mp4':
                return super().copyfile(source, outputfile)
            record = dict(path=self.path, range_request=self.headers.get('Range'),
                          response_status=200, source_size_bytes=os.fstat(source.fileno()).st_size,
                          source_sha256=clip_sha, write_timeout_s=args.media_write_timeout,
                          scope='Completed write calls, not guest receipt; a failed write may '
                                'partially transmit its uncertain bytes. Range is ignored (HTTP 200).')
            previous_timeout = self.connection.gettimeout()
            self.connection.settimeout(args.media_write_timeout)
            try:
                copy_media_stream(source, outputfile, record)
            finally:
                self.connection.settimeout(previous_timeout)
                with lock, (root / 'media-transfers.jsonl').open('a') as output:
                    output.write(json.dumps(record) + '\n')

        def do_POST(self):
            size = int(self.headers.get('Content-Length', '0'))
            limit = 524288 if self.path == '/trace' else 16384
            if self.path not in ('/state', '/input', '/trace') or not 0 < size <= limit:
                self.send_error(400)
                return
            value = json.loads(self.rfile.read(size))
            record = dict(hostMonotonicNs=time.monotonic_ns(), state=value)
            with lock, (root / (self.path[1:] + '.jsonl')).open('a') as output:
                output.write(json.dumps(record) + '\n')
            self.send_response(204)
            self.end_headers()

        def log_message(self, fmt, *values):
            with lock, (root / 'http.log').open('a') as output:
                output.write(fmt % values + '\n')

    server = FixtureServer(
        ('127.0.0.1', 0), functools.partial(Handler, directory=str(media)))
    (root / 'server.json').write_text(json.dumps({
        'port': server.server_port, 'clip_sha256': clip_sha,
        'clip_size_bytes': args.clip.stat().st_size,
        'media_write_timeout_s': args.media_write_timeout,
    }) + '\n')
    print('BROWSER_MIX_SERVER', server.server_port, flush=True)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    deadline = time.monotonic() + 3600
    try:
        while time.monotonic() < deadline and not (root / 'stop').exists():
            time.sleep(.2)
    finally:
        server.shutdown()
        server.stop_requests()
        server.server_close()
        worker.join()
    print('BROWSER_MIX_SERVER_STOPPED', flush=True)


if __name__ == '__main__':
    main()
