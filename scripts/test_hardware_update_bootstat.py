#!/usr/bin/env python3
"""Read-only first-install journal verification on synthetic sector replies."""

import contextlib
import struct
import sys
import unittest
import uuid
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from hardware_update_bootstat import ESP_TYPE, inspect_bootstat

SECTORS = 72200
ESP = 2048
FAT = ESP + 32
FAT_SIZE = 540
ROOT = FAT + 2 * FAT_SIZE
RECOVERY = ROOT + 1


def entry(name, attr, cluster=0, length=0):
    raw = bytearray(32)
    raw[:11] = name
    raw[11] = attr
    struct.pack_into('<H', raw, 20, cluster >> 16)
    struct.pack_into('<HI', raw, 26, cluster & 65535, length)
    return raw


class Disk:
    def __init__(self):
        self.blocks = {}
        self.requested = []
        self.boot = 'a' * 32
        self.wrong_lba = False
        mbr = self.block(0)
        mbr[450] = 0xee
        struct.pack_into('<II', mbr, 454, 1, SECTORS - 1)
        mbr[-2:] = b'\x55\xaa'
        table = self.block(2)
        table[:16] = ESP_TYPE
        table[16:32] = uuid.UUID(int=2).bytes_le
        struct.pack_into('<QQ', table, 32, ESP, ESP + 70000 - 1)
        self.block(SECTORS - 2)[:] = table
        for lba, other, array in [(1, SECTORS - 1, 2), (SECTORS - 1, 1, SECTORS - 2)]:
            hdr = self.block(lba)
            hdr[:8] = b'EFI PART'
            struct.pack_into('<II', hdr, 8, 0x10000, 92)
            struct.pack_into('<QQQQ', hdr, 24, lba, other, 34, SECTORS - 34)
            hdr[56:72] = uuid.UUID(int=1).bytes_le
            struct.pack_into('<QIII', hdr, 72, array, 4, 128, zlib.crc32(table))
            struct.pack_into('<I', hdr, 16, zlib.crc32(hdr[:92]))
        bpb = self.block(ESP)
        bpb[0] = 0xeb
        struct.pack_into('<HBHB', bpb, 11, 512, 1, 32, 2)
        bpb[21] = 0xf8
        struct.pack_into('<IIIIHH', bpb, 32, 70000, FAT_SIZE, 0, 2, 1, 6)
        bpb[82:90] = b'FAT32   '
        bpb[-2:] = b'\x55\xaa'
        self.block(ESP + 6)[:] = bpb
        info = self.block(ESP + 1)
        info[:4] = b'RRaA'
        info[484:488] = b'rrAa'
        info[-4:] = b'\x00\x00\x55\xaa'
        for lba in (FAT, FAT + FAT_SIZE):
            struct.pack_into('<IIII', self.block(lba), 0, 0x0ffffff8, 0x0fffffff,
                             0x0fffffff, 0x0fffffff)
        self.block(ROOT)[:32] = entry(b'RECOVERY   ', 0x10, cluster=3)
        self.block(RECOVERY)[:32] = entry(b'.          ', 0x10, cluster=3)

    def block(self, lba):
        return self.blocks.setdefault(lba, bytearray(512))

    @contextlib.contextmanager
    def session_lock(self, session):
        assert session == 'original'
        yield {'guest_boot_id': self.boot}

    def guest(self, data, command):
        assert command == 'bridge hardware ahci-ro'
        return dict(ahci_ro_api=1, requested=True, ready=True, quarantined=False,
                    sectors=SECTORS, logical_sector_bytes=512, reason='configured ATA device writable')

    def guest_batch(self, data, commands):
        assert len(commands) <= 8
        replies = []
        for command in commands:
            prefix, lba = command.rsplit(' ', 1)
            assert prefix == 'bridge hardware ahci-sector'
            lba = int(lba)
            self.requested.append(lba)
            replies.append(dict(ahci_ro_api=1, lba=lba + self.wrong_lba, hex=self.blocks[lba].hex()))
        return replies


class Tests(unittest.TestCase):
    def setUp(self):
        self.disk = Disk()

    def inspect(self):
        return inspect_bootstat(self.disk, 'original', 'a' * 32)

    def add(self, name=b'BOOTSTATBIN', attr=0x20, cluster=0, length=0):
        self.disk.block(RECOVERY)[32:64] = entry(name, attr, cluster, length)

    def test_absence_is_derived_from_only_esp_metadata(self):
        result = self.inspect()
        self.assertEqual(result['state'], 'absent')
        self.assertEqual(result['esp_first_lba'], ESP)
        self.assertEqual(set(self.disk.requested), set(self.disk.blocks))

    def test_empty_file(self):
        self.add()
        self.assertEqual(self.inspect()['state'], 'empty')

    def test_nonempty_even_if_preallocated_as_zero(self):
        self.add(cluster=4, length=1024)
        with self.assertRaisesRegex(RuntimeError, 'not an empty ordinary file'):
            self.inspect()

    def test_zero_length_entry_with_allocated_chain_is_refused(self):
        self.add(cluster=4)
        with self.assertRaisesRegex(RuntimeError, 'not an empty ordinary file'):
            self.inspect()

    def test_directory_and_duplicate_journal_are_refused(self):
        self.add(attr=0x10)
        with self.assertRaisesRegex(RuntimeError, 'not an empty ordinary file'):
            self.inspect()
        self.add()
        self.disk.block(RECOVERY)[64:96] = entry(b'BOOTSTATBIN', 0x20)
        with self.assertRaisesRegex(RuntimeError, 'ambiguous BOOTSTAT'):
            self.inspect()

    def test_lfn_alias_cannot_hide_a_nonempty_journal(self):
        short = b'BOOTST~1BIN'
        self.add(name=short, cluster=4, length=1024)
        directory = self.disk.block(RECOVERY)
        directory[64:96] = directory[32:64]
        long = b'\x00'.join(bytes([char]) for char in b'BOOTSTAT.BIN') + b'\x00\x00\x00'
        checksum = 0
        for byte in short:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xff
        lfn = bytearray(32)
        lfn[0] = 0x41
        lfn[11] = 0x0f
        lfn[13] = checksum
        lfn[1:11], lfn[14:26], lfn[28:32] = long[:10], long[10:22], long[22:]
        directory[32:64] = lfn
        with self.assertRaisesRegex(RuntimeError, 'not an empty ordinary file'):
            self.inspect()

    def test_stale_session_and_wrong_lba_are_refused(self):
        self.disk.boot = 'b' * 32
        with self.assertRaisesRegex(RuntimeError, 'stale bridge session'):
            self.inspect()
        self.disk.boot = 'a' * 32
        self.disk.wrong_lba = True
        with self.assertRaisesRegex(RuntimeError, 'invalid AHCI sector reply'):
            self.inspect()

    def test_archived_recovery_directory_is_accepted(self):
        # FAT writers set the archive bit on directories (the Dell's RECOVERY is 0x30).
        self.disk.block(ROOT)[11] = 0x30
        self.assertEqual(self.inspect()['state'], 'absent')

    def test_recovery_volume_label_or_file_is_refused(self):
        for attr in (0x08, 0x20, 0x18):
            self.disk.block(ROOT)[11] = attr
            with self.assertRaisesRegex(RuntimeError, 'RECOVERY directory unproven'):
                self.inspect()

    def test_missing_recovery_directory_is_ambiguous(self):
        self.disk.block(ROOT)[0] = 0xe5
        with self.assertRaisesRegex(RuntimeError, 'RECOVERY directory unproven'):
            self.inspect()

    def test_disagreeing_table_and_fat_are_refused(self):
        self.disk.block(SECTORS - 2)[35] ^= 1
        with self.assertRaisesRegex(RuntimeError, 'GPT entry mirrors/checksum'):
            self.inspect()
        self.disk.block(SECTORS - 2)[35] ^= 1
        self.disk.block(FAT)[12] = 4
        with self.assertRaisesRegex(RuntimeError, 'directory chain mirrors disagree'):
            self.inspect()

    def test_cyclic_or_free_recovery_cluster_is_refused(self):
        for following in (0, 3, 2):
            for lba in (FAT, FAT + FAT_SIZE):
                struct.pack_into('<I', self.disk.block(lba), 12, following)
            with self.subTest(following=following), self.assertRaisesRegex(RuntimeError, 'invalid or cyclic'):
                self.inspect()


if __name__ == '__main__':
    unittest.main()
