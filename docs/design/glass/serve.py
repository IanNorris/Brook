#!/usr/bin/env python3
"""Loopback-only preview of the study and its explicit public assets."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
import mimetypes

STUDY = Path(__file__).resolve().parent
ROOT = STUDY.parents[2]
FILES = {f'/{name}': STUDY / name for name in
         ('index.html', 'study.css', 'study.js', 'clearwater.png', 'smoked.png', 'opal.png')}
FILES['/'] = STUDY / 'index.html'
for asset in ('data/branding/brook-mark.png', 'data/wallpaper/brook-dawn.png',
              'data/icons/files.svg', 'data/icons/settings.svg'):
    FILES['/' + asset] = ROOT / asset
FILES.update({'/docs/design/glass' + path: value for path, value in list(FILES.items())
              if not path.startswith('/data/')})

class Preview(BaseHTTPRequestHandler):
    def do_GET(self):
        path = FILES.get(urlsplit(self.path).path)
        if path is None or not path.is_file():
            self.send_error(404)
            return
        body = path.read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', mimetypes.guess_type(path)[0] or 'application/octet-stream')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18872)
    args = parser.parse_args()
    with ThreadingHTTPServer(('127.0.0.1', args.port), Preview) as server:
        print(f'Brook glass study: http://127.0.0.1:{args.port}/', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
