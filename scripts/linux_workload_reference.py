#!/usr/bin/env python3
"""Owned Linux reference VM: private disk, QMP, key-only loopback SSH, no host UI.

`start` owns and waits for QEMU. Other commands target its saved PID incarnation.
A timeout is retained as a failed trial; it never starts a replacement VM.
"""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/vm_bridge'))
from common import QMP, process_identity, process_is_alive, write_json


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load(output):
    data = json.loads((output / 'session.json').read_text())
    if not process_is_alive(data['pid'], data['process_identity']):
        raise RuntimeError('The recorded Linux QEMU process is terminal or its PID was reused')
    if Path(f"/proc/{data['pid']}/exe").resolve() != Path(data['qemu_executable']).resolve():
        raise RuntimeError('Linux VM executable identity changed')
    return data


@contextlib.contextmanager
def qmp(output):
    data = load(output)
    with (Path(data['runtime']) / 'qmp.lock').open('a') as lock:
        deadline = time.monotonic() + 12
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise TimeoutError('The owned Linux QMP endpoint is busy')
                time.sleep(.02)
        load(output)
        # The shared transport accepts an absolute socket directory. This is
        # deliberately separate from the Brook bridge/session registry.
        with QMP(str(Path(data['runtime']).resolve())) as connection:
            yield connection


def ssh_argv(output):
    data = load(output)
    return ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
            '-o', 'IdentitiesOnly=yes', '-o', 'ForwardAgent=no',
            '-o', 'StrictHostKeyChecking=accept-new',
            '-o', f'UserKnownHostsFile={output / "known_hosts"}',
            '-i', data['ssh_key'], '-p', str(data['ssh_port']), 'brook@127.0.0.1']


class SilentRoutes:
    def __init__(self, daemon_file):
        self.known = set()
        self.events = []
        self.daemon = json.loads(daemon_file.read_text())
        d = self.daemon
        if not d['scope'].startswith('private null sink only'):
            raise ValueError('Audio must use the isolated null-sink daemon')
        self.env = dict(os.environ, PIPEWIRE_RUNTIME_DIR=d['runtime'], PIPEWIRE_REMOTE=d['remote'])
        self.check()

    def check(self):
        d = self.daemon
        if not process_is_alive(d['pid'], d['process_start']):
            raise RuntimeError('The owned silent PipeWire daemon is no longer live')
        if Path(f"/proc/{d['pid']}/exe").resolve().name != 'pipewire':
            raise RuntimeError('Silent PipeWire daemon executable changed')

    def command(self, argv):
        self.check()
        p = subprocess.run(argv, env=self.env, capture_output=True, text=True, timeout=4)
        if p.returncode:
            raise RuntimeError(f'{argv[0]} failed: {p.stderr[-1000:]}')
        return p.stdout

    def update(self, pid):
        objects = json.loads(self.command(['pw-dump']))
        lookup = {o['id']: o for o in objects}
        def prop(o):
            return o.get('info', {}).get('props', {})
        nodes = [o for o in objects if o.get('type') == 'PipeWire:Interface:Node']
        sinks = [o for o in nodes if prop(o).get('node.name') == 'brook-null']
        if len(sinks) != 1:
            raise RuntimeError('Expected exactly one isolated brook-null sink')
        config = json.dumps({'direction':'Output', 'mode':'dsp', 'monitor':False,
                             'format':{'mediaType':'audio', 'mediaSubtype':'raw',
                                       'format':'F32P', 'rate':44100, 'channels':2,
                                       'position':['FL','FR']}})
        for node in nodes:
            properties = prop(node)
            client = lookup.get(int(properties.get('client.id', -1)), {})
            owner = properties.get('application.process.id', prop(client).get('application.process.id'))
            token = (node['id'], str(properties.get('object.serial')))
            if str(owner) != str(pid) or token in self.known:
                continue
            self.command(['pw-cli', 'set-param', str(node['id']), 'PortConfig', config])
            time.sleep(.1)
            after = json.loads(self.command(['pw-dump']))
            ports = [o for o in after if o.get('type') == 'PipeWire:Interface:Port']
            links = []
            for channel in ('FL', 'FR'):
                sources = [o for o in ports if str(prop(o).get('node.id')) == str(node['id'])
                           and (prop(o).get('audio.channel') == channel
                                or str(prop(o).get('port.name', '')).endswith('_' + channel))]
                targets = [o for o in ports if str(prop(o).get('node.id')) == str(sinks[0]['id'])
                           and prop(o).get('port.name') == 'playback_' + channel]
                if len(sources) != 1 or len(targets) != 1:
                    raise RuntimeError('Silent audio port identity is ambiguous')
                # A watcher may attach after boot-time routing, or resume after
                # its observer exited. Adopt only an unambiguous existing link
                # between these validated owned-source/private-sink endpoints.
                existing=[o for o in after if o.get('type')=='PipeWire:Interface:Link'
                          and str(prop(o).get('link.output.port'))==str(sources[0]['id'])
                          and str(prop(o).get('link.input.port'))==str(targets[0]['id'])]
                if len(existing)>1 or any(o.get('info',{}).get('state')=='error' for o in existing):
                    raise RuntimeError('Silent audio link identity is ambiguous or failed')
                if not existing:
                    self.command(['pw-link', '-L', str(sources[0]['id']), str(targets[0]['id'])])
                links.append({'channel':channel, 'source':sources[0]['id'], 'target':targets[0]['id'],
                              'adopted':bool(existing)})
            self.known.add(token)
            self.events.append({'node':node['id'], 'serial':token[1], 'owner_pid':pid,
                                'host_monotonic_ns':time.monotonic_ns(), 'links':links})


def start(args):
    for p in Path('/proc').glob('[0-9]*/exe'):
        try:
            name = p.resolve(strict=True).name
        except OSError:
            continue
        if 'qemu-system' in name:
            raise RuntimeError(f'A QEMU VM is already live: {p.parent.name}')
    memory = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
    if int(memory['MemAvailable'].split()[0]) < 10 * 1024**2:
        raise RuntimeError('Need at least 10 GiB available host memory before the six-GiB guest')
    control = args.image.resolve(strict=True)
    packages = json.loads((control / 'manifest.json').read_text())
    key = args.ssh_key.resolve(strict=True)
    if key.stat().st_uid != os.getuid() or stat.S_IMODE(key.stat().st_mode) & 0o077:
        raise ValueError('Use an owned private SSH key with mode 0600 or stricter')
    icd = args.host_icd.resolve(strict=True)
    node = args.rendernode.resolve(strict=True)
    if not stat.S_ISCHR(node.stat().st_mode):
        raise ValueError('Render node must be a character device')
    routes = SilentRoutes(args.audio_daemon.resolve(strict=True)) if args.audio_daemon else None
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    runtime = Path(tempfile.mkdtemp(prefix='brook-linux-'))
    disk = output / 'root.img'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(control / 'root.img'), str(disk)], check=True)
    disk.chmod(0o600)
    with socket.socket() as port:
        port.bind(('127.0.0.1', 0))
        ssh_port = port.getsockname()[1]
    qemu = Path(packages['qemu']) / 'bin/qemu-system-x86_64'
    wrapped = qemu.with_name('.qemu-system-x86_64-wrapped')
    qemu_executable = wrapped.resolve() if wrapped.exists() else qemu.resolve()
    audio = 'pipewire,out.name=brook-null,out.stream-name=brook-linux-vm' if routes else 'none'
    append = ' '.join(packages['kernelParams'] + ['init=' + packages['init']])
    argv = [str(qemu), '-no-user-config', '-nodefaults', '-no-reboot',
            '-machine', 'q35,memory-backend=mem', '-accel', 'kvm', '-cpu', 'host',
            '-smp', '8', '-m', '6G', '-object', 'memory-backend-memfd,id=mem,size=6G,share=on',
            '-vga', 'none', '-device', 'virtio-gpu-gl-pci,id=vgpu,blob=true,venus=true,hostmem=1G,xres=1920,yres=1080',
            '-display', f'egl-headless,rendernode={node}',
            '-device', 'virtio-keyboard-pci', '-device', 'virtio-tablet-pci',
            '-audiodev', audio + ',id=hda0,out.buffer-length=200000,timer-period=5000',
            '-device', 'ich9-intel-hda,bus=pcie.0,addr=0x1b',
            '-device', 'hda-output,audiodev=hda0,use-timer=false',
            '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-:22',
            '-device', 'virtio-net-pci,netdev=net0',
            '-drive', f'file={disk},format=raw,if=none,id=os', '-device', 'virtio-blk-pci,drive=os',
            '-serial', f'file:{output / "serial.log"}',
            '-qmp', f'unix:{runtime / "qmp.sock"},server=on,wait=off',
            '-kernel', str(control / 'bzImage'), '-initrd', str(control / 'initrd'), '-append', append]
    env = dict(os.environ)
    for name in ('DISPLAY', 'WAYLAND_DISPLAY'):
        env.pop(name, None)
    env.update(VK_DRIVER_FILES=str(icd), XDG_RUNTIME_DIR=str(runtime),
               GBM_BACKENDS_PATH=packages['mesa'] + '/lib/gbm',
               LIBGL_DRIVERS_PATH=packages['mesa'] + '/lib/dri',
               __EGL_VENDOR_LIBRARY_DIRS=packages['mesa'] + '/share/glvnd/egl_vendor.d',
               LD_LIBRARY_PATH=':'.join(packages[k] + '/lib' for k in ('virglrenderer','glvnd','mesa','vulkanLoader')))
    if routes:
        env.update({k:routes.env[k] for k in ('PIPEWIRE_RUNTIME_DIR', 'PIPEWIRE_REMOTE')})
    manifest = dict(packages=packages, argv=argv, host_affinity=list(range(14)),
                    memory_gib=6, vcpus=8, audio_backend='pipewire-null' if routes else 'none',
                    hda_use_timer=False, rendernode=str(node), host_icd=str(icd),
                    kernel_sha256=sha(control / 'bzImage'), initrd_sha256=sha(control / 'initrd'),
                    root_source=str((control / 'root.img').resolve()), root_source_sha256=sha(control / 'root.img'),
                    private_disk=str(disk), ssh_key=str(key), ssh_port=ssh_port,
                    runtime=str(runtime), qemu=str(qemu), qemu_executable=str(qemu_executable),
                    qemu_executable_sha256=sha(qemu_executable), owned=True, timeout_seconds=args.timeout,
                    host_load=os.getloadavg(), start_host_monotonic_ns=time.monotonic_ns())
    write_json(output / 'manifest.json', manifest)
    with (output / 'qemu.stdout').open('wb') as stdout, (output / 'qemu.stderr').open('wb') as stderr:
        process = subprocess.Popen(['taskset', '-c', '0-13'] + argv, env=env, stdout=stdout, stderr=stderr)
    manifest.update(pid=process.pid, process_identity=process_identity(process.pid))
    write_json(output / 'session.json', manifest)
    print('LINUX_REFERENCE_STARTED', process.pid, output, flush=True)
    deadline = time.monotonic() + args.timeout
    timed_out = False
    try:
        while process.poll() is None:
            if time.monotonic() >= deadline:
                timed_out = True
                with qmp(output) as connection:
                    connection.call('system_powerdown')
                try:
                    process.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    # This explicitly bounded, agent-owned test is retained as
                    # failed. Only its own process is stopped; disks survive.
                    process.terminate()
                    process.wait(timeout=15)
                break
            if routes:
                routes.update(process.pid)
                write_json(output / 'audio-routes.json', routes.events)
            time.sleep(.5)
    finally:
        write_json(output / 'launcher-exit.json', {'exit_code':process.poll(), 'timed_out':timed_out,
                                                   'terminal':process.poll() is not None})
    if process.poll() is None:
        raise RuntimeError('Launcher failed while QEMU remains live; inspect the same session')
    print('LINUX_REFERENCE_STOPPED', process.returncode, 'timed_out', timed_out, flush=True)
    return 1 if timed_out else process.returncode




def check_stopped(output):
    data = json.loads((output / 'session.json').read_text())
    if process_is_alive(data['pid'], data['process_identity']):
        raise RuntimeError('The recorded Linux VM is still live; do not inspect mounted disks')
    for p in Path('/proc').glob('[0-9]*/exe'):
        try:
            name = p.resolve(strict=True).name
        except OSError:
            try:
                name=(p.parent/'comm').read_text().strip()
            except OSError:
                continue
        if 'qemu-system' in name:
            raise RuntimeError('Another QEMU is live; offline disk checks require all VMs stopped')
    disk=output/'root.img'
    if disk.is_symlink() or disk.resolve()!=Path(data['private_disk']).resolve():
        raise RuntimeError('Private disk identity changed')
    receipt=json.loads((output/'launcher-exit.json').read_text())
    with (output/'after-fsck.log').open('wb') as log:
        result=subprocess.run(['e2fsck','-fn',str(disk)],stdout=log,stderr=subprocess.STDOUT)
    powerdown='reboot: Power down' in (output/'serial.log').read_text(errors='replace')
    report=dict(exit_code=result.returncode,read_only=True,launcher=receipt,
                serial_powerdown_seen=powerdown,log=str(output/'after-fsck.log'))
    report['accepted']=result.returncode==0 and receipt['terminal'] and receipt['exit_code']==0 and not receipt['timed_out'] and powerdown
    write_json(output/'after-fsck.json',report)
    print(json.dumps(report))
    return 0 if report['accepted'] else 1


def screenshot(output):
    path = output / ('screen-' + str(time.monotonic_ns()) + '.png')
    try:
        with qmp(output) as connection:
            connection.call('screendump', {'filename':str(path), 'format':'png'})
        return {'artifact':str(path), 'source':'qmp-screendump'}
    except RuntimeError as error:
        if str(error) != 'no surface':
            raise
    # GL scanout may expose no CPU surface. Weston's normal screenshot hotkey
    # authorizes its own client; do not enable compositor debug access.
    import shlex
    listing = "import json;from pathlib import Path;print(json.dumps([{\"name\":p.name,\"size\":p.stat().st_size} for p in Path.home().glob(\"wayland-screenshot-*.png\")]))"
    def files():
        return json.loads(subprocess.check_output(ssh_argv(output) +
            [shlex.join(['python3','-c',listing])], timeout=15))
    before = {v['name'] for v in files()}
    with qmp(output) as connection:
        connection.call('send-key', {'keys':[{'type':'qcode','data':'meta_l'},
                                             {'type':'qcode','data':'s'}], 'hold-time':100})
    deadline = time.monotonic() + 15
    while True:
        candidates = [v for v in files() if v['name'] not in before and 8 < v['size'] < 16*1024**2]
        if candidates:
            remote = '/home/brook/' + sorted(candidates,key=lambda v:v['name'])[-1]['name']
            data = subprocess.check_output(ssh_argv(output) + [shlex.join(['cat',remote])], timeout=15)
            if data.startswith(b'\x89PNG\r\n\x1a\n') and data.endswith(b'IEND\xaeB`\x82'):
                path.write_bytes(data)
                return {'artifact':str(path), 'source':'weston-authorized-hotkey', 'guest_artifact':remote}
        if time.monotonic() >= deadline:
            raise TimeoutError('Guest screenshot pending; no duplicate hotkey sent')
        time.sleep(.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    launch = sub.add_parser('start')
    launch.add_argument('--image', type=Path, required=True)
    launch.add_argument('--output', type=Path, required=True)
    launch.add_argument('--ssh-key', type=Path, required=True)
    launch.add_argument('--host-icd', type=Path, required=True)
    launch.add_argument('--rendernode', type=Path, default=Path('/dev/dri/renderD128'))
    launch.add_argument('--audio-daemon', type=Path)
    launch.add_argument('--timeout', type=int, default=1800, choices=range(120, 7201), metavar='120..7200')
    for name in ('status', 'exec', 'shutdown', 'screenshot', 'check'):
        item = sub.add_parser(name)
        item.add_argument('--output', type=Path, required=True)
        if name == 'exec':
            item.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.action == 'start':
        return start(args)
    output = args.output.resolve(strict=True)
    if args.action == 'check':
        return check_stopped(output)
    if args.action == 'exec':
        if not args.command:
            parser.error('exec requires a guest command')
        command = args.command[1:] if args.command[0] == '--' else args.command
        # SSH executes one guest shell command; quote argv for that shell.
        import shlex
        return subprocess.run(ssh_argv(output) + [shlex.join(command)]).returncode
    if args.action == 'screenshot':
        result=screenshot(output)
        write_json(output / 'screenshot-last.json',result)
        print(json.dumps(result))
        return 0
    with qmp(output) as connection:
        if args.action == 'status':
            print(json.dumps(connection.call('query-status')))
        elif args.action == 'shutdown':
            write_json(output / 'shutdown-intent.json', {'host_monotonic_ns':time.monotonic_ns()})
            connection.call('system_powerdown')
            write_json(output / 'shutdown-ack.json', {'host_monotonic_ns':time.monotonic_ns()})
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
