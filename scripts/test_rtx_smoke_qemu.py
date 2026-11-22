#!/usr/bin/env python3
"""Boot the exact desktop-smoke USB image headless in QEMU and prove its scope.

q35 + OVMF + KVM, qemu-xhci with the image as a *writable* usb-storage stick and
usb-net (CDC-ECM), plus writable sacrificial AHCI, NVMe and virtio-blk disks.
Each --topology is a separate boot with its own USB layout:
  hub     usb-hub on a root port; usb-net, usb-kbd and usb-mouse behind it,
          then a usb-kbd hotplugged onto the hub
  nested  hub behind a hub: usb-net and usb-kbd on the inner hub (depth 2)
  root    usb-net and usb-kbd on root ports (Dell-style regression)
  dual    two xHCI controllers (qemu-xhci + nec-usb-xhci); NIC and a hub with
          the keyboard on the second
  late    no NIC at boot; usb-net hotplugged behind a hub after the desktop is
          up (persistent DHCP and hub hotplug)
Keys are sent to the USB keyboard by QMP input-send-event and must reach the
xHCI HID path ("xhci: USB keyboard input").
Checks: console/serial lines, DHCP, BRIDGE_V3_LOCAL ready=1, v3 attach through
tools/vm_bridge with the operator key, PCI inventory over the bridge, orderly
shutdown, and that no attached disk (including the stick) changed. Host-only:
display none, user-mode network restricted to a localhost forward.

The operator seed is passed by path to the bridge client for the attach, and
read here once only to prove it occurs in none of the published files.
"""
import argparse, hashlib, json, os, re, selectors, shutil, socket, subprocess, sys, tempfile, time, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as s:
        for chunk in iter(lambda: s.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(10); self.s.connect(str(path))
        self.f = self.s.makefile('rwb', buffering=0); self.f.readline(); self.call('qmp_capabilities')

    def call(self, cmd, **args):
        self.f.write((json.dumps({'execute': cmd, 'arguments': args}) + '\n').encode())
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r: return r['return']
            if 'error' in r: raise RuntimeError(r)


class Rpc:
    def __init__(self, base):
        env = dict(os.environ, BROOK_ROOT=str(ROOT), BROOK_HARDWARE_REGISTRY=str(base / 'hardware-registry'),
                   BROOK_VM_REGISTRY=str(base / 'absent-vm-registry'))
        self.log = (base / 'bridge-server.log').open('x')
        self.owner = subprocess.Popen([sys.executable, str(ROOT / 'tools/vm_bridge/server.py')], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=self.log, env=env)
        self.id = 0; self.select = selectors.DefaultSelector(); self.select.register(self.owner.stdout, selectors.EVENT_READ)

    def request(self, method, params=None):
        self.id += 1
        self.owner.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': self.id, 'method': method, 'params': params or {}}) + '\n').encode())
        self.owner.stdin.flush()
        assert self.select.select(60), 'MCP response timed out'
        r = json.loads(self.owner.stdout.readline()); assert r.get('id') == self.id and 'error' not in r, r
        return r['result']

    def tool(self, name, arguments=None):
        r = self.request('tools/call', {'name': name, 'arguments': arguments or {}})
        text = ''.join(i['text'] for i in r['content'] if i['type'] == 'text')
        if r.get('isError'): raise RuntimeError(f'{name}: {text}')
        return json.loads(text)

    def close(self):
        self.owner.stdin.close()
        try: self.owner.wait(timeout=15)
        except subprocess.TimeoutExpired: self.owner.terminate(); self.owner.wait(timeout=10)
        self.log.close()


def make_fixture_disks(base):
    """Random-content disks; the AHCI one also carries a GPT + FAT32 'internal' volume."""
    disks = {}
    for name, mib in (('ahci', 64), ('nvme', 32), ('virtio', 32)):
        path = base / f'{name}.img'
        with path.open('xb') as f:
            f.write(os.urandom(mib << 20))
        disks[name] = path
    ahci = disks['ahci']; esp = base / 'ahci-part.img'
    with esp.open('xb') as f: f.truncate(60 << 20)
    subprocess.run(['mkfs.fat', '-F', '32', '-n', 'WINDOWS', str(esp)], check=True, stdout=subprocess.DEVNULL)
    marker = base / 'BROOK.MNT'; marker.write_text('/data\n')
    subprocess.run(['mcopy', '-i', str(esp), str(marker), '::/BROOK.MNT'], check=True)
    subprocess.run(['sfdisk', '--no-reread', '--no-tell-kernel', str(ahci)], check=True, text=True, stdout=subprocess.DEVNULL,
                   input='label: gpt\nunit: sectors\n\nstart=2048,size=122880,type=U,name="Internal"\n')
    with ahci.open('r+b') as dst, esp.open('rb') as src:
        dst.seek(1 << 20); shutil.copyfileobj(src, dst)
    esp.unlink(); marker.unlink()
    return disks


TOPOLOGIES = {
    'hub': ['-device', 'usb-hub,bus=usbc.0,port=2,id=hub1',
            '-device', 'usb-net,bus=usbc.0,port=2.1,netdev=network',
            '-device', 'usb-kbd,bus=usbc.0,port=2.2,id=kbd0',
            '-device', 'usb-mouse,bus=usbc.0,port=2.3,id=mouse0'],
    'nested': ['-device', 'usb-hub,bus=usbc.0,port=2,id=hub1',
               '-device', 'usb-hub,bus=usbc.0,port=2.3,id=hub2',
               '-device', 'usb-net,bus=usbc.0,port=2.3.1,netdev=network',
               '-device', 'usb-kbd,bus=usbc.0,port=2.3.2,id=kbd0'],
    'root': ['-device', 'usb-net,bus=usbc.0,port=2,netdev=network',
             '-device', 'usb-kbd,bus=usbc.0,port=3,id=kbd0'],
    'dual': ['-device', 'nec-usb-xhci,id=usbd',
             '-device', 'usb-net,bus=usbd.0,port=1,netdev=network',
             '-device', 'usb-hub,bus=usbd.0,port=2,id=hub1',
             '-device', 'usb-kbd,bus=usbd.0,port=2.1,id=kbd0'],
    'late': ['-device', 'usb-hub,bus=usbc.0,port=2,id=hub1',
             '-device', 'usb-kbd,bus=usbc.0,port=2.2,id=kbd0'],
}
# Serial lines each topology must produce (after DHCP).
EXPECT = {  # QEMU puts USB3 root ports first: USB2 port N is xHCI port N+4.
    'hub': ['xhci: c0 p6 FS 0409:55aa hub', 'xhci: c0 p6.1 FS 0525:a4a2 net', 'xhci: c0 p6.2 FS 0627:0001 kbd',
            'xhci: c0 p6.3 FS 0627:0001 mouse', 'USB xhci0   p6.1 FS net 0525:a4a2', 'hubs=1 kbd=1 mouse=1 net=1'],
    'nested': ['xhci: c0 p6 FS 0409:55aa hub', 'xhci: c0 p6.3 FS 0409:55aa hub', 'xhci: c0 p6.3.1 FS 0525:a4a2 net',
               'xhci: c0 p6.3.2 FS 0627:0001 kbd', 'USB xhci0     p6.3.1 FS net', 'hubs=2 kbd=1 mouse=0 net=1'],
    'root': ['xhci: c0 p6 FS 0525:a4a2 net', 'xhci: c0 p7 HS 0627:0001 kbd', 'hubs=0 kbd=1 mouse=0 net=1'],
    'dual': ['USB xhci1 1033:0194', 'xhci: c1 p5 FS 0525:a4a2 net', 'xhci: c1 p6.1 FS 0627:0001 kbd',
             'controllers=2 driven=2', 'hubs=1 kbd=1 mouse=0 net=1'],
    'late': ['xhci: c0 p6.2 FS 0627:0001 kbd', 'xhci: c0 p6.4 FS 0525:a4a2 net', 'hubs=1 kbd=1 mouse=0 net=1',
             'NET no network interface yet'],
}


def keys_event(down, key):
    return {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': key}}}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', type=Path, required=True, help='published file set with manifest.json')
    ap.add_argument('--image', type=Path, required=True, help='raw GPT/FAT32 image built from --files')
    ap.add_argument('--operator-seed', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--topology', action='append', choices=sorted(TOPOLOGIES), help='repeatable; default all')
    ap.add_argument('--scan', type=Path, action='append', default=[], help='extra published paths to check for seed bytes')
    args = ap.parse_args()
    if subprocess.run(['pgrep', '-x', 'vmware-vmx'], stdout=subprocess.DEVNULL).returncode == 0:
        raise SystemExit('VMware is running; QEMU proof skipped')
    for key in ('OVMF_CODE', 'OVMF_VARS'):
        if not os.environ.get(key): raise SystemExit('Run inside nix-shell (OVMF paths)')
    root = args.output.absolute(); root.mkdir(parents=True, exist_ok=False)
    summary = {}
    for topology in args.topology or ['hub', 'nested', 'root', 'dual', 'late']:
        summary[topology] = run_one(args, root / topology, topology)
        (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print('RTX_SMOKE_QEMU_ALL_PASS')


def run_one(args, base, topology):
    base.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((args.files / 'manifest.json').read_text())
    for name, row in manifest['files'].items():
        assert sha(args.files / name) == row['sha256'], name
        data = subprocess.check_output(['mtype', '-i', f'{args.image}@@1048576', '::/' + name])
        assert hashlib.sha256(data).hexdigest() == row['sha256'], f'image differs from file set: {name}'
    stick = base / 'stick.raw'; shutil.copyfile(args.image, stick)
    disks = make_fixture_disks(base)
    before = {name: sha(p) for name, p in {**disks, 'stick': stick}.items()}
    variables = base / 'VARS.fd'; shutil.copy2(os.environ['OVMF_VARS'], variables); variables.chmod(0o600)
    vars_before = sha(variables)
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    # AF_UNIX paths are limited to 108 bytes; keep QMP in a short private directory.
    qmp_dir = Path(tempfile.mkdtemp(prefix='rtxq-')); qmp_path = qmp_dir / 'qmp.sock'
    qemu = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host', '-smp', '4', '-m', '2048', '-nodefaults',
            '-drive', 'if=pflash,format=raw,readonly=on,file=' + os.environ['OVMF_CODE'],
            '-drive', 'if=pflash,format=raw,file=' + str(variables),
            '-device', 'VGA,id=video0', '-device', 'qemu-xhci,id=usbc',
            '-drive', f'if=none,id=stick,format=raw,file={stick}',
            '-device', 'usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
            *TOPOLOGIES[topology],
            '-netdev', f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
            '-device', 'ahci,id=sata', '-drive', f'if=none,id=ahci0,format=raw,file={disks["ahci"]}',
            '-device', 'ide-hd,bus=sata.0,drive=ahci0',
            '-drive', f'if=none,id=nvme0,format=raw,file={disks["nvme"]}', '-device', 'nvme,serial=brooksmoke,drive=nvme0',
            '-drive', f'if=none,id=vd0,format=raw,file={disks["virtio"]}', '-device', 'virtio-blk-pci,drive=vd0',
            # PCIe root port + Intel e1000e behind it: bridge windows, extended caps,
            # and an onboard-style Intel NIC that must stay unbound.
            '-device', 'pcie-root-port,id=rp1,chassis=1,slot=1', '-device', 'e1000e,bus=rp1,netdev=lan',
            '-netdev', 'user,id=lan,restrict=on',
            '-display', 'none', '-serial', 'file:' + str(base / 'serial.log'),
            '-qmp', 'unix:' + str(qmp_path) + ',server=on,wait=off', '-no-reboot']
    (base / 'argv.json').write_text(json.dumps(qemu, indent=2) + '\n')
    log = (base / 'qemu.log').open('x'); owner = subprocess.Popen(qemu, stdout=log, stderr=subprocess.STDOUT)
    rpc = None; results = {'topology': topology, 'port_forward': port}
    serial_path = base / 'serial.log'

    def serial():
        return serial_path.read_text(errors='replace') if serial_path.exists() else ''

    def wait_for(needle, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert owner.poll() is None, 'QEMU ended early\n' + serial()[-3000:]
            text = serial()
            assert 'PANIC_HEX:' not in text, text[-3000:]
            if needle in text: return text
            time.sleep(0.1)
        raise TimeoutError(f'{needle!r} not seen\n' + serial()[-3000:])

    try:
        wait_for('DEBUG_TCP: listening on port 1234')
        if topology == 'late':
            # Desktop is up without a NIC; the status block says so. Then a
            # NIC appears behind the hub: hub hotplug + background DHCP.
            wait_for('==== END STATUS', 60)
            Qmp(qmp_path).call('device_add', driver='usb-net', bus='usbc.0', port='2.4', netdev='network', id='latenet')
            wait_for('net: IP 10.0.2.15 (DHCP)', 90)
        # The status block after the change must carry the endpoint.
        deadline = time.monotonic() + 60
        while 'BRIDGE endpoint tcp 10.0.2.15:1234 (v3' not in serial()[serial().rfind('==== BROOK STATUS'):]:
            assert time.monotonic() < deadline, 'no status block with the bridge endpoint\n' + serial()[-4000:]
            time.sleep(0.5)
        text = serial()
        required = ['HWINV_BEGIN', 'HWINV_END', 'net: IP 10.0.2.15 (DHCP)', 'BRIDGE endpoint tcp 10.0.2.15:1234',
                    'BRIDGE_V3_LOCAL build=', 'operators=1 ready=1', 'xhci: physical storage probe is read-only',
                    'xhci: CDC-ECM network interface ready', 'HWINV CPU ', 'HWINV pci ',
                    '==== BROOK STATUS (', '==== END STATUS ====', 'USB xhci0 1b36:000d', 'USB xhci0 PORTSC',
                    'NET if0 MAC ', 'SMP: 3 application processors prepared',
                    # v3: boot is not held for DHCP; per-NIC traffic counters.
                    *([] if topology == 'late' else ['net: DHCP continues in the background (boot not held)']),
                    'NET if0 stack rx=', ' usb rx posted=', 'BOOT step SmpPrepareAPs '] + EXPECT[topology]
        missing = [r for r in required if r not in text]
        assert not missing, f'missing serial lines: {missing}'
        # Per-CPU SMP lines are serial-only now (checked via the summary line);
        # launcher/dock/taskbar save warnings must not occur at all.
        for noisy in ('launcher: save failed', 'dock: save failed', 'taskbar: pins save failed'):
            assert noisy not in text, f'noise still logged: {noisy}'
        assert text.count('BRIDGE_V3_LOCAL policy_sha256=' + manifest['bridge_v3']['policy_sha256']) >= 2
        results['status_block'] = last_status(text)
        for forbidden in ('AHCI_', 'ahci: ', 'mounted at /data', 'ext2: mounted'):
            assert forbidden not in text, f'unexpected storage activity: {forbidden}'
        results['serial_ok'] = required
        rpc = Rpc(base); rpc.request('initialize', {'protocolVersion': '2025-06-18'})
        attached = rpc.tool('hw_attach', {'host': '127.0.0.1', 'port': port, 'auth_mode': 'v3',
                                          'operator_key_file': str(args.operator_seed.absolute())})
        session = attached['session']
        assert attached['auth_mode'] == 'v3' and attached['guest']['auth_ready'] is True
        assert attached['guest']['operator_policy_sha256'] == manifest['bridge_v3']['policy_sha256']
        assert attached['guest']['initrd_sha256'] == manifest['files']['INITRD.IMG']['sha256']
        results['attach'] = {k: attached['guest'].get(k) for k in ('auth_version', 'auth_ready', 'operator_keys', 'build_id',
                                                                    'payload_id', 'initrd_sha256', 'operator_policy_sha256')}
        info = rpc.tool('hw_command', {'session': session, 'command': 'bridge hardware inventory-info'})
        assert info['captured'] and info['bytes'] > 0 and not info['truncated'], info
        chunks = []; offset = 0
        while True:
            part = rpc.tool('hw_command', {'session': session, 'command': f'bridge hardware inventory {offset} 4096'})
            chunks.append(part['text']); offset = part['next_offset']
            if not part['more']: break
        inventory = ''.join(chunks); (base / 'inventory.txt').write_text(inventory)
        assert len(inventory) == info['bytes']
        start = text.index('HWINV_BEGIN\n') + len('HWINV_BEGIN\n'); end = text.index('HWINV_END')
        assert text[start:end] == inventory, 'bridge inventory differs from serial copy'
        for needle in ('cpu vendor ', 'memmap total free', 'acpi [0] DSDT', 'mcfg segment 0', 'pci 00:00.0 8086:29c0',
                       ' usb ', ' sata ', ' nvme ', 'gop framebuffer', 'pci functions'):
            assert needle in inventory, f'inventory lacks {needle!r}'
        assert 'ecam_read 0 ' not in inventory and 'ecam_refused 0' in inventory, 'ECAM extended space not read'
        for needle in ('8086:10d3 ', ' pci-bridge ', '  window mem 0x', '  window pref64 ', ' pcie id 10 ',
                       ' root-port ', '  ecap 100 aer id 0001', ' dsn id 0003'):
            assert needle in inventory, f'PCIe inventory lacks {needle!r}'
        # Word match: DMA addresses such as 0x30e1000 are not the driver.
        assert not re.search(r'\be1000', text.lower().replace('8086:10d3', '')), 'Intel NIC must stay unbound'
        results['inventory'] = {'bytes': info['bytes'], 'lines': info['lines'],
                                'pci_summary': [l for l in inventory.splitlines() if l.startswith('pci functions')]}
        pci = rpc.tool('hw_command', {'session': session, 'command': 'bridge hardware pci 0 16'})
        assert pci['pci_inventory_api'] == 1 and pci['devices'], pci
        results['legacy_pci_inventory_total'] = pci['total']
        logs = rpc.tool('hw_logs', {'session': session, 'limit': 64})
        assert logs['logs_api'] == 1
        # USB keyboard (behind the hub where the topology has one) reaches HID.
        # Unbound input goes to the most recently activated keyboard handler,
        # the USB keyboard (realized after the i8042); the xHCI line proves it.
        qmp = Qmp(qmp_path)
        for key in ('b', 'r', 'o', 'o', 'k', 'pgup', 'pgdn'):
            qmp.call('input-send-event', events=[keys_event(True, key)])
            time.sleep(0.05)
            qmp.call('input-send-event', events=[keys_event(False, key)])
            time.sleep(0.05)
        line = wait_for('xhci: USB keyboard input: 7 key presses', 30)
        results['keyboard'] = [l for l in line.splitlines() if 'USB keyboard input' in l][-1]
        if topology == 'hub':
            # usb-mouse cannot bind a console; unbound events reach the most
            # recently activated pointer, which is the guest-polled USB mouse.
            qmp.call('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': 25}},
                                                                    {'type': 'rel', 'data': {'axis': 'y', 'value': -10}}])
            line = wait_for('xhci: USB mouse input: ', 30)
            results['mouse'] = [l for l in line.splitlines() if 'USB mouse input' in l][-1]
            # Hotplug a second keyboard onto the hub, then unplug it.
            qmp.call('device_add', driver='usb-kbd', bus='usbc.0', port='2.4', id='kbd1')
            wait_for('xhci: c0 p6.4 FS 0627:0001 kbd', 30)
            qmp.call('device_del', id='kbd1')
            wait_for('xhci: c0 p6.4 kbd 0627:0001 disconnected', 30)
            results['hotplug'] = 'p6.4 keyboard enumerated and removed'
        if topology == 'late':
            results['hotplug'] = 'usb-net enumerated behind the hub after boot, DHCP bound'
        wait_for('==== BROOK STATUS (boot settled)', 40)
        if topology == 'hub':
            # The unplug is reprinted as a state change after the settle block.
            deadline = time.monotonic() + 30
            while True:
                text = serial(); gone = text.rfind('p6.4 kbd 0627:0001 disconnected')
                if text.find('==== BROOK STATUS (state changed)', gone) > 0 or \
                   text.find('==== BROOK STATUS (boot settled)', gone) > 0: break
                assert time.monotonic() < deadline, 'no status reprint after the hotplug change'
                time.sleep(0.5)
        text = serial()
        results['status_lines'] = [l for l in text.splitlines() if l.startswith('==== BROOK STATUS')]
        results['status_block'] = last_status(text)
        assert any('BRIDGE endpoint tcp 10.0.2.15:1234' in l for l in results['status_block']), results['status_block']
        assert any('DHCP bound' in l for l in results['status_block']), results['status_block']
        counters = [l for l in results['status_block'] if ' usb rx posted=' in l]
        assert counters and ' FAILED ' not in counters[0], results['status_block']
        assert not re.search(r' tx sent=0 ', counters[0]) and not re.search(r' rx posted=\d+ done=0 ', counters[0]), counters
        request = uuid.uuid4().hex
        power = rpc.tool('hw_power', {'session': session, 'request_id': request, 'operation': 'restart'})
        results['power_request'] = power.get('state')
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline and owner.poll() is None:
            try:
                status = rpc.tool('hw_power', {'session': session, 'request_id': request, 'operation': 'status'})
                results.setdefault('power_states', []).append(status.get('state'))
                if status.get('state') == 'ready':
                    rpc.tool('hw_power', {'session': session, 'request_id': request, 'operation': 'commit'})
            except RuntimeError:
                pass
            time.sleep(1)
        # -no-reboot turns the committed ACPI reset into a QEMU exit.
        results['orderly_restart_exit'] = owner.poll() is not None
        if owner.poll() is None:
            Qmp(qmp_path).call('quit')
    finally:
        if rpc: rpc.close()
        if owner.poll() is None:
            try: Qmp(qmp_path).call('quit')
            except Exception: owner.terminate()
        owner.wait(timeout=30); log.close()
        shutil.rmtree(qmp_dir, ignore_errors=True)
    after = {name: sha(p) for name, p in {**disks, 'stick': stick}.items()}
    results['disks'] = {name: {'before': before[name], 'after': after[name], 'unchanged': before[name] == after[name]}
                        for name in before}
    results['ovmf_vars_unchanged'] = sha(variables) == vars_before
    seed = args.operator_seed.read_bytes()
    leaks = []
    for root in [args.files, args.image, *args.scan]:
        for path in ([root] if Path(root).is_file() else sorted(Path(root).rglob('*'))):
            if path.is_file():
                data = path.read_bytes()
                if seed in data or seed.hex().encode() in data or seed.hex().upper().encode() in data:
                    leaks.append(str(path))
    results['seed_absent_from_outputs'] = not leaks
    (base / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    assert not leaks, leaks
    assert all(row['unchanged'] for row in results['disks'].values()), results['disks']
    # OVMF itself records boot entries in its variable store (as in v1);
    # reported, not asserted. Brook has no EFI variable write path.
    print(f'RTX_SMOKE_QEMU_PASS {topology}')
    return {'pass': True, 'keyboard': results.get('keyboard'), 'hotplug': results.get('hotplug'),
            'disks_unchanged': True, 'attach': results['attach']['auth_ready']}


def last_status(text):
    start = text.rfind('==== BROOK STATUS')
    end = text.find('==== END STATUS ====', start)
    return text[start:end + len('==== END STATUS ====')].splitlines() if start >= 0 and end > 0 else []


if __name__ == '__main__':
    main()
