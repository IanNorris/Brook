#!/usr/bin/env python3
"""Prepare a matched hardware kernel/initrd pair without touching a device.

Every driver already present in the source initrd must have a matching module
in the supplied build, with the requested module ABI. The source policy and
other initrd contents are preserved. This script does not activate the pair.
The supplied ABI is not extracted from the kernel; a separate exact-pair boot
test is required to verify kernel/module compatibility.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from hardware_recovery_fsck import validate_checker

MAGIC = 0x4B524F42
KERNEL_MAX = 8 * 1024 * 1024
INITRD_MAX = 64 * 1024 * 1024
FAT_CHECKER_SHA256 = '99cc8160e7a076ecd9cb04e13bdbe71c4aad4c64bfdc43ea83a016c8fee9d5e3'
VULKAN_DESKTOP_START = (b'run --service --once --console --uid 1000 '
                        b'/boot/VKSUP.ELF /boot/VKRENDER.ELF')
VULKAN_PARTIAL_START = (b'run --service --once --console --uid 1000 '
                        b'/boot/VKSUP.ELF --partial /boot/VKRENDER.ELF')
VULKAN_OLD_STARTS = (b'run --service --once --uid 1000 /boot/VKSUP.ELF /boot/VKRENDER.ELF',
                     b'run --service --once --uid 1000 /boot/VKSUP.ELF --partial /boot/VKRENDER.ELF')
GPU_SOCKET = b'/tmp/brook-desktop-gpu.sock'
GPU_PROVIDER = b'run --service --once --uid 1000 /boot/INTLWL.ELF'
KERNEL_CONSOLE_START = b'kconsole'
# With --hide-kernel-console the log window is not opened at boot; this Apps
# menu entry (the repository's data/shortcuts/kernel_console.rc) reopens it.
KERNEL_CONSOLE_SHORTCUT_PATH = 'BOOT/SHORTCUTS/KCONSOLE.RC'
KERNEL_CONSOLE_SHORTCUT = (Path(__file__).resolve().parents[1] / 'data/shortcuts/kernel_console.rc').read_bytes()


def hide_kernel_console(script):
    """Remove the single boot-time `kconsole` line, keeping everything else."""
    lines=script.splitlines()
    if lines.count(KERNEL_CONSOLE_START)!=1:
        raise ValueError('hiding Kernel Console requires exactly one kconsole startup line')
    lines.remove(KERNEL_CONSOLE_START)
    return b'\n'.join(lines)+b'\n'


def desktop_startup(script, hide_console=False, vulkan=False, partial=False, gpu_source=False,
                    gpu_source_trace=False, async_partial=False):
    """The exact startup script prepared (and later verified) for these options."""
    if hide_console:script=hide_kernel_console(script)
    if vulkan:script=add_vulkan_desktop_start(script,partial,gpu_source,gpu_source_trace,async_partial)
    return script

def add_vulkan_desktop_start(script, partial=False, gpu_source=False, gpu_source_trace=False, async_partial=False):
    if gpu_source_trace and not gpu_source:
        raise ValueError('GPU source trace requires GPU source opt-in')
    if not script or len(script)>65536 or b'\0' in script or any(
            b'/boot/VKSUP.ELF ' in line for line in script.splitlines()):
        raise ValueError('invalid startup script or duplicate compositor service')
    if gpu_source:
        lines=script.splitlines()
        if lines.count(GPU_PROVIDER)!=1:
            raise ValueError('GPU source requires exactly one embedded provider startup')
        lines[lines.index(GPU_PROVIDER)]=GPU_PROVIDER+b' --gpu-source '+GPU_SOCKET
        script=b'\n'.join(lines)+b'\n'
    start=VULKAN_PARTIAL_START if partial or async_partial else VULKAN_DESKTOP_START
    if async_partial:start=start.replace(b' --partial ',b' --async-partial ')
    if gpu_source:
        start=start.replace(b'/boot/VKRENDER.ELF',b'--gpu-source '+GPU_SOCKET+b' /boot/VKRENDER.ELF')
    if gpu_source_trace:
        start=start.replace(b'/boot/VKRENDER.ELF',b'--trace-gpu-source /boot/VKRENDER.ELF')
    return script+(b'' if script.endswith(b'\n') else b'\n')+start+b'\n'


def regular(path, maximum):
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
            raise ValueError(f'{path}: not a bounded regular file')
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            return stream.read(maximum + 1)
    finally:
        os.close(fd)


def module_version(data):
    section = ELFFile(io.BytesIO(data)).get_section_by_name('.modinfo')
    if section is None or len(section.data()) < 8:
        raise ValueError('missing module metadata')
    magic, version = struct.unpack_from('<II', section.data())
    if magic != MAGIC:
        raise ValueError('invalid module magic')
    return version


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(source, build, abi, payload_id, output, expected_kernel_sha256, panic_exporter=None, fat_checker=None,
            enable_efi_poweroff=False, enable_usb_boot_poweroff=False, enable_vulkan_desktop=False,
            vulkan_supervisor=None,vulkan_renderer=None,enable_vulkan_partial=False,
            gpu_source_provider=None,enable_gpu_source_trace=False,enable_ac_loss_poweroff=False,
            enable_dell_acpi_monitor=False,enable_dell_bcs_present=False,enable_vulkan_async_partial=False, enable_irq_signal_probe=False,
            hide_kernel_console_window=False):
    if enable_gpu_source_trace and not gpu_source_provider:
        raise ValueError('GPU source trace requires GPU source opt-in')
    if gpu_source_provider and not enable_vulkan_desktop:
        raise ValueError('GPU source requires Vulkan desktop opt-in')
    if (enable_vulkan_partial or enable_vulkan_async_partial) and not enable_vulkan_desktop:
        raise ValueError('partial rendering requires Vulkan desktop opt-in')
    if enable_dell_bcs_present and not enable_vulkan_desktop:
        raise ValueError('Dell BCS presentation requires Vulkan desktop opt-in')
    if enable_vulkan_desktop != bool(vulkan_supervisor and vulkan_renderer) or bool(vulkan_supervisor)!=bool(vulkan_renderer):
        raise ValueError('Vulkan desktop requires both explicit executables and opt-in')
    if enable_usb_boot_poweroff and not enable_efi_poweroff:
        raise ValueError('USB boot power-off requires the EFI power-off opt-in')
    if enable_ac_loss_poweroff and not (enable_efi_poweroff and enable_dell_acpi_monitor):
        raise ValueError('AC loss power-off requires the EFI power-off and Dell monitor opt-ins')
    if enable_dell_acpi_monitor:
        cache=regular(build/'CMakeCache.txt',1024*1024).decode()
        if 'BROOK_DELL_ACPI_MONITOR:BOOL=ON' not in cache.splitlines():
            raise ValueError('The selected kernel does not include the Dell AC monitor')
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,95}', payload_id):
        raise ValueError('invalid payload ID')
    kernel = regular(build / 'BROOK.elf', KERNEL_MAX)
    if not re.fullmatch(r'[0-9a-f]{64}', expected_kernel_sha256) or digest(kernel) != expected_kernel_sha256:
        raise ValueError('kernel hash does not match the explicitly selected build')
    source_data = regular(source, INITRD_MAX)
    # Enumerate the exact bytes that will be copied, not a second read of the
    # source pathname that could refer to different contents by this point.
    with tempfile.TemporaryDirectory(prefix='brook-hardware-pair-') as directory:
        captured = Path(directory) / 'source.img'
        captured.write_bytes(source_data)
        listing = subprocess.check_output(['mdir', '-a', '-b', '-i', str(captured), '::/DRIVERS/*'], text=True)
    names = [line.rsplit('/', 1)[-1] for line in listing.splitlines() if line]
    if not names or len({n.lower() for n in names}) != len(names):
        raise ValueError('missing or ambiguous source modules')
    inventory = []
    for name in names:
        if not re.fullmatch(r'[A-Za-z0-9_-]+\.[Mm][Oo][Dd]', name):
            raise ValueError('unexpected module name')
        module = build / 'drivers' / name.lower()
        contents = regular(module, 16 * 1024 * 1024)
        if module_version(contents) != abi:
            raise ValueError(f'{name}: mismatched module ABI')
        inventory.append((name, module, digest(contents)))
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        image = output / 'INITRD.IMG'
        image.write_bytes(source_data)
        (output / 'kernel.elf').write_bytes(kernel)
        marker = output / 'hardware-payload-id'
        marker.write_text(payload_id + '\n')
        for name, module, _ in inventory:
            subprocess.run(['mcopy', '-o', '-i', str(image), str(module), '::/DRIVERS/' + name], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(image), str(marker), '::/ETC/hardware-payload-id'], check=True)
        enabled=[]
        vulkan_files={}
        if enable_vulkan_desktop:
            for name,path in (('VKSUP.ELF',vulkan_supervisor),('VKRENDER.ELF',vulkan_renderer)):
                data=regular(path,8*1024*1024)
                if not data.startswith(b'\x7fELF'):raise ValueError(name+' must be an ELF executable')
                copied=output/name;copied.write_bytes(data)
                subprocess.run(['mcopy','-o','-i',str(image),str(copied),'::/BOOT/'+name],check=True)
                if subprocess.check_output(['mtype','-i',str(image),'::/BOOT/'+name])!=data:
                    raise RuntimeError(name+' readback mismatch')
                vulkan_files[name]=digest(data)
            if gpu_source_provider:
                data=regular(gpu_source_provider,8*1024*1024)
                if not data.startswith(b'\x7fELF'):raise ValueError('INTLWL.ELF must be an ELF executable')
                copied=output/'INTLWL.ELF';copied.write_bytes(data)
                subprocess.run(['mcopy','-o','-i',str(image),str(copied),'::/BOOT/INTLWL.ELF'],check=True)
                if subprocess.check_output(['mtype','-i',str(image),'::/BOOT/INTLWL.ELF'])!=data:
                    raise RuntimeError('INTLWL.ELF readback mismatch')
                vulkan_files['INTLWL.ELF']=digest(data)
            enabled.append('vulkan-desktop')
            if enable_vulkan_partial:enabled.append('vulkan-partial')
            if enable_vulkan_async_partial:enabled.append('vulkan-async-partial')
            if gpu_source_provider:enabled.append('gpu-source')
            if enable_gpu_source_trace:enabled.append('gpu-source-trace')
        if enable_vulkan_desktop or hide_kernel_console_window:
            original=subprocess.check_output(['mtype','-i',str(image),'::/BOOT/INIT.RC'])
            startup=desktop_startup(original,hide_kernel_console_window,enable_vulkan_desktop,enable_vulkan_partial,
                                    bool(gpu_source_provider),enable_gpu_source_trace,enable_vulkan_async_partial)
            startup_path=output/'INIT.RC';startup_path.write_bytes(startup)
            subprocess.run(['mcopy','-o','-i',str(image),str(startup_path),'::/BOOT/INIT.RC'],check=True)
            if subprocess.check_output(['mtype','-i',str(image),'::/BOOT/INIT.RC'])!=startup:
                raise RuntimeError('desktop startup script readback mismatch')
        if hide_kernel_console_window:
            existing=subprocess.run(['mtype','-i',str(image),'::/'+KERNEL_CONSOLE_SHORTCUT_PATH],capture_output=True)
            if existing.returncode==0 and existing.stdout!=KERNEL_CONSOLE_SHORTCUT:
                raise ValueError('source initrd has a different '+KERNEL_CONSOLE_SHORTCUT_PATH)
            if subprocess.run(['mdir','-i',str(image),'::/BOOT/SHORTCUTS'],capture_output=True).returncode:
                subprocess.run(['mmd','-i',str(image),'::/BOOT/SHORTCUTS'],check=True)
            shortcut=output/'KCONSOLE.RC';shortcut.write_bytes(KERNEL_CONSOLE_SHORTCUT)
            subprocess.run(['mcopy','-o','-i',str(image),str(shortcut),'::/'+KERNEL_CONSOLE_SHORTCUT_PATH],check=True)
            if subprocess.check_output(['mtype','-i',str(image),'::/'+KERNEL_CONSOLE_SHORTCUT_PATH])!=KERNEL_CONSOLE_SHORTCUT:
                raise RuntimeError('Kernel Console shortcut readback mismatch')
            enabled.append('hidden-kernel-console')
        for selected,name in ((enable_efi_poweroff,'efi-poweroff'),
                              (enable_usb_boot_poweroff,'usb-boot-poweroff'),
                              (enable_dell_acpi_monitor,'dell-acpi-monitor'),
                              (enable_ac_loss_poweroff,'ac-loss-poweroff'),
                              (enable_dell_bcs_present,'dell-gpu-bcs-present'),
                              (enable_irq_signal_probe,'irq-signal-probe')):
            if selected:
                item=output/name;item.write_bytes(b'1\n')
                subprocess.run(['mcopy','-o','-i',str(image),str(item),'::/ETC/'+name],check=True)
                if subprocess.check_output(['mtype','-i',str(image),'::/ETC/'+name])!=b'1\n':
                    raise RuntimeError(name+' readback mismatch')
                enabled.append(name)
        export_info = None
        if panic_exporter is not None:
            export_info = validate_checker(panic_exporter)
            subprocess.run(['mcopy', '-o', '-i', str(image), str(panic_exporter), '::/BOOT/PANICLOG.ELF'], check=True)
            copied = subprocess.check_output(['mtype', '-i', str(image), '::/BOOT/PANICLOG.ELF'])
            if len(copied) != export_info['bytes'] or digest(copied) != export_info['sha256']:
                raise RuntimeError('panic exporter readback mismatch')
        fat_info = None
        if fat_checker is not None:
            fat_info = validate_checker(fat_checker)
            if fat_info['sha256'] != FAT_CHECKER_SHA256:
                raise ValueError('FAT checker does not match the recovery kernel pin')
            subprocess.run(['mcopy', '-o', '-i', str(image), str(fat_checker), '::/BOOT/FATFSCK.ELF'], check=True)
            copied = subprocess.check_output(['mtype', '-i', str(image), '::/BOOT/FATFSCK.ELF'])
            if len(copied) != fat_info['bytes'] or digest(copied) != fat_info['sha256']:
                raise RuntimeError('FAT checker readback mismatch')
        for name, _, expected in inventory:
            data = subprocess.check_output(['mtype', '-i', str(image), '::/DRIVERS/' + name])
            if digest(data) != expected:
                raise RuntimeError(f'{name}: readback mismatch')
        if subprocess.check_output(['mtype', '-i', str(image), '::/ETC/hardware-payload-id']) != marker.read_bytes():
            raise RuntimeError('payload ID readback mismatch')
        result = {'scope': 'prepared private files only; no deployment or boot test',
                  'payload_id': payload_id, 'module_abi': abi,
                  'source_initrd_sha256': digest(source_data),
                  'kernel_sha256': digest(kernel), 'initrd_sha256': digest(image.read_bytes()),
                  'modules': {name: sha for name, _, sha in inventory}}
        result['enabled_boot_options'] = enabled
        if vulkan_files:result['vulkan_executables_sha256']=vulkan_files
        if export_info is not None: result['panic_journal_exporter'] = export_info
        if fat_info is not None: result['fat_checker'] = fat_info
        (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        return result
    except BaseException:
        shutil.rmtree(output)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-initrd', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--abi', type=int, required=True)
    p.add_argument('--payload-id', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--expected-kernel-sha256', required=True)
    p.add_argument('--panic-exporter', type=Path, help='Embed the static panic journal export utility')
    p.add_argument('--fat-checker', type=Path, help='Embed the pinned static recovery FAT checker')
    p.add_argument('--enable-efi-poweroff',action='store_true',help='Enable validated EFI power-off in the candidate')
    p.add_argument('--enable-usb-boot-poweroff',action='store_true',help='Enable the one-minute USB-reset recovery policy')
    p.add_argument('--enable-ac-loss-poweroff',action='store_true',help='Enable orderly shutdown after confirmed AC loss')
    p.add_argument('--enable-dell-acpi-monitor',action='store_true',help='Start the admitted Dell ACPI monitor')
    p.add_argument('--enable-vulkan-desktop',action='store_true',help='Start the experimental supervised Vulkan desktop service')
    p.add_argument('--enable-vulkan-partial',action='store_true',help='Use experimental partial updates with the Vulkan desktop')
    p.add_argument('--enable-vulkan-async-partial',action='store_true',help='Use two-buffer asynchronous Vulkan desktop partial updates')
    p.add_argument('--enable-dell-bcs-present',action='store_true',help='Try the Dell Broadwell blitter for Vulkan desktop presentation')
    p.add_argument('--vulkan-supervisor',type=Path,help='Pinned supervisor executable to embed in the boot image')
    p.add_argument('--vulkan-renderer',type=Path,help='Pinned renderer executable to embed in the boot image')
    p.add_argument('--gpu-source-provider',type=Path,help='Embed and explicitly enable the paired direct-source Wayland provider')
    p.add_argument('--enable-gpu-source-trace',action='store_true',help='Log the first direct presentation per window incarnation')
    p.add_argument('--enable-irq-signal-probe',action='store_true',help='Enable experimental interrupt-return signal delivery')
    p.add_argument('--hide-kernel-console',action='store_true',
                   help='Do not open the Kernel Console window at boot; add an Apps menu entry that opens it')
    a = p.parse_args()
    print(json.dumps(prepare(a.source_initrd, a.build, a.abi, a.payload_id, a.output,
                             a.expected_kernel_sha256, a.panic_exporter, a.fat_checker,
                             a.enable_efi_poweroff, a.enable_usb_boot_poweroff, a.enable_vulkan_desktop,
                             a.vulkan_supervisor,a.vulkan_renderer,a.enable_vulkan_partial,a.gpu_source_provider,
                             a.enable_gpu_source_trace,a.enable_ac_loss_poweroff,a.enable_dell_acpi_monitor,
                             a.enable_dell_bcs_present,a.enable_vulkan_async_partial,a.enable_irq_signal_probe,
                             a.hide_kernel_console), indent=2))


if __name__ == '__main__':
    main()
