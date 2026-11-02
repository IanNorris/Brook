#!/usr/bin/env python3
"""Exercise offline USB slot staging without touching a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
from stage_usb_ssd_recovery import stage, verify_pair
from hardware_update_protocol import record, KERNEL_MAX, INITRD_MAX


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def fixture(root, kernel, initrd, malformed=False):
    updates = root / 'UPDATES'; updates.mkdir()
    for slot in ('A', 'B'):
        d = updates / slot; d.mkdir()
        k = (bytes([0]) + kernel[1:]) if malformed and slot == 'A' else kernel
        for name, data, capacity in (('KERNEL.ELF', k, KERNEL_MAX), ('INITRD.IMG', initrd, INITRD_MAX)):
            with (d / name).open('xb') as f:
                f.write(data); f.truncate(capacity)
        (updates / ('SELECT0.BIN' if slot == 'A' else 'SELECT1.BIN')).write_bytes(
            record(23 if slot == 'A' else 22, 0 if slot == 'A' else 1, k, initrd))
    return updates


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--payload', required=True, type=Path)
    p.add_argument('--validator', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    kernel = (a.payload / 'kernel.elf').read_bytes(); initrd = (a.payload / 'INITRD.IMG').read_bytes()
    with tempfile.TemporaryDirectory(prefix='brook-usb-stage-') as tmp:
        root = Path(tmp) / 'good'; root.mkdir(); u = fixture(root, kernel, initrd)
        if os.stat(root).st_dev == os.stat(a.output).st_dev:
            raise RuntimeError('test needs /tmp and output on different filesystems')
        before = {name: digest(u / 'A' / name) for name in ('KERNEL.ELF', 'INITRD.IMG')}
        old = (u / 'SELECT0.BIN').read_bytes()
        result = stage(root, a.payload, a.output / 'backup-good', 23, a.validator.resolve())
        assert result['state'] == 'selected_and_verified' and (u / 'SELECT0.BIN').read_bytes() == old
        assert all(digest(u / 'A' / name) == value for name, value in before.items())
        assert verify_pair(u, (u / 'SELECT1.BIN').read_bytes(), a.validator.resolve())[:2] == (24, 1)
        before = {name: digest(u / 'B' / name) for name in ('KERNEL.ELF', 'INITRD.IMG')}
        active = (u / 'SELECT1.BIN').read_bytes()
        result = stage(root, a.payload, a.output / 'backup-reversed', 24, a.validator.resolve())
        assert result['state'] == 'selected_and_verified' and result['new_slot'] == 0
        assert (u / 'SELECT1.BIN').read_bytes() == active
        assert all(digest(u / 'B' / name) == value for name, value in before.items())
        assert verify_pair(u, (u / 'SELECT0.BIN').read_bytes(), a.validator.resolve())[:2] == (25, 0)
        bad = Path(tmp) / 'bad'; bad.mkdir(); u = fixture(bad, kernel, initrd, malformed=True)
        before = digest(u / 'B/KERNEL.ELF')
        try:
            stage(bad, a.payload, a.output / 'backup-bad', 23, a.validator.resolve())
        except ValueError as e:
            assert 'ineligible' in str(e)
        else:
            raise AssertionError('malformed active kernel accepted')
        assert digest(u / 'B/KERNEL.ELF') == before and not (a.output / 'backup-bad').exists()
    print('USB_RECOVERY_STAGING_TEST_PASS')


if __name__ == '__main__':
    main()
