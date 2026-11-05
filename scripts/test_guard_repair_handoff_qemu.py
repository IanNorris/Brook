#!/usr/bin/env python3
"""Exercise automatic repair, reset and guarded desktop on a disposable QEMU disk."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from hardware_update_protocol import record
from test_recovery_menu_boot import boot
from test_recovery_data_dispatch import metadata_hash,outside_esp_hash
from test_panic_fat_qemu import verify_record
from test_ext2_idle_flush_sata_qemu import DIRS,FILES


def check_pair(path,identity,automatic):
    manifest=json.loads((path/'manifest.json').read_text())
    if manifest.get('profile')!='qemu-fixture' or manifest.get('guard_identity')!=identity:
        raise ValueError('pair is not the identity-bound QEMU fixture')
    flags=manifest.get('kernel_build_flags',{})
    if flags.get('BROOK_SSD_AUTO_RECOVERY','OFF')!=('ON' if automatic else 'OFF') or flags.get('BROOK_SSD_GUARD_PENDING_TEST','OFF')!=('OFF' if automatic else 'ON'):
        raise ValueError('unexpected automatic/test kernel configuration')
    for name in ('KERNEL.ELF','INITRD.IMG','SELECT.BIN'):
        item=path/name;meta=manifest['files'][name]
        if item.is_symlink() or item.stat().st_size!=meta['bytes'] or hashlib.sha256(item.read_bytes()).hexdigest()!=meta['sha256']:
            raise ValueError('pair manifest mismatch: '+name)


def compact(source,dest,tree,size=8*1024*1024):
    tree.mkdir()
    subprocess.run(['mcopy','-s','-i',str(source),'::*',str(tree)],check=True)
    with dest.open('wb') as f:f.truncate(size)
    subprocess.run(['mformat','-i',str(dest),'::'],check=True)
    subprocess.run(['mcopy','-s','-i',str(dest),*[str(p) for p in tree.iterdir()],'::/'],check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','efi','firmware','desktop-pair','recovery-pair','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--identity',required=True)
    parser.add_argument('--inject-panic',action='store_true',help='Interrupt the first repair attempt after pending state using GDB')
    parser.add_argument('--auto-desktop',action='store_true',help='Use an automatic guard kernel with checked desktop handoff')
    parser.add_argument('--auto-desktop-panic',action='store_true',help='Inject a panic after the guarded desktop is mounted')
    parser.add_argument('--auto-desktop-reset',action='store_true',help='Require a QEMU-only timed reset after the desktop panic')
    parser.add_argument('--auto-desktop-production-reset',action='store_true',help='Use guarded desktop reset without the QEMU-only test selector')
    parser.add_argument('--auto-desktop-no-test-request',action='store_true',help='Request the QEMU reset hook on a build that must not contain it')
    parser.add_argument('--auto-desktop-hard-stall',action='store_true',help='Stall a real vCPU and require watchdog capture and reset')
    parser.add_argument('--auto-desktop-headless-panic',action='store_true',help='Force the panic framebuffer lookup to fail and check diagnostic sections')
    parser.add_argument('--auto-desktop-serial-stall',action='store_true',help='Stall at serial packet output after the disk capture')
    parser.add_argument('--auto-desktop-nested-panic',action='store_true',help='Re-enter KernelPanic on the panic owner after disk capture')
    parser.add_argument('--auto-desktop-peer-panic',action='store_true',help='Model an existing owner and verify another CPU parks')
    parser.add_argument('--auto-desktop-early-nested-panic',action='store_true',help='Re-enter panic before the initial peer halt')
    parser.add_argument('--auto-desktop-shutdown-panic',action='store_true',help='Inject during final shutdown sync and require reset inhibition')
    parser.add_argument('--auto-desktop-network-module',type=Path,help='Test the bridge transition using this matching virtio-net module')
    parser.add_argument('--auto-desktop-script-fail',action='store_true',help='Require the bridge to stay restricted after a startup-script failure')
    parser.add_argument('--auto-desktop-shutdown',action='store_true',help='Perform checked shutdown through the desktop bridge')
    parser.add_argument('--auto-desktop-dell-initrd',action='store_true',help='Exercise the Dell initrd and startup script using synthetic QEMU services')
    parser.add_argument('--normal-kernel',type=Path,help='Use this normal kernel for the final boot')
    parser.add_argument('--normal-network-module',type=Path,help='Matching virtio-net module for normal boot')
    a=parser.parse_args()
    if bool(a.normal_kernel)!=bool(a.normal_network_module) or (a.normal_kernel and not a.inject_panic) or (a.normal_kernel and a.auto_desktop):
        parser.error('normal kernel and network module must be supplied together with --inject-panic')
    if a.auto_desktop_panic and (not a.auto_desktop or a.inject_panic):
        parser.error('desktop panic requires auto desktop and excludes repair panic')
    if a.auto_desktop_reset and not a.auto_desktop_panic:
        parser.error('desktop reset requires desktop panic injection')
    if a.auto_desktop_production_reset and not a.auto_desktop_reset:
        parser.error('guarded desktop reset requires the reset fixture')
    if a.auto_desktop_no_test_request and (not a.auto_desktop_panic or a.auto_desktop_reset):
        parser.error('disabled-hook request requires an unarmed desktop panic fixture')
    if a.auto_desktop_shutdown_panic and not a.auto_desktop_reset:
        parser.error('shutdown panic requires the armed desktop reset fixture')
    if a.auto_desktop_hard_stall and (not a.auto_desktop_reset or a.auto_desktop_shutdown_panic):
        parser.error('hard stall requires desktop reset and excludes shutdown panic')
    if a.auto_desktop_headless_panic and (not a.auto_desktop_panic or a.auto_desktop_hard_stall or a.auto_desktop_shutdown_panic):
        parser.error('headless panic requires desktop panic and excludes stall and shutdown fixtures')
    if a.auto_desktop_serial_stall and (not a.auto_desktop_reset or a.auto_desktop_hard_stall or a.auto_desktop_shutdown_panic or a.auto_desktop_headless_panic):
        parser.error('serial stall requires desktop reset and excludes other panic fixtures')
    if a.auto_desktop_nested_panic and (not a.auto_desktop_panic or a.auto_desktop_hard_stall or a.auto_desktop_shutdown_panic or a.auto_desktop_headless_panic or a.auto_desktop_serial_stall):
        parser.error('nested panic requires desktop panic and excludes other fixtures')
    if a.auto_desktop_peer_panic and (not a.auto_desktop_reset or a.auto_desktop_hard_stall or a.auto_desktop_shutdown_panic or a.auto_desktop_headless_panic or a.auto_desktop_serial_stall or a.auto_desktop_nested_panic):
        parser.error('peer panic requires desktop reset and excludes other fixtures')
    if a.auto_desktop_early_nested_panic and (not a.auto_desktop_reset or a.auto_desktop_hard_stall or a.auto_desktop_shutdown_panic or a.auto_desktop_headless_panic or a.auto_desktop_serial_stall or a.auto_desktop_nested_panic or a.auto_desktop_peer_panic):
        parser.error('early nested panic requires desktop reset and excludes other fixtures')
    if a.auto_desktop_network_module and (not a.auto_desktop or a.auto_desktop_panic):
        parser.error('desktop bridge test requires a non-panic auto desktop fixture')
    if a.auto_desktop_script_fail and not a.auto_desktop_network_module:
        parser.error('startup-script failure requires the desktop bridge fixture')
    if a.auto_desktop_shutdown and (not a.auto_desktop_network_module or a.auto_desktop_script_fail):
        parser.error('checked shutdown requires a successful desktop bridge fixture')
    if a.auto_desktop_dell_initrd and (not a.auto_desktop or a.auto_desktop_panic or a.auto_desktop_network_module or a.inject_panic):
        parser.error('Dell initrd fixture requires a non-panic auto desktop')
    if len(a.identity)!=96 or any(c not in '0123456789abcdef' for c in a.identity):parser.error('invalid fixture identity')
    if a.image.is_symlink() or a.image.stat().st_size!=128*1024*1024:parser.error('expected disposable 128 MiB fixture')
    check_pair(a.desktop_pair,a.identity,a.auto_desktop);check_pair(a.recovery_pair,a.identity,True)
    if a.auto_desktop and json.loads((a.desktop_pair/'manifest.json').read_text()).get('kernel_build_flags',{}).get('BROOK_SSD_GUARDED_DESKTOP')!='ON':
        parser.error('automatic desktop pair lacks the guarded desktop build option')
    if a.auto_desktop_no_test_request and json.loads((a.desktop_pair/'manifest.json').read_text()).get('kernel_build_flags',{}).get('BROOK_SSD_GUARDED_DESKTOP_RESET_TEST')!='OFF':
        parser.error('disabled-hook request requires a build with desktop reset test off')
    if a.auto_desktop_production_reset:
        flags=json.loads((a.desktop_pair/'manifest.json').read_text()).get('kernel_build_flags',{})
        if flags.get('BROOK_SSD_GUARDED_DESKTOP_RESET')!='ON' or flags.get('BROOK_SSD_GUARDED_DESKTOP_RESET_TEST')!='OFF':
            parser.error('guarded desktop reset requires its build option and no QEMU test hook')
    a.output.mkdir(parents=True,exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='brk-auto-handoff-') as tmp:
        work=Path(tmp);disk=work/'disk.raw';shutil.copyfile(a.image,disk);media=str(disk)+'@@1048576'
        normal=work/'normal.img';recovery=work/'recovery.img'
        compact(a.desktop_pair/'INITRD.IMG',normal,work/'normal-tree',16*1024*1024 if a.auto_desktop_dell_initrd else 8*1024*1024)
        compact(a.recovery_pair/'INITRD.IMG',recovery,work/'recovery-tree')
        if a.auto_desktop_dell_initrd:
            # Keep the Dell startup script, but bind the private image to the
            # QEMU disk identity used by the recovery fixture.
            for name in ('ahci-write-target','ahci-boot-volume','ahci-data-volume'):
                data=subprocess.check_output(['mtype','-i',str(recovery),'::/ETC/'+name])
                subprocess.run(['mcopy','-o','-i',str(normal),'-','::/ETC/'+name],input=data,check=True)
            startup=subprocess.check_output(['mtype','-i',str(normal),'::/BOOT/INIT.RC'])
            if startup!=subprocess.check_output(['mtype','-i',str(a.desktop_pair/'INITRD.IMG'),'::/BOOT/INIT.RC']):
                raise RuntimeError('Dell startup script changed during compaction')
            (a.output/'dell-startup-script.sha256').write_text(hashlib.sha256(startup).hexdigest()+'\n')
        if a.auto_desktop:
            for directory in ('MEDIA','MEDIA/SYSTEM','DATA','NIX'):
                if subprocess.run(['mdir','-i',str(normal),'::/'+directory],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
                    subprocess.run(['mmd','-i',str(normal),'::/'+directory],check=True)
            volatile=work/'volatile-desktop';volatile.write_text('1\n')
            subprocess.run(['mcopy','-o','-i',str(normal),str(volatile),'::/ETC/volatile-desktop'],check=True)
        service=work/'GDSVC.ELF';script=work/'INIT.RC';defaults=work/'defaults.ini'
        subprocess.run(['cc','-nostdlib','-static','-fno-stack-protector','-no-pie','-O2',
                        str(ROOT/'scripts/workloads/guard_handoff_service.c'),'-o',str(service)],check=True)
        script.write_text('set wm\nset vfb none\nsched eevdf\nkconsole\nrun --service --once --console --uid 1000 /boot/GDSVC.ELF\n' +
                          ('shutdown\n' if a.auto_desktop_shutdown_panic else '') +
                          ('run --console /boot/DOES_NOT_EXIST.ELF\n' if a.auto_desktop_script_fail else ''))
        defaults.write_text('default-value\n')
        payloads=((service,'::/BOOT/GDSVC.ELF'),) if a.auto_desktop_dell_initrd else ((service,'::/BOOT/GDSVC.ELF'),(script,'::/BOOT/INIT.RC'),(defaults,'::/ETC/desktop-defaults.ini'))
        for src,target in payloads:
            subprocess.run(['mcopy','-o','-i',str(normal),str(src),target],check=True)
        if a.normal_kernel or a.auto_desktop_network_module:
            if subprocess.run(['mdir','-i',str(normal),'::/DRIVERS'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
                subprocess.run(['mmd','-i',str(normal),'::/DRIVERS'],check=True)
            subprocess.run(['mcopy','-o','-i',str(normal),str(a.normal_network_module or a.auto_desktop_network_module),'::/DRIVERS/virtio_net.mod'],check=True)
        selection=work/'select.bin';selection.write_bytes(record(1,0,(a.recovery_pair/'KERNEL.ELF').read_bytes(),recovery.read_bytes()))
        for src,target in ((a.efi,'::/EFI/BOOT/BOOTX64.EFI'),(a.normal_kernel or a.desktop_pair/'KERNEL.ELF','::/KERNEL/BROOK.ELF'),
                           (normal,'::/INITRD.IMG'),(a.recovery_pair/'KERNEL.ELF','::/RECOVERY/KERNEL.ELF'),
                           (recovery,'::/RECOVERY/INITRD.IMG'),(selection,'::/RECOVERY/SELECT.BIN')):
            subprocess.run(['mcopy','-o','-i',media,str(src),target],check=True)
        partition=work/'data.img'
        with disk.open('rb') as f:f.seek(133120*512+1024+104);fs_uuid=uuid.UUID(bytes=f.read(16))
        with partition.open('wb') as f:f.truncate(126976*512)
        subprocess.run(['mke2fs','-q','-F','-t','ext2','-b','4096','-I','128','-N','1024','-m','0',
                        '-O','none,filetype,sparse_super,large_file','-U',str(fs_uuid),str(partition)],check=True,stderr=subprocess.DEVNULL)
        witness=work/'placeholder';witness.write_text('synthetic fixture\n')
        for name in DIRS:
            for command in (f'mkdir /{name}',f'set_inode_field /{name} uid 1000',f'set_inode_field /{name} gid 1000'):
                subprocess.run(['debugfs','-w','-R',command,str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        for name in FILES:
            subprocess.run(['debugfs','-w','-R',f'write {witness} /{name}',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if name.startswith(('bin/','profile/bin/')):
                subprocess.run(['debugfs','-w','-R',f'set_inode_field /{name} mode 0100755',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if a.auto_desktop_dell_initrd:
            for name in ('waylandd','hardware-dbus','hardware-message'):
                target='/profile/bin/'+name
                subprocess.run(['debugfs','-w','-R',f'write {service} {target}',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                subprocess.run(['debugfs','-w','-R',f'set_inode_field {target} mode 0100755',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        with disk.open('r+b') as dst,partition.open('rb') as src:
            dst.seek(133120*512);shutil.copyfileobj(src,dst);dst.seek(133120*512+1024+58);dst.write(b'\0\0')
        guard=bytearray(128);guard[:8]=b'BRKRCV01';struct.pack_into('<QI',guard,8,1,1)
        guard[24:72]=bytes.fromhex(a.identity);struct.pack_into('<I',guard,124,zlib.crc32(guard[:124]))
        guard_file=work/'guard.bin';guard_file.write_bytes(guard)
        for name in ('GUARD0.BIN','GUARD1.BIN'):
            subprocess.run(['mcopy','-o','-i',media,str(guard_file),'::/RECOVERY/'+name],check=True)
        journal=work/'PANIC.BIN';journal.write_bytes(bytes(258*512))
        subprocess.run(['mcopy','-o','-i',media,str(journal),'::/PANIC.BIN'],check=True)
        metadata=metadata_hash(disk)
        journal_after=journal.read_bytes()
        if a.inject_panic:
            before_data=outside_esp_hash(disk)
            symbols=subprocess.check_output(['nm',str(a.recovery_pair/'KERNEL.ELF')],text=True)
            def symbol(name):
                matches=[int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1:]==[name]]
                if len(matches)!=1:raise RuntimeError('missing exact panic injection symbol '+name)
                return matches[0]
            pending=symbol('_ZN5brook12_GLOBAL__N_114RunExt2CheckerEPNS_6DeviceEb')
            panic=symbol('KernelPanic');scratch=symbol('_ZN5brookL8g_kbdBufE')
            interrupted=work/'interrupted';interrupted.mkdir()
            gdb=interrupted/'panic.gdb'
            gdb.write_text('set pagination off\nset confirm off\nset architecture i386:x86-64\n'
                           f'target remote {interrupted / "gdb.sock"}\nhbreak *{pending:#x}\ncontinue\n'
                           f'set {{unsigned long long}}{scratch:#x}=0x0054534554424447\n'
                           f'set $rdi={scratch:#x}\nset $rip={panic:#x}\ndelete breakpoints\ndetach\nquit\n')
            boot(disk,a.firmware,interrupted,False,'PANIC_DISK_CAPTURE_OK',120,True,sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA',expect_reset=True,reset_evidence_marker='PANIC_RECOVERY_RESET_WAIT',
                 extra_fw_cfg=['name=opt/brook/power,string=q35-reset'],gdb_script=gdb)
            shutil.copyfile(interrupted/'serial.log',a.output/'interrupted-serial.log')
            failed=(interrupted/'serial.log').read_text(errors='replace')
            if 'GUARD_ATTEMPT_NUMBER 1' not in failed or 'GUARD_PANIC_RESET armed=1' not in failed:
                raise RuntimeError('injected panic occurred before repair authorization')
            packet=next((line[10:] for line in failed.splitlines() if line.startswith('PANIC_HEX:')),None)
            if not packet:raise RuntimeError('injected panic packet missing')
            packet_bytes=bytes.fromhex(packet)
            journal_after=subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])
            verify_record(journal_after,0,1,packet_bytes)
            if outside_esp_hash(disk)!=before_data:raise RuntimeError('interrupted repair changed disk outside ESP')
        first=work/'repair';first.mkdir()
        boot(disk,a.firmware,first,False,'GUARD_COMPLETE_RESTART',120,True,sata_snapshot=False,
             sata_serial='BROOK-RECOVERY-QA',expect_reset=True,reset_evidence_marker='GUARD_COMPLETE_RESTART',
             failure_marker='GUARD_EXT2_INCOMPLETE',extra_fw_cfg=['name=opt/brook/power,string=q35-reset'])
        shutil.copyfile(first/'serial.log',a.output/'repair-serial.log')
        serial=(first/'serial.log').read_text(errors='replace')
        if a.inject_panic:
            prior=f'GUARD_PANIC_PRIOR sequence=1 bytes={len(packet_bytes)} crc={zlib.crc32(packet_bytes):x}'
            if prior not in serial or 'GUARD_ATTEMPT_NUMBER 2' not in serial:
                raise RuntimeError('retry did not recover the exact panic and consume attempt two')
        for marker in ('Recovery required; normal boot blocked.','brook-ssd-recovery-automatic-v1','GUARD_AUTO_NEEDS_CHECK','GUARD_EXT2_VERIFIED_PENDING','GUARD_COMPLETE_RESULT outcome=2'):
            if marker not in serial:raise RuntimeError('automatic repair did not reach '+marker)
        guards=[subprocess.check_output(['mtype','-i',media,'::/RECOVERY/'+name]) for name in ('GUARD0.BIN','GUARD1.BIN')]
        if guards[0]!=guards[1] or len(guards[0])!=128 or struct.unpack_from('<I',guards[0],16)[0]!=1 or struct.unpack_from('<I',guards[0],20)[0]!=0 or zlib.crc32(guards[0][:124])!=struct.unpack_from('<I',guards[0],124)[0]:
            raise RuntimeError('repair did not publish matching valid clean guards')
        if metadata_hash(disk)!=metadata:raise RuntimeError('repair changed GPT or disk metadata')
        with disk.open('rb') as src,partition.open('wb') as dst:src.seek(133120*512);dst.write(src.read(126976*512))
        with partition.open('rb') as f:
            f.seek(1024+58)
            if f.read(2)!=b'\x01\x00':raise RuntimeError('repair did not clean ext2')
        subprocess.run(['e2fsck','-f','-n',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        second=work/'desktop';second.mkdir()
        if a.normal_kernel:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0));network_port=probe.getsockname()[1]
            def retrieve():
                sys.path.insert(0,str(ROOT/'tools/vm_bridge'))
                from hardware import Hardware
                bridge=Hardware(work/'normal-registry');deadline=time.monotonic()+30
                while True:
                    try:session=bridge.attach('127.0.0.1',network_port)['session'];break
                    except (OSError,RuntimeError):
                        if time.monotonic()>=deadline:raise
                        time.sleep(.2)
                report=bridge.recovery_panic(session,str(work/'recovered.bin'))
                if Path(report['artifact_path']).read_bytes()!=packet_bytes or report['sequence']!=1:
                    raise RuntimeError('normal debug bridge returned the wrong panic packet')
                shutil.copyfile(report['artifact_path'],a.output/'retrieved-panic.bin')
                decoded=json.loads(subprocess.check_output(['python3',str(ROOT/'tools/crash_decoder.py'),
                                    '--packet',report['artifact_path'],'--json'],stderr=subprocess.DEVNULL))
                if (decoded.get('message')!='GDBTEST' or not decoded.get('log_tail',{}).get('available') or
                    'GUARD_ATTEMPT_NUMBER 1' not in decoded['log_tail']['text']):
                    raise RuntimeError('retrieved panic did not retain its reason and repair log')
                (a.output/'decoded-panic.json').write_text(json.dumps(decoded,indent=2)+'\n')
            boot(disk,a.firmware,second,False,'GUARD_DESKTOP_SERVICE_PASS',120,True,sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA',network_port=network_port,on_marker=retrieve)
        elif a.auto_desktop_early_nested_panic:
            kernel=a.desktop_pair/'KERNEL.ELF'
            symbols=subprocess.check_output(['nm',str(kernel)],text=True)
            def early_symbol(name):
                matches=[int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1:]==[name]]
                if len(matches)!=1:raise RuntimeError('missing early panic symbol '+name)
                return matches[0]
            launch=early_symbol('_ZN5brook15ShellExecScriptEPKc')
            panic=early_symbol('KernelPanic');halt=early_symbol('_ZN5brook13SmpHaltAllAPsEv')
            scratch=early_symbol('_ZN5brookL8g_kbdBufE')
            gdb=second/'early-nested.gdb'
            gdb.write_text('set pagination off\nset confirm off\nset architecture i386:x86-64\n'
                           f'target remote {second / "gdb.sock"}\nhbreak *{launch:#x}\ncontinue\n'
                           f'set {{unsigned long long}}{scratch:#x}=0x0054534554424447\n'
                           f'set $rdi={scratch:#x}\nset $rip={panic:#x}\ndelete breakpoints\n'
                           f'hbreak *{halt:#x}\ncontinue\nset $rdi={scratch:#x}\nset $rip={panic:#x}\n'
                           f'delete breakpoints\nhbreak *{halt:#x}\ncontinue\n'
                           'set $halt_return=*(unsigned long long*)$rsp\ndelete breakpoints\n'
                           'hbreak *$halt_return\ncontinue\nprintf "PANIC_NESTED_PEERS_HALTED %u\\n", $rax\n'
                           'delete breakpoints\ndetach\nquit\n')
            seen=[]
            boot(disk,a.firmware,second,False,'PANIC_RECOVERY_RESET_WAIT',120,True,sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA',gdb_script=gdb,expect_reset=True,
                 reset_evidence_marker='PANIC_RECOVERY_RESET_WAIT',on_marker=lambda:seen.append(time.monotonic()),
                 extra_fw_cfg=['name=opt/brook/power,string=q35-reset','name=opt/guard-desktop-panic-reset,string=test-1'])
            serial=(second/'serial.log').read_text(errors='replace')
            debugger_log=(second/'qemu.log').read_text(errors='replace')
            if ('PANIC_NESTED_PEERS_HALTED 1' not in debugger_log or '*** DOUBLE PANIC' not in serial or
                not seen or time.monotonic()-seen[0]<4 or 'PANIC_DISK_CAPTURE_OK' in serial):
                raise RuntimeError('early nested panic did not halt peers and wait before reset')
            if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal_after:
                raise RuntimeError('early nested panic unexpectedly changed the journal')
            shutil.copyfile(second/'serial.log',a.output/'desktop-early-nested-serial.log')
            (a.output/'desktop-early-nested-gdb.log').write_text(debugger_log)
            retry=work/'early-nested-recovery';retry.mkdir()
            boot(disk,a.firmware,retry,False,'GUARD_COMPLETE_RESTART',120,True,sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA',expect_reset=True,reset_evidence_marker='GUARD_COMPLETE_RESTART',
                 failure_marker='GUARD_EXT2_INCOMPLETE',extra_fw_cfg=['name=opt/brook/power,string=q35-reset'])
            shutil.copyfile(retry/'serial.log',a.output/'desktop-early-nested-recovery.log')
            if 'GUARD_EXT2_VERIFIED_PENDING' not in (retry/'serial.log').read_text(errors='replace'):
                raise RuntimeError('early nested recovery did not verify ext2')
            with disk.open('rb') as src,partition.open('wb') as dst:src.seek(133120*512);dst.write(src.read(126976*512))
            subprocess.run(['e2fsck','-f','-n',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            print('GUARD_DESKTOP_EARLY_NESTED_RESET_PASS')
            return
        elif a.auto_desktop_peer_panic:
            kernel=a.desktop_pair/'KERNEL.ELF'
            symbols=subprocess.check_output(['nm',str(kernel)],text=True)
            def peer_symbol(name):
                matches=[int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1:]==[name]]
                if len(matches)!=1:raise RuntimeError('missing peer panic symbol '+name)
                return matches[0]
            launch=peer_symbol('_ZN5brook15ShellExecScriptEPKc')
            panic=peer_symbol('KernelPanic');owner=peer_symbol('_ZL12g_panicOwner')
            disassembly=subprocess.check_output(['objdump','-d','--disassemble=KernelPanic',str(kernel)],text=True)
            halts=re.findall(r'^\s*([0-9a-f]+):\s+f4\s+hlt\s*$',disassembly,re.MULTILINE)
            if len(halts)!=1:raise RuntimeError('unexpected peer halt instruction')
            gdb=second/'peer.gdb'
            gdb.write_text('set pagination off\nset confirm off\nset architecture i386:x86-64\n'
                           f'target remote {second / "gdb.sock"}\nhbreak *{launch:#x}\ncontinue\n'
                           'set $cpu=$_thread-1\nif $cpu < 0 || $cpu >= 2\necho Invalid vCPU mapping\\n\nquit 1\nend\n'
                           f'set *(unsigned int*){owner:#x}=2-$cpu\nset $rip={panic:#x}\n'
                           f'delete breakpoints\nhbreak *{int(halts[0],16):#x}\ncontinue\n'
                           'printf "PANIC_PEER_PARKED cpu=%d owner=%d\\n", $cpu, 2-$cpu\n'
                           'delete breakpoints\ndetach\nquit\n')
            boot(disk,a.firmware,second,False,'GUARD_DESKTOP_PANIC_RESET armed=1',120,True,sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA',gdb_script=gdb,hold_after_marker=7,
                 forbidden='PANIC_RECOVERY_RESET_WAIT',failure_marker='GUARD_DESKTOP_HANDOFF_FAILED',
                 extra_fw_cfg=['name=opt/brook/power,string=q35-reset','name=opt/guard-desktop-panic-reset,string=test-1'])
            serial=(second/'serial.log').read_text(errors='replace')
            debugger_log=(second/'qemu.log').read_text(errors='replace')
            if not re.search(r'PANIC_PEER_PARKED cpu=[01] owner=[12]',debugger_log) or '*** DOUBLE PANIC' in serial:
                raise RuntimeError('peer panic did not park without entering owner reset')
            if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal_after:
                raise RuntimeError('peer panic changed the journal')
            shutil.copyfile(second/'serial.log',a.output/'desktop-peer-serial.log')
            (a.output/'desktop-peer-gdb.log').write_text(debugger_log)
            print('GUARD_DESKTOP_PEER_PANIC_PARK_PASS')
            return
        elif a.auto_desktop_panic:
            symbols=subprocess.check_output(['nm',str(a.desktop_pair/'KERNEL.ELF')],text=True)
            def desktop_symbol(name):
                matches=[int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1:]==[name]]
                if len(matches)!=1:raise RuntimeError('missing desktop panic injection symbol '+name)
                return matches[0]
            launch=desktop_symbol('_ZN5brook25AhciSystemDirectFinalSyncEv' if a.auto_desktop_shutdown_panic else '_ZN5brook15ShellExecScriptEPKc')
            gdb=second/'panic.gdb'
            if a.auto_desktop_hard_stall:
                from elftools.elf.elffile import ELFFile
                progress=desktop_symbol('_ZN5brookL18g_recoveryProgressE')
                spin=None
                with (a.desktop_pair/'KERNEL.ELF').open('rb') as executable:
                    for segment in ELFFile(executable).iter_segments():
                        if segment['p_type']=='PT_LOAD' and segment['p_flags']&1:
                            at=segment.data().find(b'\xeb\xfe')
                            if at>=0:spin=segment['p_vaddr']+at;break
                if spin is None:raise RuntimeError('no executable self-loop for watchdog fixture')
                body=('delete breakpoints\nset $cpu=$_thread-1\n'
                      'if $cpu < 0 || $cpu >= 4\necho Invalid vCPU mapping\\n\nquit 1\nend\n'
                      'printf "WATCHDOG_STALLED_CPU %d\\n", $cpu\n'
                      'python import time; print("WATCHDOG_STALL_MONOTONIC %.6f" % time.monotonic(), flush=True)\n'
                      f'set *(unsigned long long*)({progress:#x}+16*$cpu)=0\n'
                      f'set *(unsigned int*)({progress+8:#x}+16*$cpu)=0\n'
                      f'set *(unsigned char*)({progress+12:#x}+16*$cpu)=1\n'
                      f'set $eflags=$eflags & ~0x200\nset $rip={spin:#x}\ndetach\nquit\n')
            else:
                panic=desktop_symbol('KernelPanic');scratch=desktop_symbol('_ZN5brookL8g_kbdBufE')
                body=(f'set {{unsigned long long}}{scratch:#x}=0x0054534554424447\n'
                      f'set $rdi={scratch:#x}\nset $rip={panic:#x}\ndelete breakpoints\n')
                if a.auto_desktop_headless_panic:
                    fb=desktop_symbol('_ZN5brook19CompositorGetPhysFbEPj')
                    body+=(f'hbreak *{fb:#x}\ncontinue\nset $rax=0\nset $rip=*(unsigned long long*)$rsp\n'
                           'set $rsp=$rsp+8\necho PANIC_FRAMEBUFFER_FORCED_NULL\\n\ndelete breakpoints\n')
                if a.auto_desktop_serial_stall:
                    from elftools.elf.elffile import ELFFile
                    dump=desktop_symbol('_ZN5brookL18DumpPacketToSerialEPKhj')
                    spin=None
                    with (a.desktop_pair/'KERNEL.ELF').open('rb') as executable:
                        for segment in ELFFile(executable).iter_segments():
                            if segment['p_type']=='PT_LOAD' and segment['p_flags']&1:
                                at=segment.data().find(b'\xeb\xfe')
                                if at>=0:spin=segment['p_vaddr']+at;break
                    if spin is None:raise RuntimeError('no executable self-loop for serial stall fixture')
                    body+=(f'hbreak *{dump:#x}\ncontinue\nset $rip={spin:#x}\n'
                           'echo PANIC_SERIAL_STALLED\\n\ndelete breakpoints\n')
                if a.auto_desktop_nested_panic:
                    dump=desktop_symbol('_ZN5brookL18DumpPacketToSerialEPKhj')
                    body+=(f'hbreak *{dump:#x}\ncontinue\nset $rdi={scratch:#x}\nset $rip={panic:#x}\n'
                           'echo PANIC_NESTED_INJECTED\\n\ndelete breakpoints\n')
                body+='detach\nquit\n'
            gdb.write_text('set pagination off\nset confirm off\nset architecture i386:x86-64\n'
                           f'target remote {second / "gdb.sock"}\nhbreak *{launch:#x}\ncontinue\n'+body)
            capture_seen=[]
            try:
                boot(disk,a.firmware,second,False,'PANIC_DISK_CAPTURE_OK',120,True,sata_snapshot=False,
                     sata_serial='BROOK-RECOVERY-QA',gdb_script=gdb,failure_marker='GUARD_DESKTOP_HANDOFF_FAILED',
                     expect_reset=a.auto_desktop_reset and not (a.auto_desktop_shutdown_panic or a.auto_desktop_serial_stall),
                     hold_after_marker=7 if a.auto_desktop_shutdown_panic or a.auto_desktop_serial_stall or not a.auto_desktop_reset else 0,reset_evidence_marker='PANIC_RECOVERY_RESET_WAIT',
                     on_marker=lambda:capture_seen.append(time.monotonic()),
                     smp=4 if a.auto_desktop_hard_stall else 2,
                     extra_fw_cfg=(['name=opt/brook/power,string=q35-reset']+
                         ([] if a.auto_desktop_production_reset else ['name=opt/guard-desktop-panic-reset,string=test-1'])
                         if a.auto_desktop_reset or a.auto_desktop_no_test_request else None))
            finally:
                for name in ('serial.log','qemu.log'):
                    if (second/name).exists():shutil.copyfile(second/name,a.output/('desktop-attempt-'+name))
            if a.auto_desktop_hard_stall:
                debugger_log=(second/'qemu.log').read_text(errors='replace')
                (a.output/'desktop-stall-gdb.log').write_text(debugger_log)
                stalled=re.search(r'WATCHDOG_STALLED_CPU ([0-3])',debugger_log)
                started=re.search(r'WATCHDOG_STALL_MONOTONIC ([0-9.]+)',debugger_log)
                if not stalled or not started or not capture_seen or capture_seen[0]-float(started.group(1))<25:
                    raise RuntimeError('watchdog did not observe a sustained vCPU stall')
            if a.auto_desktop_reset and not (a.auto_desktop_shutdown_panic or a.auto_desktop_serial_stall) and (not capture_seen or time.monotonic()-capture_seen[0]<4):
                raise RuntimeError('desktop panic reset occurred before the expected delay')
        elif a.auto_desktop_network_module:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0));network_port=probe.getsockname()[1]
            bridge_state={}
            def bridge_before():
                sys.path.insert(0,str(ROOT/'tools/vm_bridge'))
                from hardware import Hardware
                bridge=Hardware(work/'desktop-registry');deadline=time.monotonic()+6
                while True:
                    try:attached=bridge.attach('127.0.0.1',network_port);break
                    except (OSError,RuntimeError):
                        if time.monotonic()>=deadline:raise
                        time.sleep(.1)
                session=attached['session'];caps=attached['guest']
                if caps.get('recovery_only') is not True or caps.get('power_api')!=0 or bridge.command(session,'bridge job caps').get('job_api')!=0:
                    raise RuntimeError('bridge opened before the desktop handoff completed')
                bridge_state.update(bridge=bridge,session=session,before=caps)
            def bridge_after():
                bridge=bridge_state['bridge'];session=bridge_state['session']
                doctor=bridge.doctor(session)
                if a.auto_desktop_script_fail:
                    if (not doctor.get('identity_matches') or doctor['capabilities'].get('command_jobs') or
                        doctor['capabilities'].get('hardware_power') or bridge.command(session,'bridge job caps').get('job_api')!=0):
                        raise RuntimeError('failed startup exposed normal bridge capabilities')
                    (a.output/'bridge-transition.json').write_text(json.dumps({'before':bridge_state['before'],'after':doctor},indent=2)+'\n')
                    return
                if not doctor.get('identity_matches') or not doctor['capabilities'].get('command_jobs') or not doctor['capabilities'].get('hardware_power'):
                    raise RuntimeError('normal bridge capabilities did not appear after handoff')
                job=bridge.guest_run(session,uuid.uuid4().hex,['/boot/GDSVC.ELF'],cwd='/',uid=1000,wait_s=10)
                if job.get('state')!='completed' or job.get('exit',{}).get('code')!=0 or 'GUARD_DESKTOP_SERVICE_PASS' not in job.get('stdout',{}).get('text',''):
                    raise RuntimeError('desktop bridge guest job did not complete: '+str(job))
                evidence={'before':bridge_state['before'],'after':doctor,'job':job}
                if a.auto_desktop_shutdown:
                    request=uuid.uuid4().hex;state=bridge.power(session,request,'shutdown');deadline=time.monotonic()+60
                    while state.get('state')!='ready':
                        if state.get('state')=='failed' or time.monotonic()>=deadline:
                            raise RuntimeError('checked desktop shutdown did not reach ready: '+str(state))
                        time.sleep(.2);state=bridge.power(session,request,'status')
                    final=bridge.doctor(session)['guest']['writable_desktop']
                    if not final.get('clean_finalized') or final.get('dirty_session') or final.get('state_uncertain') or final.get('final_error'):
                        raise RuntimeError('desktop did not report a clean final sync: '+str(final))
                    evidence.update(shutdown=state,final_desktop=final)
                (a.output/'bridge-transition.json').write_text(json.dumps(evidence,indent=2)+'\n')
            boot(disk,a.firmware,second,False,'GUARD_DESKTOP_HANDOFF_SCRIPT status='+('-1' if a.auto_desktop_script_fail else '0'),120,True,sata_snapshot=False,
                 hold_after_marker=5,sata_serial='BROOK-RECOVERY-QA',failure_marker='GUARD_DESKTOP_HANDOFF_FAILED',
                 network_port=network_port,intermediate_marker='GUARD_DESKTOP_BRIDGE_TEST_WINDOW',
                 on_intermediate=bridge_before,on_marker=bridge_after,
                 extra_fw_cfg=['name=opt/brook/power,string=q35-reset','name=opt/guard-bridge-window,string=test-1'])
        elif a.auto_desktop:
            boot(disk,a.firmware,second,False,'GUARD_DESKTOP_HANDOFF_SCRIPT status=0',120,True,sata_snapshot=False,
                 hold_after_marker=5,sata_serial='BROOK-RECOVERY-QA',failure_marker='GUARD_DESKTOP_HANDOFF_FAILED')
        else:
            boot(disk,a.firmware,second,False,'GUARD_JOURNAL_NORMAL_SCRIPT status=0',120,True,sata_snapshot=False,
                 hold_after_marker=5,sata_serial='BROOK-RECOVERY-QA',failure_marker='GUARD_JOURNAL_DESKTOP_FAIL',
                 extra_fw_cfg=['name=opt/guard-journal-pin,string=test-b','name=opt/guard-panic-capture,string=test-1'])
        shutil.copyfile(second/'serial.log',a.output/'desktop-serial.log')
        serial=(second/'serial.log').read_text(errors='replace')
        if a.auto_desktop_dell_initrd:
            for executable in ('/nix/profile/bin/waylandd','/nix/profile/bin/hardware-dbus',
                               '/nix/profile/bin/hardware-message','/boot/HWSTATUS.ELF'):
                if 'ELF HASH: '+executable+' ' not in serial:
                    raise RuntimeError('Dell startup did not launch '+executable)
        if ((not a.auto_desktop_panic and 'GUARD_DESKTOP_SERVICE_PASS' not in serial) or (not a.auto_desktop and 'brook-ssd-recovery-automatic-v1' in serial) or
            'Recovery required; normal boot blocked.' in serial):
            raise RuntimeError('guarded desktop service did not start after repair')
        if a.auto_desktop_panic:
            if ('ahci direct: ready ' not in serial or
                (('PANIC_RECOVERY_RESET_WAIT' in serial)!=(a.auto_desktop_reset and not (a.auto_desktop_shutdown_panic or a.auto_desktop_serial_stall))) or
                (a.auto_desktop_reset and 'GUARD_DESKTOP_PANIC_RESET armed=1' not in serial)):
                raise RuntimeError('desktop panic occurred before mount or unexpectedly armed reset')
            if a.auto_desktop_no_test_request and 'GUARD_DESKTOP_PANIC_RESET armed=' in serial:
                raise RuntimeError('disabled desktop reset test hook was unexpectedly active')
            packet=next((line[10:] for line in serial.splitlines() if line.startswith('PANIC_HEX:')),None)
            journal_after=subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])
            if a.auto_desktop_serial_stall or a.auto_desktop_nested_panic:
                debugger_log=(second/'qemu.log').read_text(errors='replace')
                (a.output/'desktop-interrupted-serial-gdb.log').write_text(debugger_log)
                expected_injection='PANIC_SERIAL_STALLED' if a.auto_desktop_serial_stall else 'PANIC_NESTED_INJECTED'
                if expected_injection not in debugger_log or packet:
                    raise RuntimeError('panic was not interrupted before serial packet emission')
                if a.auto_desktop_nested_panic and '*** DOUBLE PANIC (handler faulted) ***' not in serial:
                    raise RuntimeError('nested panic handler did not run')
                length=struct.unpack_from('<I',journal_after,12)[0]
                if not 0<length<=65536:raise RuntimeError('invalid journal packet length')
                packet=journal_after[512:512+length].hex()
                packet_file=a.output/'desktop-interrupted-serial-panic.bin';packet_file.write_bytes(bytes.fromhex(packet))
                decoded=json.loads(subprocess.check_output(['python3',str(ROOT/'tools/crash_decoder.py'),
                                   '--packet',str(packet_file),'--json'],stderr=subprocess.DEVNULL))
                if decoded.get('message')!='GDBTEST':raise RuntimeError('serial-stall journal packet lost its reason')
                (a.output/'desktop-interrupted-serial-panic.json').write_text(json.dumps(decoded,indent=2)+'\n')
            if not packet:raise RuntimeError('desktop panic packet missing')
            verify_record(journal_after,0,1,bytes.fromhex(packet))
            if a.auto_desktop_hard_stall:
                cpu=stalled.group(1)
                if f'WATCHDOG: CPU{cpu} timer accounting unchanged for 3 checks' not in serial:
                    raise RuntimeError('watchdog did not report the stalled CPU')
                packet_file=a.output/'desktop-watchdog-panic.bin';packet_file.write_bytes(bytes.fromhex(packet))
                decoded=json.loads(subprocess.check_output(['python3',str(ROOT/'tools/crash_decoder.py'),
                                   '--packet',str(packet_file),'--json'],stderr=subprocess.DEVNULL))
                if (decoded.get('message')!=f'Recovery watchdog: CPU{cpu} timer accounting stalled for 30 checks' or
                    not decoded.get('log_tail',{}).get('available')):
                    raise RuntimeError('watchdog packet did not retain its reason and log')
                (a.output/'desktop-watchdog-panic.json').write_text(json.dumps(decoded,indent=2)+'\n')
            if a.auto_desktop_headless_panic:
                debugger_log=(second/'qemu.log').read_text(errors='replace')
                (a.output/'desktop-headless-gdb.log').write_text(debugger_log)
                if 'PANIC_FRAMEBUFFER_FORCED_NULL' not in debugger_log:
                    raise RuntimeError('headless panic framebuffer override did not run')
                packet_file=a.output/'desktop-headless-panic.bin';packet_file.write_bytes(bytes.fromhex(packet))
                decoded=json.loads(subprocess.check_output(['python3',str(ROOT/'tools/crash_decoder.py'),
                                   '--packet',str(packet_file),'--json'],stderr=subprocess.DEVNULL))
                if (decoded.get('message')!='GDBTEST' or not decoded.get('system_info') or
                    not decoded.get('processes') or not decoded.get('stack_dump') or
                    not decoded.get('cpus') or not decoded.get('log_tail',{}).get('available')):
                    raise RuntimeError('headless packet is missing diagnostic sections')
                (a.output/'desktop-headless-panic.json').write_text(json.dumps(decoded,indent=2)+'\n')
        if a.normal_kernel and f'NORMAL_PANIC_PRIOR sequence=1 bytes={len(packet_bytes)} crc={zlib.crc32(packet_bytes):x}' not in serial:
            raise RuntimeError('normal boot did not validate the captured panic packet')
        if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal_after:raise RuntimeError('journal unexpectedly changed')
        for index in range(2):
            if subprocess.check_output(['mtype','-i',media,f'::/RECOVERY/GUARD{index}.BIN'])!=guards[index]:
                raise RuntimeError('desktop changed verified clean guard records')
        if metadata_hash(disk)!=metadata:raise RuntimeError('desktop changed GPT or disk metadata')
        if a.auto_desktop_panic:
            # boot() observed reset or stopped the halted VM. A fresh boot
            # must select recovery, retain the
            # panic packet, repair the dirty data volume and request reset.
            retry=work/'desktop-panic-recovery';retry.mkdir()
            boot(disk,a.firmware,retry,False,'GUARD_COMPLETE_RESTART',120,True,sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA',expect_reset=True,reset_evidence_marker='GUARD_COMPLETE_RESTART',
                 failure_marker='GUARD_EXT2_INCOMPLETE',extra_fw_cfg=['name=opt/brook/power,string=q35-reset'])
            shutil.copyfile(retry/'serial.log',a.output/'desktop-panic-recovery.log')
            retry_text=(retry/'serial.log').read_text(errors='replace')
            packet_bytes=bytes.fromhex(packet)
            prior=f'GUARD_PANIC_PRIOR sequence=1 bytes={len(packet_bytes)} crc={zlib.crc32(packet_bytes):x}'
            if prior not in retry_text or 'GUARD_EXT2_VERIFIED_PENDING' not in retry_text:
                raise RuntimeError('desktop panic recovery lost the packet or did not verify repair')
            if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal_after:
                raise RuntimeError('desktop panic recovery changed the journal')
        with disk.open('rb') as src,partition.open('wb') as dst:src.seek(133120*512);dst.write(src.read(126976*512))
        if a.auto_desktop_shutdown:
            with partition.open('rb') as source:
                source.seek(1024+58)
                if source.read(2)!=b'\x01\x00':raise RuntimeError('checked shutdown did not persist the clean marker')
            if 'AHCI_DIRECT_SESSION_CLEAN' not in serial:raise RuntimeError('checked shutdown did not complete direct sync')
        subprocess.run(['e2fsck','-f','-n',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if not a.auto_desktop_panic:
            content=subprocess.check_output(['debugfs','-R','cat /home/brook/SERVICE.TXT',str(partition)],stderr=subprocess.DEVNULL)
            if content!=b'guarded desktop service\n':raise RuntimeError('desktop service data missing')
        fat=work/'esp.img'
        with disk.open('rb') as src,fat.open('wb') as dst:src.seek(2048*512);dst.write(src.read(131072*512))
        subprocess.run(['fsck.fat','-n',str(fat)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if a.auto_desktop_shutdown:
            # Boot the same clean disk through a QEMU snapshot so this final
            # check cannot modify the already-verified source image.
            digest=hashlib.sha256(disk.read_bytes()).hexdigest()
            third=work/'checked-reboot';third.mkdir()
            boot(disk,a.firmware,third,False,'GUARD_DESKTOP_SERVICE_PASS',120,True,sata_snapshot=True,
                 hold_after_marker=5,sata_serial='BROOK-RECOVERY-QA',failure_marker='GUARD_DESKTOP_HANDOFF_FAILED')
            third_text=(third/'serial.log').read_text(errors='replace')
            if ('GUARD_CHECK_RESULT exit=0' not in third_text or 'GUARD_DESKTOP_HANDOFF_SCRIPT status=0' not in third_text or
                'Recovery required; normal boot blocked.' in third_text):
                raise RuntimeError('checked desktop did not return after clean shutdown')
            if hashlib.sha256(disk.read_bytes()).hexdigest()!=digest:
                raise RuntimeError('snapshot reboot modified the verified source disk')
            shutil.copyfile(third/'serial.log',a.output/'checked-reboot-serial.log')
        print('GUARD_DESKTOP_SHUTDOWN_PANIC_INHIBITED_PASS' if a.auto_desktop_shutdown_panic else
              'GUARD_DESKTOP_PANIC_RESET_PASS' if a.auto_desktop_reset else
              'GUARD_DESKTOP_PANIC_CAPTURE_PASS' if a.auto_desktop_panic else 'GUARD_REPAIR_TO_DESKTOP_PASS')

if __name__=='__main__':main()
