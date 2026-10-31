#!/usr/bin/env python3
"""Real curl regression checks for metadata-cache and archive HTTP failures.

Runs only a loopback server and disposable files. Pass a host C compiler and
curl via CC/CURL_PATH when they are not on PATH.
"""
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

SOURCE = Path(__file__).resolve().with_name('nix-fetch.c')
HASH = '00000000000000000000000000000001'
OTHER = '00000000000000000000000000000002'
META = (f'StorePath: /nix/store/{HASH}-test\nURL: nar/test\n'
        'Compression: none\nNarHash: sha256:test\nNarSize: 8\n').encode()


def main():
    response = {'metadata_status': 200, 'metadata': META, 'archive_status': 404, 'truncate': False}
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def do_GET(self):
            requests.append(self.path)
            meta = self.path.endswith('.narinfo')
            body = response['metadata'] if meta else b'not an archive'
            if OTHER in self.path: body = body.replace(HASH.encode(), OTHER.encode())
            interrupted = response['truncate'] and HASH in self.path
            self.send_response(response['metadata_status'] if meta else response['archive_status'])
            self.send_header('Content-Length', str(len(body)+(128 if interrupted else 0)))
            self.end_headers()
            self.wfile.write(body)
            if interrupted:
                self.wfile.flush()
                self.close_connection = True
        def log_message(self, *args): pass

    with tempfile.TemporaryDirectory(prefix='brook-fetch-http-') as scratch:
        root = Path(scratch); cache = root/'cache'; cache.mkdir()
        store = root/'store'; store.mkdir()
        binary = root/'nix-fetch'
        subprocess.run([os.environ.get('HOST_CC', os.environ.get('CC', 'cc')), '-O2', '-Wall', '-Wextra',
                        '-DNARINFO_CACHE_DIR='+json.dumps(str(cache)), str(SOURCE), '-o', str(binary)], check=True)
        batch_source = root/'batch.c'
        batch_source.write_text('#define main unused_fetch_main\n#include '+json.dumps(str(SOURCE))+
            '\n#undef main\nint main(int argc, char **argv) {\n'
            'if(argc!=4)return 2; g_cache_url=argv[1];g_narinfo_cache_dir=argv[2];g_curl_path=argv[3];\n'
            'g_cacert_path=NULL;char hashes[2][33]={"'+HASH+'","'+OTHER+'"};\n'
            'batch_prefetch_narinfos(hashes,2);return 0; }\n')
        batch = root/'batch'
        subprocess.run([os.environ.get('HOST_CC', os.environ.get('CC', 'cc')), '-O2',
                        str(batch_source), '-o', str(batch)], check=True)
        unpack = root/'unpack'; unpack.write_text('#!/bin/sh\ncat >/dev/null\n'); unpack.chmod(0o700)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        env = dict(os.environ, NIX_STORE=str(store), NIX_CACHE_URL=f'http://127.0.0.1:{server.server_port}',
                   CURL_PATH=os.environ.get('CURL_PATH') or shutil.which('curl'),
                   XZ_PATH=shutil.which('false'), NAR_UNPACK_PATH=str(unpack))
        cached = cache/(HASH+'.narinfo'); installed = store/(HASH+'-test')
        installed.mkdir()
        def run(success, deps=False):
            result = subprocess.run([str(binary), *(['--deps'] if deps else []), HASH], env=env,
                                    capture_output=True, text=True, timeout=15)
            assert (result.returncode == 0) == success, result.stdout+result.stderr
        try:
            for status in (404, 500):
                response['metadata_status'] = status
                run(False)
                assert not cached.exists(), 'HTTP error body was cached'
            response['metadata_status'] = 200
            cached.write_bytes(b'404')
            run(True)
            assert cached.read_bytes() == META, 'Legacy poison did not recover'
            for bad in (META.replace(HASH.encode(), OTHER.encode()), META[:-1],
                        META+b'X'*32768, META+b'\0', META.replace(b'nar/test', b'../test')):
                cached.unlink(missing_ok=True); response['metadata'] = bad
                run(False)
                assert not cached.exists(), 'Invalid metadata was cached'
            response['metadata'] = META
            cached.write_bytes(META+b'X'*32768)
            run(True)
            assert cached.read_bytes() == META, 'Oversized legacy cache did not recover'
            installed.rmdir(); requests.clear()
            run(False)
            assert '/nar/test' in requests
            assert not installed.exists(), 'Archive404 published an installation'
            assert not list(store.iterdir()), 'Failed extraction left temporary store paths'
            cached.unlink()
            response['truncate'] = True
            batch_args = [str(batch), env['NIX_CACHE_URL'], str(cache), env['CURL_PATH']]
            subprocess.run(batch_args, check=True, capture_output=True, timeout=15)
            assert not list(cache.iterdir()), 'Failed batch published a partial response or successful peer'
            response['truncate'] = False
            subprocess.run(batch_args, check=True, capture_output=True, timeout=15)
            assert cached.read_bytes() == META
            assert (cache/(OTHER+'.narinfo')).read_bytes() == META.replace(HASH.encode(), OTHER.encode())
            cached.unlink(); installed.mkdir()
            writers = [subprocess.Popen([str(binary), HASH], env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE) for _ in range(3)]
            for writer in writers:
                stdout, stderr = writer.communicate(timeout=15)
                assert writer.returncode == 0, stdout+stderr
            assert cached.read_bytes() == META
            assert not list(cache.glob('*.tmp.*')), 'Concurrent publication left temporary cache files'
        finally:
            server.shutdown(); server.server_close(); thread.join()
    print('nix-fetch HTTP/cache regression checks passed')


if __name__ == '__main__': main()
