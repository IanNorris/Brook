#!/usr/bin/env python3
"""Build a private UEFI/RAM-disk VMware probe, without existing user disks.

Run inside nix-shell after scripts/build.sh Release. This is a bring-up image,
not the full Nix desktop: no persistent storage, network, audio or 3D driver.
The destination must not already exist. No host devices are opened.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 512
ESP_START = 2048
ESP_SECTORS = 131072


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/release')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    for name in ['mkfs.fat', 'mcopy', 'mmd', 'mtype', 'sfdisk', 'qemu-img']:
        if not shutil.which(name):
            parser.error('Missing tool: '+name+' (run inside nix-shell)')
    bootloader, kernel = build/'bootloader/BOOTX64.efi', build/'kernel/BROOK.elf'
    modules = sorted((build/'kernel/drivers').glob('*.mod'))
    if not bootloader.is_file() or not kernel.is_file() or not modules:
        parser.error('Missing built bootloader, kernel or driver modules')
    # Exclusive directory creation prevents replacing an existing VM or image.
    output.mkdir(parents=True, exist_ok=False)
    (output/'build/esp').mkdir(parents=True)
    drivers = output/'build/kernel/drivers'
    drivers.mkdir(parents=True)
    for module in modules:
        shutil.copy2(module, drivers/module.name)
    env = dict(os.environ, BROOK_BUILD_DIR=str(output/'build'))
    env.pop('BROOK_PROVIDER_PAYLOAD', None)
    run('bash', ROOT/'scripts/make_initrd.sh', '--release', env=env)
    initrd = output/'build/esp/INITRD.IMG'
    # The kernel's normal boot script is /boot/INIT.RC. In this probe that
    # directory lives in the RAM disk, so no guest storage driver is needed.
    run('mmd', '-i', initrd, '::BOOT')
    script = output/'INIT.RC'
    script.write_text('set wm\nset vfb none\nkconsole\n')
    run('mcopy', '-i', initrd, script, '::BOOT/INIT.RC')
    assert subprocess.check_output(['mtype', '-i', str(initrd), '::BOOT/INIT.RC']) == script.read_bytes()
    esp = output/'esp.img'
    with esp.open('xb') as stream:
        stream.truncate(ESP_SECTORS*SECTOR)
    run('mkfs.fat', '-F', '32', '-n', 'BROOK_EFI', esp)
    run('mmd', '-i', esp, '::EFI', '::EFI/BOOT', '::KERNEL')
    cfg = output/'BROOK.CFG'
    cfg.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n')
    exports = [(bootloader, '::EFI/BOOT/BOOTX64.EFI'),
               (kernel, '::KERNEL/BROOK.ELF'),
               (initrd, '::INITRD.IMG'), (cfg, '::BROOK.CFG')]
    for source, target in exports:
        run('mcopy', '-i', esp, source, target)
        data = subprocess.check_output(['mtype', '-i', str(esp), target])
        if hashlib.sha256(data).hexdigest() != sha(source):
            raise RuntimeError('ESP round-trip mismatch: '+target)
    disk = output/'brook.raw'
    with disk.open('xb') as stream:
        stream.truncate(128*1024*1024)
    run('sfdisk', '--no-reread', '--no-tell-kernel', disk,
        input=f'label: gpt\nunit: sectors\n\nstart={ESP_START},size={ESP_SECTORS},type=U,name="Brook EFI"\n', text=True)
    table = json.loads(subprocess.check_output(['sfdisk', '--json', str(disk)]))['partitiontable']
    partition = table['partitions'][0]
    assert table['label'] == 'gpt' and table['sectorsize'] == SECTOR
    assert partition['start'] == ESP_START and partition['size'] == ESP_SECTORS
    with disk.open('r+b') as destination, esp.open('rb') as source:
        destination.seek(ESP_START*SECTOR)
        shutil.copyfileobj(source, destination, 1024*1024)
    vmdk = output/'brook.vmdk'
    run('qemu-img', 'convert', '-f', 'raw', '-O', 'vmdk', '-o', 'subformat=monolithicSparse', disk, vmdk)
    run('qemu-img', 'compare', '-f', 'raw', '-F', 'vmdk', disk, vmdk)
    vmx = output/'Brook.vmx'
    vmx.write_text('''.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "20"
displayName = "Brook UEFI bring-up"
guestOS = "other-64"
firmware = "efi"
uefi.secureBoot.enabled = "FALSE"
memsize = "2048"
numvcpus = "2"
cpuid.coresPerSocket = "2"
mks.enable3d = "FALSE"
ide0:0.present = "TRUE"
ide0:0.fileName = "brook.vmdk"
ethernet0.present = "FALSE"
sound.present = "FALSE"
usb.present = "FALSE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "serial.log"
serial0.startConnected = "TRUE"
''')
    report = dict(scope='UEFI + RAM-backed software desktop; this generated image requires runtime acceptance',
                  bootloader_sha256=sha(bootloader), kernel_sha256=sha(kernel),
                  initrd_sha256=sha(initrd), vmdk_sha256=sha(vmdk),
                  modules={m.name: sha(m) for m in modules},
                  vmx=str(vmx), inputs='Built Brook artifacts only; no user/game disks')
    (output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Probe prepared:', vmx)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
