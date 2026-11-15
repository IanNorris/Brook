"""Guest-side (Brook, uid 1000): which video decoder a browser uses for a clip.

Usage: python3 browser_vaapi_probe.py WORK MODE CLIP [seconds] [extra browser args...]
  MODE  chromium-vaapi | chromium-sw | firefox-vaapi | firefox-sw
Serves CLIP (MP4 or WebM) on a local page with <video autoplay muted loop>,
launches the browser with a PRIVATE profile under WORK, and reports:
  - Chromium: DevTools Media domain player properties (kVideoDecoderName,
    kIsPlatformVideoDecoder, ...), player errors/messages, and the GPU
    process's videoDecoding capabilities (SystemInfo.getInfo);
  - both: getVideoPlaybackQuality, the media-time rate and frame callbacks.
Closes the browser at the end. Prints one JSON line prefixed VAAPI_BROWSER_RESULT.

The VA-API environment comes from BROOK_VAAPI_PKG (the staged brook-vaapi
store path): LIBVA_DRIVER_NAME=i965, LIBVA_DRIVERS_PATH=$pkg/lib/dri. The
Intel render node comes from BROOK_VAAPI_NODE (default /dev/dri/renderD129).
"""
import base64, http.server, json, os, socket, struct, subprocess, sys, threading, time
import urllib.parse, urllib.request
from pathlib import Path

PAGE = b"""<!doctype html><title>vaapi probe</title><body style=margin:0;background:#000>
<video id=v src=/clip autoplay muted loop playsinline width=1280 height=720></video><script>
const v=document.getElementById('v');let t=[],start=null;
function cb(now,m){t.push(now);if(start===null)start={wall:now,media:m.mediaTime};v.requestVideoFrameCallback(cb);}
if(v.requestVideoFrameCallback)v.requestVideoFrameCallback(cb);
window.probe=()=>{const d=t.slice(1).map((x,i)=>x-t[i]).sort((a,b)=>a-b);const q=p=>d[Math.floor(p*(d.length-1))];
const pq=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():{};
return {presented:t.length,p50:q(.5),p90:q(.9),p99:q(.99),currentTime:v.currentTime,paused:v.paused,error:v.error&&v.error.code,
 width:v.videoWidth,height:v.videoHeight,total:pq.totalVideoFrames,dropped:pq.droppedVideoFrames,
 mediaRate:start&&t.length>1?(v.currentTime-start.media)/((performance.now()-start.wall)/1000):null};};
</script>"""


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
        self.events = []

    def send(self, payload):
        size = len(payload)
        header = bytes([0x81]) + (bytes([128 | size]) if size < 126 else
                                  bytes([254]) + struct.pack('>H', size) if size < 65536 else bytes([255]) + struct.pack('>Q', size))
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
            if 'method' in m: self.events.append(m)

    def pump(self, seconds):
        """Collect events in a reader thread for `seconds` (the connection is then spent)."""
        self.socket.settimeout(None)
        def run():
            try:
                while True:
                    m = self.recv()
                    if 'method' in m: self.events.append(m)
            except Exception:
                pass
        threading.Thread(target=run, daemon=True).start()
        time.sleep(seconds)


def read_cpu():
    """Aggregate (busy, total) jiffies from /proc/stat, or None."""
    try:
        f = [int(x) for x in open('/proc/stat').readline().split()[1:]]
        idle = f[3] + (f[4] if len(f) > 4 else 0)
        return sum(f) - idle, sum(f)
    except Exception:
        return None


work, mode, clip = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 20
extra = sys.argv[5:]
work.mkdir(parents=True, exist_ok=True)
clip_type = 'video/mp4' if clip.endswith('.mp4') else 'video/webm'


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/clip':
            data = open(clip, 'rb').read()
            self.send_response(200); self.send_header('Content-Type', clip_type); self.send_header('Content-Length', str(len(data)))
            self.end_headers(); self.wfile.write(data); return
        self.send_response(200); self.send_header('Content-Type', 'text/html'); self.end_headers(); self.wfile.write(PAGE)

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Page)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f'http://127.0.0.1:{server.server_port}/'
pkg = os.environ.get('BROOK_VAAPI_PKG', '')
node = os.environ.get('BROOK_VAAPI_NODE', '/dev/dri/renderD129')
env = dict(os.environ, HOME='/data/home', XDG_CONFIG_HOME='/data/home/.config', XDG_CACHE_HOME='/data/home/.cache',
           XDG_DATA_HOME='/data/home/.local/share')
env.pop('DISPLAY', None)
vaapi = mode.endswith('-vaapi')
if vaapi:
    env.update(LIBVA_DRIVER_NAME='i965', LIBVA_DRIVERS_PATH=os.environ.get('BROOK_VAAPI_DRIVERS', pkg + '/lib/dri'),
               LIBVA_MESSAGING_LEVEL='2')
gpu = ['/nix/profile/bin/brook-iris-run']
result = {'mode': mode, 'clip': clip, 'pkg': pkg, 'node': node, 'extra': extra}
if mode.startswith('chromium'):
    profile = work / 'chromium-profile'
    tmp = work / 'tmp'; tmp.mkdir(exist_ok=True); env['TMPDIR'] = str(tmp)
    flags = ['--use-gl=angle', '--use-angle=gles', '--ignore-gpu-blocklist']
    if vaapi:
        flags += ['--enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks,AcceleratedVideoDecodeLinuxGL,'
                  'AcceleratedVideoDecodeLinuxZeroCopyGL,VaapiOnNvidiaGPUs',
                  '--disable-features=Vulkan,UseChromeOSDirectVideoDecoder', f'--render-node-override={node}',
                  '--enable-logging=stderr', '--vmodule=*vaapi*=2,*video_decoder*=2,*gpu_video*=2']
    else:
        flags += ['--disable-features=Vulkan', '--disable-accelerated-video-decode']
    argv = ['/nix/profile/bin/brook-run', *gpu, '/nix/profile/bin/chromium', '--no-sandbox', *flags,
            '--disable-background-networking', '--disable-component-update', '--disable-extensions',
            '--renderer-process-limit=2', '--disable-crash-reporter', '--no-first-run', '--no-default-browser-check',
            '--ozone-platform=wayland', f'--user-data-dir={profile}', '--remote-debugging-port=0',
            '--autoplay-policy=no-user-gesture-required', '--window-size=1340,850', *extra, '--new-window', url]
    port_file = profile / 'DevToolsActivePort'
else:
    profile = work / 'firefox-profile'; profile.mkdir(exist_ok=True)
    tmp = work / 'tmp'; tmp.mkdir(exist_ok=True); env['TMPDIR'] = str(tmp)
    prefs = ['user_pref("browser.shell.checkDefaultBrowser", false);', 'user_pref("browser.startup.page", 0);',
             'user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);',
             'user_pref("browser.sessionstore.resume_from_crash", false);', 'user_pref("media.autoplay.default", 0);',
             'user_pref("devtools.debugger.remote-enabled", true);', 'user_pref("devtools.chrome.enabled", true);',
             'user_pref("remote.active-protocols", 2);', 'user_pref("media.ffmpeg.vaapi.enabled", %s);' % ('true' if vaapi else 'false'),
             'user_pref("media.hardware-video-decoding.force-enabled", %s);' % ('true' if vaapi else 'false'),
             'user_pref("widget.dmabuf.force-enabled", true);', 'user_pref("media.ffvpx.enabled", true);']
    (profile / 'user.js').write_text('\n'.join(prefs) + '\n')
    env.update(MOZ_ENABLE_WAYLAND='1', GDK_BACKEND='wayland', MOZ_SHM_NO_SEALS='1', MOZ_LOG='PlatformDecoderModule:5,FFmpegVideo:5,Dmabuf:5',
               MOZ_LOG_FILE=str(work / 'moz.log'))
    if vaapi: env['MOZ_DISABLE_RDD_SANDBOX'] = '1'
    argv = ['/nix/profile/bin/brook-run', *gpu, '/nix/profile/bin/firefox', '--no-remote', '--profile', str(profile),
            '--remote-debugging-port=0', *extra, url]
    port_file = None
with (work / 'browser.log').open('w') as log:
    t0 = time.monotonic()
    proc = subprocess.Popen(argv, stdout=log, stderr=log, env=env)
    try:
        if port_file is not None:
            if port_file.exists(): port_file.unlink()
            end = time.monotonic() + 60
            while not port_file.exists():
                if proc.poll() is not None: raise RuntimeError('browser exited early')
                if time.monotonic() > end: raise RuntimeError('no DevTools port')
                time.sleep(0.2)
            port = int(port_file.read_text().split()[0])
            base = f'http://127.0.0.1:{port}'
            browser = DevTools(json.load(urllib.request.urlopen(base + '/json/version', timeout=5))['webSocketDebuggerUrl'])
            info = browser.call('SystemInfo.getInfo')
            gpu_info = info.get('gpu', {})
            result['video_decoding'] = gpu_info.get('videoDecoding')
            result['feature_status'] = gpu_info.get('featureStatus')
            result['gl'] = {k: v for k, v in gpu_info.get('auxAttributes', {}).items()
                            if any(s in k.lower() for s in ('glrenderer', 'glversion', 'glvendor', 'displaytype', 'glimplementation'))}
            tabs = [t for t in json.load(urllib.request.urlopen(base + '/json/list', timeout=5)) if t['type'] == 'page']
            tab = next((t for t in tabs if t.get('url', '').startswith(url)), tabs[0])
            page = DevTools(tab['webSocketDebuggerUrl'])
            page.call('Media.enable')
            page.call('Page.reload')  # restart playback so the Media domain sees player creation
            cpu0 = read_cpu()
            page.pump(secs)
            cpu1 = read_cpu()
            if cpu0 and cpu1:
                busy = (cpu1[0] - cpu0[0]); total = (cpu1[1] - cpu0[1])
                result['cpu_busy_pct'] = round(100.0 * busy / total, 1) if total else None
            props, msgs, errs = {}, [], []
            for e in page.events:
                p = e.get('params', {})
                if e['method'] == 'Media.playerPropertiesChanged':
                    for q in p.get('properties', []): props[q['name']] = q['value']
                elif e['method'] == 'Media.playerMessagesLogged':
                    msgs += [m.get('message', '')[:300] for m in p.get('messages', [])]
                elif e['method'] == 'Media.playerErrorsRaised':
                    errs += [json.dumps(x)[:400] for x in p.get('errors', [])]
            result['player'] = props
            result['messages'] = msgs[-40:]
            result['errors'] = errs[-20:]
            probe = DevTools(tab['webSocketDebuggerUrl'])
            result['playback'] = probe.call('Runtime.evaluate', expression='probe()', returnByValue=True)['result'].get('value')
            try: browser.call('Browser.close')
            except Exception: pass
        else:
            # Firefox: no CDP media domain; sample playback through the page after secs.
            time.sleep(secs)
            result['note'] = 'firefox: decoder identified from MOZ_LOG (moz.log*)'
            proc.terminate()
    except Exception as e:
        result['error'] = repr(e)
    finally:
        try: proc.wait(timeout=20)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        result['exit'] = proc.returncode
        result['elapsed_s'] = round(time.monotonic() - t0, 1)
print('VAAPI_BROWSER_RESULT ' + json.dumps(result), flush=True)
