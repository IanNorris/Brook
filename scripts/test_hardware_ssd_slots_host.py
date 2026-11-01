#!/usr/bin/env python3
"""Test slot initialization only in private host directories."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record, KERNEL_MAX, INITRD_MAX


def main():
    kernel = (ROOT / 'build/hardware/dell-xhci-recovery-candidate-20261031/kernel.elf').read_bytes()
    initrd = b'private-initrd' * 64
    efi = b'private-efi' * 64
    expected = hashlib.sha256(efi).hexdigest()
    with tempfile.TemporaryDirectory(dir=ROOT / 'build/hardware') as tmp:
        tmp = Path(tmp)
        executable = tmp / 'test'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'src/tests/test_ssd_slots/host_wrapper.c'), '-o', str(executable)], check=True)
        def fixture(name):
            root = tmp / name
            (root / 'KERNEL').mkdir(parents=True); (root / 'EFI/BOOT').mkdir(parents=True)
            (root / 'KERNEL/BROOK.ELF').write_bytes(kernel)
            (root / 'INITRD.IMG').write_bytes(initrd)
            (root / 'EFI/BOOT/BOOTX64.EFI').write_bytes(efi)
            (root / 'BROOK.CFG').write_bytes(b'TARGET=KERNEL\\BROOK.ELF\n')
            return root
        wrong = fixture('wrong-efi')
        assert subprocess.run([executable, wrong, '0' * 64]).returncode != 0
        assert not (wrong / 'UPDATES').exists()
        existing = fixture('existing'); (existing / 'UPDATES').mkdir()
        assert subprocess.run([executable, existing, expected]).returncode != 0
        assert (existing / 'BROOK.CFG').read_bytes() == b'TARGET=KERNEL\\BROOK.ELF\n'
        large = fixture('large'); (large / 'BROOK.CFG').write_bytes(b'x' * 4090)
        assert subprocess.run([executable, large, expected]).returncode != 0
        assert not (large / 'UPDATES').exists()
        for stage in ('directory', 'slot-a-kernel', 'slot-a', 'slot-b-kernel', 'slot-b', 'selector-a', 'selectors', 'activation-written'):
            root = fixture('interrupted-' + stage)
            env = dict(os.environ, BROOK_TEST_STOP=stage)
            assert subprocess.run([executable, root, expected], env=env).returncode != 0
            config = (root / 'BROOK.CFG').read_bytes()
            if stage != 'activation-written':
                assert config == b'TARGET=KERNEL\\BROOK.ELF\n'
            else:
                assert config == b'TARGET=KERNEL\\BROOK.ELF\n\nUPDATE_SLOTS=1\n'
                assert (root / 'UPDATES/SELECT0.BIN').read_bytes() == record(1, 0, kernel, initrd)
                assert (root / 'UPDATES/SELECT1.BIN').read_bytes() == record(2, 1, kernel, initrd)
            assert (root / 'KERNEL/BROOK.ELF').read_bytes() == kernel
            assert (root / 'INITRD.IMG').read_bytes() == initrd
        root = fixture('success')
        subprocess.run([executable, root, expected], check=True)
        assert (root / 'BROOK.CFG').read_bytes() == b'TARGET=KERNEL\\BROOK.ELF\n\nUPDATE_SLOTS=1\n'
        assert (root / 'KERNEL/BROOK.ELF').read_bytes() == kernel
        assert (root / 'INITRD.IMG').read_bytes() == initrd
        for index, slot in enumerate(('A', 'B')):
            base = root / 'UPDATES' / slot
            for name, data, capacity in (('KERNEL.ELF', kernel, KERNEL_MAX), ('INITRD.IMG', initrd, INITRD_MAX)):
                path = base / name; assert path.stat().st_size == capacity
                with path.open('rb') as f: assert f.read(len(data)) == data
            assert (root / 'UPDATES' / f'SELECT{index}.BIN').read_bytes() == record(index + 1, index, kernel, initrd)
        print('SSD_SLOT_HOST_PASS')


if __name__ == '__main__': main()
