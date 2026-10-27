from http.server import ThreadingHTTPServer,SimpleHTTPRequestHandler
from pathlib import Path
import os
os.chdir(Path(__file__).resolve().parent.parent/'dist')
class Handler(SimpleHTTPRequestHandler):
 def end_headers(self):
  self.send_header('Cross-Origin-Opener-Policy','same-origin')
  self.send_header('Cross-Origin-Embedder-Policy','require-corp')
  self.send_header('Cross-Origin-Resource-Policy','same-origin')
  super().end_headers()
 def guess_type(self,path):
  if '.wasm.part' in path:return 'application/wasm'
  return super().guess_type(path)
port=int(os.environ.get('BROOK_PREVIEW_PORT','38147'))
print(f'Brook browser preview: http://127.0.0.1:{port}/',flush=True)
ThreadingHTTPServer(('127.0.0.1',port),Handler).serve_forever()
