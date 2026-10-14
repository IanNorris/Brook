#!/usr/bin/env python3
"""Serve the fixed 4 MiB Ladybird network/IPC benchmark on host loopback.

Run on the host, then open http://10.0.2.2:8899/ in a QEMU user-network guest.
The page title records its JavaScript transfer duration; no external assets.
"""
from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
import time,json
page=b'''<!doctype html><meta charset="utf-8"><title>BENCH_READY</title><h1>Brook network / IPC benchmark</h1><p id="status">Loading 32 x 128 KiB local resources...</p><button onclick="document.title='BENCH_CLICK';this.textContent='Click received'">Input test</button><script>
const started=performance.now(); Promise.all(Array.from({length:32},(_,i)=>fetch('/blob?i='+i).then(r=>r.arrayBuffer()))).then(data=>{const ms=Math.round(performance.now()-started);document.getElementById('status').textContent='Loaded '+data.reduce((n,b)=>n+b.byteLength,0)+' bytes in '+ms+' ms';document.title='BENCH_DONE_'+ms}).catch(e=>{document.title='BENCH_ERROR';document.getElementById('status').textContent=String(e)});
</script>'''
class Handler(BaseHTTPRequestHandler):
 protocol_version='HTTP/1.1'
 def do_GET(self):
  body=b'x'*131072 if self.path.startswith('/blob') else page
  self.send_response(200);self.send_header('Content-Length',str(len(body)));self.send_header('Content-Type','application/octet-stream' if self.path.startswith('/blob') else 'text/html');self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(body)
 def log_message(self,fmt,*args):print(json.dumps({'at':time.monotonic(),'request':fmt%args}),flush=True)
ThreadingHTTPServer(('127.0.0.1',8899),Handler).serve_forever()
