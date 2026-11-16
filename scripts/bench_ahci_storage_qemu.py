#!/usr/bin/env python3
"""Disposable QEMU/KVM storage benchmark on the direct SATA (AHCI) desktop data path.

    nix-shell --run 'python3 scripts/bench_ahci_storage_qemu.py --kernel build/release/kernel/BROOK.elf \
        --out build/bench/ahci-storage/candidate'

Builds a fresh 1 GiB GPT disk (FAT ESP that also boots the guest, plus an ext2 data volume admitted
through the production AHCI write-target/data-volume records for this QEMU identity), boots the given
kernel with the current release initrd, and runs scripts/workloads/storage_io_bench.c against
/data/home. While it runs, the host polls `bridge stats` (taskbar R/W rates and per-source device
totals) over the debug bridge (virtio-net). Writes serial.log, bridge-stats.jsonl, ahci.json and a
summary.json under --out. The disk is deleted afterwards unless --keep-disk; e2fsck -fn checks it first.

This is a QEMU AHCI (ICH9) measurement. It does not establish physical SSD behaviour, interrupt
delivery on the Dell's controller, or power-loss durability.
"""
import argparse, json, re, shutil, socket, subprocess, sys, tempfile, time, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge'), str(ROOT / 'scripts')]
sys.dont_write_bytecode = True
from test_ext2_idle_flush_sata_qemu import DIRS, FILES

SECTORS = 2 * 1024 * 1024  # 1 GiB
SERIAL = 'BROOK-IOBENCH'


def run(*argv, **kw):
    return subprocess.run([str(a) for a in argv], check=True, **kw)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--build-dir', type=Path, default=ROOT / 'build/release')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--mib', type=int, default=256)
    p.add_argument('--smp', type=int, default=4)
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--compiler', default='x86_64-unknown-linux-musl-gcc')
    p.add_argument('--polled', action='store_true', help='Add /etc/ahci-polled (interrupt opt-out control)')
    p.add_argument('--keep-disk', action='store_true')
    p.add_argument('--spin-us', type=int, help='Set the AHCI spin budget at attach (bridge hardware ahci-spin-us)')
    p.add_argument('--profile-ms', type=int, default=0,
                   help='Kernel profile from the start of the first write phase; saved as profile.txt')
    p.add_argument('--sched', default='eevdf', help="INIT.RC scheduler policy ('' keeps the boot default)")
    a = p.parse_args()
    kernel = a.kernel.resolve(strict=True)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    firmware = Path(subprocess.check_output(['sh', '-c', 'dirname "$OVMF_CODE"'], text=True).strip())
    run(ROOT / 'scripts/make_initrd.sh', '--release', stdout=subprocess.DEVNULL)
    source_initrd = a.build_dir / 'esp/INITRD.IMG'
    with tempfile.TemporaryDirectory(prefix='brook-iobench-', dir=out) as temp:
        d = Path(temp)
        disk = d / 'sata.raw'
        with disk.open('wb') as f:
            f.truncate(SECTORS * 512)
        layout = 'label: gpt\nunit: sectors\n\nstart=2048,size=65536,type=uefi\nstart=67584,type=linux\n'
        run('sfdisk', '--no-reread', disk, input=layout.encode(), stdout=subprocess.DEVNULL)
        table = json.loads(subprocess.check_output(['sfdisk', '--json', str(disk)]))['partitiontable']
        esp_part, data_part = table['partitions']
        disk_guid = uuid.UUID(table['id']).bytes_le.hex()
        # ESP: boots the guest and is the admitted system volume.
        esp = d / 'esp.img'
        with esp.open('wb') as f:
            f.truncate(esp_part['size'] * 512)
        run('mkfs.vfat', '-F', '16', esp, stdout=subprocess.DEVNULL)
        for name in ('EFI', 'EFI/BOOT', 'KERNEL'):
            run('mmd', '-i', esp, '::/' + name)
        run('mcopy', '-i', esp, a.build_dir / 'bootloader/BOOTX64.efi', '::/EFI/BOOT/BOOTX64.EFI')
        run('mcopy', '-i', esp, kernel, '::/KERNEL/BROOK.ELF')
        cfg = d / 'BROOK.CFG'
        cfg.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=0\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n')
        run('mcopy', '-i', esp, cfg, '::/BROOK.CFG')
        # Data volume: the synthetic desktop tree on ext2 (4 KiB blocks).
        tree = d / 'tree'
        for name in DIRS:
            (tree / name).mkdir(parents=True, exist_ok=True)
        for name in FILES:
            (tree / name).write_text('synthetic fixture\n')
            (tree / name).chmod(0o755)
        part = d / 'data.img'
        with part.open('wb') as f:
            f.truncate(data_part['size'] * 512)
        fs_uuid = uuid.uuid4()
        run('mkfs.ext2', '-q', '-F', '-b', '4096', '-I', '128', '-N', '4096', '-m', '0',
            '-O', 'none,filetype,sparse_super,large_file', '-U', str(fs_uuid), '-d', tree, part)
        for name in DIRS:
            for field in ('uid', 'gid'):
                run('debugfs', '-w', '-R', f'set_inode_field /{name} {field} 1000', part,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Initrd: release initrd plus direct-SATA admission records and the benchmark.
        initrd = d / 'initrd.img'
        shutil.copyfile(source_initrd, initrd)
        for name in ('BOOT', 'DATA', 'NIX', 'MEDIA', 'MEDIA/SYSTEM', 'MNT', 'MNT/PROBE', 'PROC'):
            subprocess.run(['mmd', '-i', str(initrd), '::/' + name], stderr=subprocess.DEVNULL)
        records = {
            'ahci-observe': '1\n', 'ahci-read-only': '1\n', 'volatile-desktop': '1\n',
            'ahci-write-target': f'BRKAHCI1\n32902,10530,0,31,2,0,{SECTORS},512,512\nQEMU HARDDISK\n{SERIAL}\n',
            'ahci-boot-volume': f'BRKESP1\n{disk_guid}\n{uuid.UUID(esp_part["uuid"]).bytes_le.hex()}\n'
                                f'{esp_part["start"]},{esp_part["size"]}\n',
            'ahci-data-volume': f'BRKDATA1\n{disk_guid}\n{uuid.UUID(data_part["uuid"]).bytes_le.hex()}\n'
                                f'{data_part["start"]},{data_part["size"]}\n{fs_uuid.bytes.hex()}\n',
        }
        if a.polled:
            records['ahci-polled'] = '1\n'
        for name, content in records.items():
            item = d / name
            item.write_text(content)
            run('mcopy', '-o', '-i', initrd, item, '::/ETC/' + name)
        bench = d / 'iobench.elf'
        run(a.compiler, '-static', '-O2', ROOT / 'scripts/workloads/storage_io_bench.c', '-o', bench)
        run('mcopy', '-o', '-i', initrd, bench, '::/BOOT/IOBENCH.ELF')
        init = d / 'INIT.RC'
        init.write_text('set wm\nset vfb none\n' + (f'sched {a.sched}\n' if a.sched else '') + f'run --console --uid 0 /boot/IOBENCH.ELF /data/home {a.mib}\n')
        run('mcopy', '-o', '-i', initrd, init, '::/BOOT/INIT.RC')
        run('mcopy', '-i', esp, initrd, '::/INITRD.IMG')
        with disk.open('r+b') as o:
            for image, start in ((esp, esp_part['start']), (part, data_part['start'])):
                o.seek(start * 512)
                with image.open('rb') as i:
                    shutil.copyfileobj(i, o, length=1 << 20)
        esp.unlink()
        part.unlink()
        variables = d / 'vars.fd'
        shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        serial = out / 'serial.log'
        command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
                   '-smp', str(a.smp), '-m', '2048', '-nodefaults',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
                   '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
                   '-drive', f'if=none,id=sata,format=raw,cache=none,aio=native,file={disk}',
                   '-device', f'ide-hd,drive=sata,bus=ide.0,serial={SERIAL},bootindex=1',
                   '-netdev', f'user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
                   '-device', 'virtio-net-pci,netdev=net0',
                   '-display', 'none', '-serial', f'file:{serial}', '-no-reboot']
        (out / 'qemu-command.json').write_text(json.dumps(command, indent=1) + '\n')
        from hardware import Hardware
        bridge = Hardware(d / 'registry')
        stats_log = (out / 'bridge-stats.jsonl').open('w')
        session = None
        result = {'kernel': str(kernel), 'mib': a.mib, 'smp': a.smp, 'polled_marker': a.polled}
        with (out / 'qemu.log').open('wb') as errors:
            qemu = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + a.timeout
                while time.monotonic() < deadline:
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if session is None and 'IOBENCH_START' in text:
                        try:
                            session = bridge.attach('127.0.0.1', port)['session']
                            if a.spin_us is not None:
                                result['spin'] = bridge.command(session, f'bridge hardware ahci-spin-us {a.spin_us}')
                            if a.profile_ms:
                                result['profile_start'] = bridge.command(session, f'prof start {a.profile_ms} /media/system')
                        except (OSError, RuntimeError, ValueError):
                            pass
                    if session:
                        try:
                            sample = bridge.command(session, 'bridge stats')
                            stats_log.write(json.dumps({'host_time': time.time(), 'storage': sample.get('storage')}) + '\n')
                            stats_log.flush()
                        except Exception as error:  # observation only
                            stats_log.write(json.dumps({'host_time': time.time(), 'error': str(error)}) + '\n')
                    if any(m in text for m in ('IOBENCH_DONE', 'IOBENCH_VERIFY_FAILED', 'PANIC', 'No bootable option')):
                        break
                    if qemu.poll() is not None:
                        break
                    time.sleep(0.5)
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if session:
                    try:
                        (out / 'ahci.json').write_text(json.dumps(bridge.command(session, 'bridge hardware ahci-ro'), indent=1) + '\n')
                        result['final_stats'] = bridge.command(session, 'bridge stats').get('storage')
                    except Exception as error:
                        result['ahci_error'] = str(error)
            finally:
                qemu.kill()
                qemu.wait()
                stats_log.close()
        result['mounted_direct'] = 'ahci direct: ready' in text
        result['msi'] = re.findall(r'ahci: MSI completion[^\n]*', text)
        result['reverted'] = 'reverting to polled completion' in text
        result['phases'] = {}
        for m in re.finditer(r'IOBENCH phase=(\S+) (.*)', text):
            result['phases'][m.group(1)] = {k: float(v) for k, v in re.findall(r'(\w+)=([\d.]+)', m.group(2))}
        rates = []
        for line in (out / 'bridge-stats.jsonl').read_text().splitlines():
            storage = json.loads(line).get('storage') or {}
            if 'taskbar_write_bytes_per_second' in storage:
                rates.append((storage['taskbar_read_bytes_per_second'], storage['taskbar_write_bytes_per_second']))
        result['taskbar_max_read_bps'] = max((r for r, _ in rates), default=None)
        result['taskbar_max_write_bps'] = max((w for _, w in rates), default=None)
        result['done'] = 'IOBENCH_DONE' in text
        # Independent filesystem check of the data volume.
        part = d / 'data-check.img'
        run('dd', f'if={disk}', f'of={part}', 'bs=1M', 'iflag=skip_bytes,count_bytes',
            f'skip={data_part["start"] * 512}', f'count={data_part["size"] * 512}', 'status=none')
        fsck = subprocess.run(['e2fsck', '-fn', str(part)], capture_output=True, text=True)
        result['e2fsck_exit'] = fsck.returncode
        if a.profile_ms:
            esp_at = f'{disk}@@{esp_part["start"] * 512}'
            names = subprocess.run(['mdir', '-b', '-i', esp_at, '::/'], capture_output=True, text=True).stdout.split()
            for name in [n for n in names if 'PROF' in n.upper()][-1:]:
                run('mcopy', '-n', '-i', esp_at, name, out / 'profile.txt')
        if a.keep_disk:
            shutil.move(str(disk), out / 'sata.raw')
    (out / 'summary.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps(result, indent=1))
    return 0 if result['done'] and result['e2fsck_exit'] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
