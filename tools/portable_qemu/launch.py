#!/usr/bin/env python3
"""Portable Brook runner. Python 3.10+, host QEMU 8+, no third-party modules.

The disk beside this script is writable: guest changes stay in that file.
--mcp enables optional local control; ordinary launches need no setup.
"""
import argparse
import contextlib
import hmac
import http.server
import json
import os
from pathlib import Path
import platform
import queue
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid


class QMP:
    def __init__(self, proc):
        self.proc, self.lock, self.replies = proc, threading.Lock(), queue.Queue()
        self.events = []
        threading.Thread(target=self.read, daemon=True).start()
        greeting = self.replies.get(timeout=20)
        if 'QMP' not in greeting:
            raise RuntimeError('Missing QMP greeting: ' + str(greeting))
        self.call('qmp_capabilities')

    def read(self):
        for line in self.proc.stdout:
            try:
                value = json.loads(line)
            except ValueError:
                continue
            if 'event' in value:
                self.events.append(value)
                self.events[:] = self.events[-32:]
            else:
                self.replies.put(value)
        self.replies.put({'error': {'desc': 'QEMU disconnected'}})

    def call(self, command, arguments=None):
        with self.lock:
            request_id = secrets.token_hex(8)
            self.proc.stdin.write(json.dumps(dict(execute=command, arguments=arguments or {}, id=request_id)) + '\n')
            self.proc.stdin.flush()
            deadline = time.monotonic() + 15
            while True:
                reply = self.replies.get(timeout=max(.01, deadline - time.monotonic()))
                if 'error' in reply:
                    raise RuntimeError(str(reply['error']))
                if reply.get('id') == request_id:
                    return reply['return']


def startup_qmp(proc, log_path):
    try:
        return QMP(proc)
    except Exception as exc:
        # QMP EOF only describes the broken control channel. QEMU writes the
        # useful startup failure (display, audio, firmware, etc.) to stderr.
        with log_path.open('rb') as stream:
            stream.seek(max(0, stream.seek(0, 2) - 8192))
            detail = stream.read().decode('utf-8', errors='replace').strip()
        raise RuntimeError(f'{exc}\nQEMU log: {log_path}\n' +
                           (detail or '(No QEMU error output was recorded.)')) from exc


def cpu_model(accel):
    if accel in ('kvm', 'hvf'):
        return 'host'
    # Some WHPX/Hyper-V versions reject OVMF's IA32_FEATURE_CONTROL write
    # when VMX is advertised. Brook does not require nested virtualization.
    if accel == 'whpx':
        return 'max,vmx=off'
    return 'max'


def accelerators(system=None, machine=None):
    system, machine = system or platform.system(), (machine or platform.machine()).lower()
    x64 = machine in ('amd64', 'x86_64')
    if system == 'Linux' and x64:
        return [('kvm', 'host'), ('tcg', 'max')]
    if system == 'Darwin' and x64:
        return [('hvf', 'host'), ('tcg', 'max')]
    if system == 'Windows' and x64:
        return [('whpx', cpu_model('whpx')), ('tcg', 'max')]
    return [('tcg', 'max')]  # x86 guest on Apple Silicon: no cross-ISA HVF.


def choose_accel(qemu, wanted):
    candidates = accelerators() if wanted == 'auto' else [(wanted, cpu_model(wanted))]
    for accel, cpu in candidates:
        with subprocess.Popen([qemu, '-machine', 'q35', '-accel', accel, '-cpu', cpu,
                               '-m', '64', '-nodefaults', '-display', 'none', '-S', '-qmp', 'stdio'],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as proc:
            try:
                qmp = QMP(proc)
                qmp.call('quit')
                proc.wait(timeout=5)
                if proc.returncode == 0:
                    return accel, cpu
            except Exception as exc:
                proc.kill()
                proc.wait()
                detail = proc.stderr.read().strip()
                print(f'Acceleration probe failed: {accel}: {detail or exc}', file=sys.stderr)
                continue
            detail = proc.stderr.read().strip()
            print(f'Acceleration probe failed: {accel}: {detail or "nonzero exit status"}', file=sys.stderr)
    raise RuntimeError('No usable QEMU accelerator; check virtualization settings or use --accel tcg')


def choose_display(qemu, wanted):
    if wanted != 'auto':
        return wanted
    probe = subprocess.run([qemu, '-display', 'help'], capture_output=True, text=True, timeout=10)
    available = set((probe.stdout + '\n' + probe.stderr).split())
    # SDL's software renderer in affected QEMU/SDL2 combinations applies
    # window-to-guest mouse scaling twice after a resize. Prefer GTK's native
    # event/viewport path; macOS uses Cocoa when available.
    candidates = ['cocoa', 'gtk', 'sdl'] if platform.system() == 'Darwin' else ['gtk', 'sdl']
    for backend in candidates:
        if backend in available:
            return backend
    raise RuntimeError('No graphical display backend found; explicitly use --display none for headless operation')


def choose_audio(qemu, wanted):
    if wanted != 'auto':
        return wanted
    probe = subprocess.run([qemu, '-audiodev', 'help'], capture_output=True, text=True, timeout=10)
    available = set((probe.stdout + '\n' + probe.stderr).split())
    system = platform.system()
    if system == 'Darwin':
        candidates = ['coreaudio', 'sdl']
    elif system == 'Windows':
        candidates = ['wasapi', 'dsound', 'sdl']
    else:
        runtime = Path(os.environ.get('XDG_RUNTIME_DIR', '/run/user/' + str(os.getuid())))
        candidates = []
        if (runtime / 'pipewire-0').exists():
            candidates.append('pipewire')
        if os.environ.get('PULSE_SERVER') or (runtime / 'pulse/native').exists():
            candidates.append('pa')
        candidates += ['sdl', 'alsa', 'pa', 'pipewire']
    for backend in candidates:
        if backend in available:
            return backend
    raise RuntimeError('No host audio backend found; install an audio-enabled QEMU or explicitly use --audio none')


def command(qemu, accel, cpu, cores, memory, display, audio, session, bridge_port=None):
    args = [qemu, '-name', 'Brook', '-uuid', session, '-machine', 'q35', '-accel', accel,
            '-cpu', cpu, '-smp', str(cores), '-m', str(memory),
            '-drive', 'if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd',
            '-drive', 'if=pflash,format=raw,file=OVMF_VARS.fd',
            '-drive', 'if=virtio,format=qcow2,file=brook.qcow2',
            '-vga', 'std', '-display', display,
            '-device', 'virtio-tablet-pci', '-device', 'virtio-rng-pci',
            '-device', 'virtio-net-pci,netdev=net0',
            '-netdev', 'user,id=net0' + (f',hostfwd=tcp:127.0.0.1:{bridge_port}-:1234' if bridge_port else ''),
            '-device', 'ich9-intel-hda', '-device', 'hda-output,audiodev=hda0',
            '-audiodev', audio + ',id=hda0', '-serial', 'file:serial.log',
            '-chardev', 'socket,id=lifecycle,host=127.0.0.1,port=0,server=on,wait=off',
            '-serial', 'chardev:lifecycle',
            '-fw_cfg', 'name=opt/brook/lifecycle,string=' + session.replace('-', ''),
            '-fw_cfg', 'name=opt/brook/power,string=q35-reset',
            '-fw_cfg', 'name=opt/audio-staging-frames,string=4096',
            '-fw_cfg', 'name=opt/composite,string=cpu', '-qmp', 'stdio']
    return args


class Control:
    def __init__(self, proc, qmp, run, session, bridge_port):
        self.proc, self.qmp, self.run = proc, qmp, run
        self.session, self.bridge_port = session.replace('-', ''), bridge_port
        self.lock, self.generation, self.boot_id = threading.Lock(), 1, None
        self.lifecycle_socket = None
        self.lifecycle_buffer = b''

    def lifecycle(self, verb, request, boot):
        if self.lifecycle_socket is None:
            endpoints = self.qmp.call('query-chardev')
            endpoint = next(x['filename'] for x in endpoints if x['label'] == 'lifecycle')
            # QEMU reports tcp:127.0.0.1:PORT,server=on; IPv4 is explicitly bound.
            port = int(endpoint.split(',')[0].rsplit(':', 1)[1])
            self.lifecycle_socket = socket.create_connection(('127.0.0.1', port), timeout=5)
        sock = self.lifecycle_socket
        sock.settimeout(10)
        # Firmware/UART reset can leave a partial pre-boot line or a discard
        # marker. An empty line before a read-only hello resynchronizes framing
        # without replaying a power request or changing its retained ID.
        prefix = '\n' if verb == 'hello' else ''
        sock.sendall(f'{prefix}BLC1 {self.session} {request} {boot} {verb}\n'.encode())
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if b'\n' not in self.lifecycle_buffer:
                chunk = sock.recv(4096)
                if not chunk:
                    raise RuntimeError('Lifecycle disconnected; completion unknown')
                self.lifecycle_buffer += chunk
                if len(self.lifecycle_buffer) > 65536:
                    raise RuntimeError('Oversized lifecycle response')
                continue
            line, self.lifecycle_buffer = self.lifecycle_buffer.split(b'\n', 1)
            parts = line.decode('ascii').split()
            if len(parts) == 5 and parts[:2] == ['BLC1', request]:
                return dict(boot_id=parts[2], state=parts[3], result=int(parts[4]))
        raise TimeoutError('Lifecycle completion unknown')

    def dispatch(self, body):
        # A close/save confirmation may need input while orderly shutdown is
        # waiting. Serialize power transactions, not screenshots or VM input.
        with (self.lock if body.get('action') in ('sync', 'shutdown', 'restart') else contextlib.nullcontext()):
            action = body.get('action')
            if action == 'status':
                return dict(running=self.proc.poll() is None, generation=self.generation,
                            qmp=self.qmp.call('query-status'), run=str(self.run),
                            guest_bridge_enabled=bool(self.bridge_port))
            if body.get('generation') != self.generation:
                raise ValueError('Stale or missing generation; call status first')
            if action == 'logs':
                with (self.run / 'serial.log').open('rb') as stream:
                    stream.seek(max(0, stream.seek(0, 2) - 32768))
                    return dict(text=stream.read().decode('utf-8', errors='replace'))
            if action == 'screenshot':
                name = 'screen-' + secrets.token_hex(6) + '.ppm'
                self.qmp.call('screendump', dict(filename=name))
                return dict(path=str(self.run / name), format='ppm')
            if action == 'input':
                events = body.get('events', [])
                if not 1 <= len(events) <= 64:
                    raise ValueError('Supply 1–64 QMP input events')
                for event in events:
                    if event.get('type') not in ('key', 'btn', 'rel', 'abs'):
                        raise ValueError('Unsupported input event')
                return self.qmp.call('input-send-event', dict(events=events))
            if action == 'force_stop':
                if body.get('acknowledge_data_loss') is not True:
                    raise ValueError('Explicit acknowledge_data_loss=true required')
                self.qmp.call('quit')
                return dict(clean=False, reason='Forced QEMU exit; guest sync not confirmed')
            if action in ('shutdown', 'restart', 'sync'):
                hello = self.lifecycle('hello', secrets.token_hex(16), secrets.token_hex(16))
                if hello['state'] != 'ok' or hello['result'] != 0:
                    raise RuntimeError(str(hello))
                boot = hello['boot_id']
                request = secrets.token_hex(16)
                receipt = dict(action=action, request=request, boot_id=boot, completion='unknown')
                receipt_path = self.run / ('power-' + request + '.json')
                receipt_path.write_text(json.dumps(receipt, indent=2))
                reply = self.lifecycle(action, request, boot)
                deadline = time.monotonic() + 120
                while reply['state'] not in ('done', 'ready') and time.monotonic() < deadline:
                    if reply['result'] < 0 or reply['state'] == 'failed':
                        raise RuntimeError(str(reply))
                    time.sleep(.25)
                    reply = self.lifecycle('status' if action == 'sync' else 'power_status', request, boot)
                if reply['boot_id'] != boot or reply['result'] != 0 or reply['state'] not in ('done', 'ready'):
                    raise RuntimeError('Guest sync not confirmed: ' + str(reply))
                receipt.update(guest_sync_completed=True, completion='synced')
                receipt_path.write_text(json.dumps(receipt, indent=2))
                if action != 'sync':
                    receipt['commit'] = self.lifecycle('commit', request, boot)
                    if action == 'shutdown':
                        self.proc.wait(timeout=30)
                        receipt.update(completion='exited', exit_code=self.proc.returncode)
                    else:
                        # Invalidate old inputs immediately after dispatching
                        # restart, including when observing the new boot fails.
                        self.generation += 1
                        deadline = time.monotonic() + 120
                        while time.monotonic() < deadline:
                            time.sleep(1)
                            try:
                                fresh = self.lifecycle('hello', secrets.token_hex(16), secrets.token_hex(16))
                            except (OSError, TimeoutError):
                                # Firmware can consume/reset UART data during
                                # reboot. Retrying a hello is safe; never replay
                                # shutdown/restart/commit on uncertain delivery.
                                if self.lifecycle_socket:
                                    self.lifecycle_socket.close()
                                self.lifecycle_socket = None
                                self.lifecycle_buffer = b''
                                continue
                            if fresh['boot_id'] != boot and fresh['state'] == 'ok':
                                receipt.update(completion='rebooted', generation=self.generation)
                                break
                        else:
                            raise TimeoutError('Restart committed, new boot not confirmed')
                receipt_path.write_text(json.dumps(receipt, indent=2))
                return receipt
            raise ValueError('Unknown action')


def serve(control, token):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            if self.path != '/control' or not hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + token):
                self.send_error(403)
                return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 65536:
                    raise ValueError('Invalid request size')
                result = dict(result=control.dispatch(json.loads(self.rfile.read(length))))
                status = 200
            except Exception as exc:
                result, status = dict(error=str(exc)), 400
            data = json.dumps(result).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mcp', action='store_true', help='Enable optional authenticated local VM control')
    parser.add_argument('--accel', choices=['auto', 'kvm', 'hvf', 'whpx', 'tcg'], default='auto')
    parser.add_argument('--cpus', type=int)
    parser.add_argument('--memory', type=int, default=8192, help='MiB')
    parser.add_argument('--display', choices=['auto', 'sdl', 'gtk', 'cocoa', 'none'], default='auto')
    parser.add_argument('--audio', choices=['auto', 'none', 'pipewire', 'pa', 'sdl', 'alsa', 'coreaudio', 'dsound', 'wasapi'], default='auto')
    parser.add_argument('--bridge-port', type=int, help='Opt-in guest debug TCP; loopback only, unauthenticated legacy protocol')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    args = parser.parse_args()
    if not 256 <= args.memory <= 262144 or (args.cpus is not None and not 1 <= args.cpus <= 64):
        parser.error('Memory/CPU count outside supported range')
    if args.bridge_port is not None and not 1024 <= args.bridge_port <= 65535:
        parser.error('Bridge port must be 1024–65535')
    root = Path(__file__).resolve().parent
    qemu = shutil.which(args.qemu)
    if not qemu:
        parser.error('Install QEMU: https://www.qemu.org/download/ ; see README.txt')
    state = root
    for name in ('brook.qcow2', 'OVMF_CODE.fd', 'OVMF_VARS.fd'):
        if not (root / name).is_file():
            parser.error('Missing bundle file: ' + name)
    # Firmware variables, like the guest disk, persist beside the launcher.
    (state / 'OVMF_VARS.fd').chmod(0o600)
    accel, cpu = choose_accel(qemu, args.accel)
    audio = choose_audio(qemu, args.audio)
    cores = args.cpus or (2 if accel == 'tcg' else 8)
    session = str(uuid.uuid4())
    display = choose_display(qemu, args.display)
    cmd = command(qemu, accel, cpu, cores, args.memory, display, audio, session, args.bridge_port)
    print(f'Brook: {accel}, {cores} vCPUs, {args.memory} MiB, software graphics, audio={audio}', flush=True)
    with (state / 'qemu.log').open('w') as log:
        proc = subprocess.Popen(cmd, cwd=state, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
        descriptor = state / 'session.json'
        server = None
        try:
            qmp = startup_qmp(proc, state / 'qemu.log')
            if args.mcp:
                control = Control(proc, qmp, state, session, args.bridge_port)
                token = secrets.token_hex(32)
                server = serve(control, token)
                descriptor.write_text(json.dumps(dict(url=f'http://127.0.0.1:{server.server_port}/control', token=token,
                                                      session=session, pid=proc.pid, created=time.time()), indent=2))
                if os.name != 'nt':
                    descriptor.chmod(0o600)
                print('Control descriptor:', descriptor, flush=True)
            print('Use the Brook launcher power menu to shut down. Closing QEMU is a forced stop.', flush=True)
            return proc.wait()
        finally:
            if server:
                server.shutdown()
                descriptor.unlink(missing_ok=True)
            if proc.poll() is None:
                # Preserve writable state; do not silently terminate a guest on control failure.
                print('Control failed; QEMU is still running. Waiting; inspect qemu.log.', file=sys.stderr)
                proc.wait()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print('Brook launch failed:', exc, file=sys.stderr)
        sys.exit(1)
