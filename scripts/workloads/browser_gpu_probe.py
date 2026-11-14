"""Guest-side: launch Chromium through a given wrapper, report its GPU status and frame pacing.

Usage (on Brook, uid 1000): python3 browser_gpu_probe.py WORK WRAPPER [seconds]
Serves a local canvas/CSS animation page, reads SystemInfo.getInfo (the chrome://gpu data)
over DevTools, samples requestAnimationFrame intervals in the page, then closes the browser.
Prints one JSON line prefixed BROWSER_GPU_RESULT.
"""
import base64, hashlib, http.server, json, os, socket, struct, subprocess, sys, threading, time
import urllib.parse, urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from browser_gpu_probe_page import PAGES  # noqa: E402



class DevTools:
    def __init__(self, url):
        path = urllib.parse.urlparse(url)
        self.socket = socket.create_connection(('127.0.0.1', path.port), timeout=15)
        self.reader = self.socket.makefile('rb')
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET {path.path} HTTP/1.1\r\nHost: 127.0.0.1:{path.port}\r\nUpgrade: websocket\r\n'
                             f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        assert b' 101 ' in self.reader.readline()
        while self.reader.readline() != b'\r\n':
            pass
        self.seq = 0

    def send(self, payload):
        size = len(payload)
        header = bytes([0x81]) + (bytes([128 | size]) if size < 126 else bytes([254]) + struct.pack('>H', size))
        mask = os.urandom(4)
        self.socket.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def recv(self):
        parts = []
        while True:
            b0, b1 = self.reader.read(2)
            n = b1 & 127
            if n == 126: n = struct.unpack('>H', self.reader.read(2))[0]
            elif n == 127: n = struct.unpack('>Q', self.reader.read(8))[0]
            parts.append(self.reader.read(n))
            if b0 & 0x80: return json.loads(b''.join(parts))

    def call(self, method, **params):
        self.seq += 1
        self.send(json.dumps(dict(id=self.seq, method=method, params=params)).encode())
        while True:
            m = self.recv()
            if m.get('id') == self.seq:
                if 'error' in m: raise RuntimeError(m['error'])
                return m['result']


work, wrapper = Path(sys.argv[1]), sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 15
page_name = sys.argv[4] if len(sys.argv) > 4 else 'anim'
clip = sys.argv[5] if len(sys.argv) > 5 else None
PAGE = PAGES[page_name]
work.mkdir(parents=True, exist_ok=True)


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/clip.webm' and clip:
            data = open(clip, 'rb').read()
            self.send_response(200); self.send_header('Content-Type', 'video/webm'); self.send_header('Content-Length', str(len(data)))
            self.end_headers(); self.wfile.write(data); return
        self.send_response(200); self.send_header('Content-Type', 'text/html'); self.end_headers(); self.wfile.write(PAGE)

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Page)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f'http://127.0.0.1:{server.server_port}/'
# A private profile: never attach to (and later close) the user's running browser.
profile = work / 'chromium-profile'
os.environ['BROOK_CHROMIUM_PROFILE'] = str(profile)
port_file = profile / 'DevToolsActivePort'
if port_file.exists(): port_file.unlink()
result = {'wrapper': wrapper, 'page': page_name}
with (work / 'browser.log').open('w') as log:
    proc = subprocess.Popen([wrapper, '--remote-debugging-port=0', '--window-size=1340,850', '--new-window', url],
                            stdout=log, stderr=log)
    try:
        end = time.monotonic() + 40
        while not port_file.exists():
            if proc.poll() is not None: raise RuntimeError('browser exited early')
            if time.monotonic() > end: raise RuntimeError('no DevTools port')
            time.sleep(0.2)
        port = int(port_file.read_text().split()[0])
        base = f'http://127.0.0.1:{port}'
        browser = DevTools(json.load(urllib.request.urlopen(base + '/json/version', timeout=5))['webSocketDebuggerUrl'])
        info = browser.call('SystemInfo.getInfo')
        gpu = info.get('gpu', {})
        result['gpu'] = {'devices': gpu.get('devices'), 'featureStatus': gpu.get('featureStatus'),
                         'driverBugWorkarounds': gpu.get('driverBugWorkarounds', [])[:20],
                         'auxAttributes': {k: v for k, v in gpu.get('auxAttributes', {}).items()
                                           if any(s in k.lower() for s in ('gl', 'angle', 'egl', 'vendor', 'renderer', 'skia', 'display'))}}
        tabs = [t for t in json.load(urllib.request.urlopen(base + '/json/list', timeout=5)) if t['type'] == 'page']
        tab = next((t for t in tabs if t.get('url', '').startswith(url)), tabs[0])
        page = DevTools(tab['webSocketDebuggerUrl'])
        print('BROWSER_GPU_READY', flush=True)
        time.sleep(secs)
        result['frames'] = page.call('Runtime.evaluate', expression='probe()', returnByValue=True)['result'].get('value')
        try: browser.call('Browser.close')
        except Exception: pass
    except Exception as e:
        result['error'] = repr(e)
    finally:
        try: proc.wait(timeout=20)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        result['exit'] = proc.returncode
print('BROWSER_GPU_RESULT ' + json.dumps(result), flush=True)
