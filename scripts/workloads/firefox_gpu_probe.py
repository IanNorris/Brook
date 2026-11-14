"""Guest-side: launch Firefox through a given wrapper, report its compositor/GPU status and frame pacing.

Usage (on Brook, uid 1000): python3 firefox_gpu_probe.py WORK WRAPPER [seconds]
Uses Marionette (--marionette, port 2828) to read the layer manager type and nsIGfxInfo
feature status (the about:support graphics data), then samples requestAnimationFrame
intervals on the same local animation page as browser_gpu_probe.py. Prints BROWSER_GPU_RESULT.
"""
import http.server, json, socket, subprocess, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from browser_gpu_probe_page import PAGE  # noqa: E402


class Marionette:
    def __init__(self, port=2828):
        end = time.monotonic() + 40
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20); break
            except OSError:
                if time.monotonic() > end: raise
                time.sleep(0.5)
        self.f = self.s.makefile('rb'); self.read(); self.seq = 0

    def read(self):
        n = b''
        while (c := self.f.read(1)) != b':': n += c
        return json.loads(self.f.read(int(n)))

    def call(self, name, **params):
        self.seq += 1
        body = json.dumps([0, self.seq, name, params]).encode()
        self.s.sendall(str(len(body)).encode() + b':' + body)
        while True:
            m = self.read()
            if m[1] == self.seq:
                if m[2]: raise RuntimeError(m[2])
                return m[3]


work, wrapper = Path(sys.argv[1]), sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 15
work.mkdir(parents=True, exist_ok=True)
# A private profile, so the user's Firefox profile and lock are never touched.
import os
os.environ['BROOK_FIREFOX_PROFILE'] = str(work / 'firefox-profile')


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.send_header('Content-Type', 'text/html'); self.end_headers(); self.wfile.write(PAGE)

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Page)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f'http://127.0.0.1:{server.server_port}/'
CHROME = """const g=Cc['@mozilla.org/gfx/info;1'].getService(Ci.nsIGfxInfo);
const w=Services.wm.getMostRecentWindow('navigator:browser');
let features={};try{features=g.getFeatures();}catch(e){features={error:String(e)};}
let log={};try{log=g.getFeatureLog();}catch(e){}
return {layerManager:w?w.windowUtils.layerManagerType:null, webrender:w?w.windowUtils.layerManagerRemote:null,
 adapter:g.adapterDescription, vendor:g.adapterVendorID, driver:g.adapterDriver, version:g.adapterDriverVersion,
 windowProtocol:g.windowProtocol, features:features, log:log};"""
result = {'wrapper': wrapper}
with (work / 'browser.log').open('w') as log:
    proc = subprocess.Popen([wrapper, '--marionette', '-remote-allow-system-access', '--new-window', 'about:blank'], stdout=log, stderr=log)
    try:
        m = Marionette()
        m.call('WebDriver:NewSession', capabilities={})
        m.call('Marionette:SetContext', value='chrome')
        result['gfx'] = m.call('WebDriver:ExecuteScript', script=CHROME, args=[]).get('value')
        m.call('Marionette:SetContext', value='content')
        m.call('WebDriver:Navigate', url=url)
        print('BROWSER_GPU_READY', flush=True)
        time.sleep(secs)
        result['frames'] = m.call('WebDriver:ExecuteScript', script='return window.probe();', args=[]).get('value')
        try: m.call('Marionette:Quit', flags=['eForceQuit'])
        except Exception: pass
    except Exception as e:
        result['error'] = repr(e)
    finally:
        try: proc.wait(timeout=30)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        result['exit'] = proc.returncode
print('BROWSER_GPU_RESULT ' + json.dumps(result), flush=True)
