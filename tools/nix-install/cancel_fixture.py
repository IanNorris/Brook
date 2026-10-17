"""Tiny public-data binary cache with a controllable archive boundary.

Shared by host cancellation tests and isolated guest acceptance. Contains no
external packages: each archive is a directory holding one generated text file.
"""
import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import struct
import threading
import time

ROOT_HASH = 'a' * 32
DEP_HASH = 'b' * 32
ROOT_NAME = ROOT_HASH + '-brook-cancel-fixture'
DEP_NAME = DEP_HASH + '-brook-cancel-dependency'
PAYLOAD = b'Brook cooperative package cancellation fixture.\n' * 4096


def string(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('<Q', len(value)) + value + bytes((-len(value)) % 8)


def archive():
    fields = ['nix-archive-1', '(', 'type', 'directory', 'entry', '(', 'name', 'proof.txt',
              'node', '(', 'type', 'regular', 'contents', PAYLOAD, ')', ')', ')']
    return b''.join(map(string, fields))


class Cache(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address=('127.0.0.1', 0), release_file=None):
        self.started = threading.Event()
        self.release = threading.Event()
        self.release_file = release_file
        self.requests = Counter()
        self.lock = threading.Lock()
        self.gated_path = '/dependency.nar'
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        cache = self.server
        with cache.lock:
            cache.requests[self.path] += 1
        if self.path in ('/'+ROOT_HASH+'.narinfo', '/'+DEP_HASH+'.narinfo'):
            root = ROOT_HASH in self.path
            name = ROOT_NAME if root else DEP_NAME
            refs = DEP_NAME if root else ''
            body = (f'StorePath: /nix/store/{name}\nURL: {"root" if root else "dependency"}.nar\n'
                    f'Compression: none\nReferences: {refs}\n').encode()
        elif self.path in ('/root.nar', '/dependency.nar'):
            body = archive()
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        if self.path == cache.gated_path:
            self.wfile.write(body[:128]); self.wfile.flush()
            cache.started.set()
            print('CANCEL_FIXTURE_WAIT', self.path, flush=True)
            deadline = time.monotonic()+90
            while not cache.release.is_set() and not (cache.release_file and cache.release_file.exists()):
                if time.monotonic() > deadline:
                    return
                time.sleep(.02)
            body = body[128:]
        self.wfile.write(body)
        self.wfile.flush()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=21771)
    parser.add_argument('--release', type=Path, required=True)
    parser.add_argument('--gate', choices=['root','dependency'], default='dependency')
    args = parser.parse_args()
    server = Cache(('127.0.0.1', args.port), args.release)
    server.gated_path='/'+args.gate+'.nar'
    print('CANCEL_FIXTURE_READY', args.port, args.gate, flush=True)
    server.serve_forever()
