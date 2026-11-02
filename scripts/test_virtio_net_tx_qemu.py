#!/usr/bin/env python3
"""Disposable QEMU virtio-net TCP transfer; no host audio or USB passthrough."""
import json
import os
import selectors
import shutil
import socket
import subprocess
import time
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)


def run(*command):
    subprocess.run(list(map(str, command)), cwd=root, check=True, stdout=subprocess.DEVNULL)


run(os.environ['MUSL_CC'], '-static', '-O2', '-Wall', '-Wextra', '-Werror',
    root/'scripts/workloads/tcp_send_chunks.c', '-o', out/'TCPCHUNKS.ELF')
probe = out/'probe'
run('python3', root/'scripts/make_uefi_probe.py', '--output', probe,
    '--driver', 'virtio_net', '--display-width', '800', '--display-height', '600')
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
initrd = probe/'payload/INITRD.IMG'
run('mcopy', '-i', initrd, out/'TCPCHUNKS.ELF', '::BOOT/TCPCHUNKS.ELF')
rc = out/'INIT.RC'
rc.write_text(f'set wm\nset vfb none\nkconsole\nrun /boot/TCPCHUNKS.ELF 10.0.2.2 {port} 4096\n')
run('mcopy', '-o', '-i', initrd, rc, '::BOOT/INIT.RC')
run('mcopy', '-o', '-i', str(probe/'probe.raw')+'@@1048576', initrd, '::INITRD.IMG')
variables = out/'VARS.fd'
shutil.copy2(os.environ['OVMF_VARS'], variables)
variables.chmod(0o600)
sink = subprocess.Popen(['python3', str(root/'scripts/workloads/tcp_chunk_sink.py'),
                         '--bind', '127.0.0.1', '--port', str(port), '--connections', '1',
                         '--output', str(out/'sink.json')], stdout=subprocess.PIPE, text=True)
vm = None
try:
    with selectors.DefaultSelector() as selector:
        selector.register(sink.stdout, selectors.EVENT_READ)
        if not selector.select(timeout=20) or 'TCP_CHUNK_SINK_READY' not in sink.stdout.readline():
            raise RuntimeError('TCP sink did not start')
    command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
               '-smp', '4', '-m', '2048', '-nodefaults',
               '-drive', 'if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
               '-drive', 'if=pflash,format=raw,file='+str(variables),
               '-device', 'VGA', '-device', 'qemu-xhci,id=usb',
               '-drive', 'if=none,id=stick,format=raw,readonly=on,file='+str(probe/'probe.raw'),
               '-device', 'usb-storage,bus=usb.0,drive=stick,bootindex=1',
               '-device', 'virtio-net-pci,netdev=net0', '-netdev', 'user,id=net0',
               '-display', 'none', '-serial', 'file:'+str(out/'serial.log'), '-no-reboot']
    (out/'argv.json').write_text(json.dumps(command, indent=2)+'\n')
    with (out/'qemu.log').open('w') as log:
        vm = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic()+120
        while time.monotonic()<deadline:
            if vm.poll() is not None:
                raise RuntimeError('QEMU stopped before transfer completed')
            serial = (out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
            if 'PANIC_HEX:' in serial:
                raise RuntimeError('guest panic')
            if 'TCP_CHUNKS_OK' in serial:
                break
            time.sleep(.1)
        else:
            raise TimeoutError('guest transfer did not complete')
        if sink.wait(timeout=20) != 0:
            raise RuntimeError('sink failed')
        records=json.loads((out/'sink.json').read_text())
        if len(records)!=1 or records[0]['bytes']!=4*1024*1024:
            raise RuntimeError('invalid sink result')
        (out/'result.json').write_text(json.dumps({'passed': True, 'sink': records}, indent=2)+'\n')
        print('VIRTIO_NET_QEMU_TRANSFER_PASS', flush=True)
finally:
    for child in (vm, sink):
        if child is None or child.poll() is not None:
            continue
        child.terminate()
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=10)
