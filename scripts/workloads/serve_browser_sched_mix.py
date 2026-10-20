#!/usr/bin/env python3
"""Bounded loopback fixture for the VM browser scheduler workload.

Supply an existing generated video with continuous audio. No media is bundled.
Clock replies bracket source samples; input receipts include HTTP transport time.
"""
import argparse
import functools
import http.server
import json
from pathlib import Path
import shutil
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clip', type=Path, required=True)
    args = parser.parse_args()
    if not args.clip.is_file():
        raise SystemExit('Supply an existing generated video fixture')
    root = args.output.resolve()
    root.mkdir(exist_ok=False)
    media = root / 'http'
    media.mkdir()
    (media / 'clip.mp4').symlink_to(args.clip.resolve())
    shutil.copyfile(Path(__file__).with_name('browser_sched_mix.html'), media / 'index.html')
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

    server = http.server.ThreadingHTTPServer(
        ('127.0.0.1', 0), functools.partial(Handler, directory=str(media)))
    (root / 'server.json').write_text(json.dumps({'port': server.server_port}) + '\n')
    print('BROWSER_MIX_SERVER', server.server_port, flush=True)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    deadline = time.monotonic() + 3600
    try:
        while time.monotonic() < deadline and not (root / 'stop').exists():
            time.sleep(.2)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
    print('BROWSER_MIX_SERVER_STOPPED', flush=True)


if __name__ == '__main__':
    main()
