"""Bounded, read-only BOOTSTAT inspection through the pre-slot Brook bridge."""

import re
import struct
import uuid
import zlib

SECTOR = 512
ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
MAX_DIRECTORY_SECTORS = 96


def check(condition, reason):
    if not condition:
        raise RuntimeError('Cannot verify first-install BOOTSTAT: ' + reason)


def gpt_header(raw, sectors, backup):
    check(raw[:8] == b'EFI PART' and struct.unpack_from('<II', raw, 8) == (0x10000, 92)
          and not any(raw[20:24] + raw[92:]), 'unsupported GPT header')
    checksum, = struct.unpack_from('<I', raw, 16)
    check(zlib.crc32(raw[:16] + bytes(4) + raw[20:92]) == checksum, 'GPT header checksum')
    current, other, first, last = struct.unpack_from('<QQQQ', raw, 24)
    check((current, other) == ((sectors - 1, 1) if backup else (1, sectors - 1))
          and 34 <= first <= last < sectors - 33 and any(raw[56:72]), 'GPT geometry')
    table, count, size, crc = struct.unpack_from('<QIII', raw, 72)
    check(1 <= count <= 128 and size == 128, 'unsupported GPT entry geometry')
    blocks = (count * size + SECTOR - 1) // SECTOR
    check(last < table and table + blocks <= sectors - 1 if backup else
          2 <= table and table + blocks <= first, 'GPT entries out of bounds')
    return first, last, raw[56:72], table, blocks, count, crc


def esp_partition(read, sectors):
    mbr, primary, backup = read([0, 1, sectors - 1])
    protective = [mbr[p:p + 16] for p in range(446, 510, 16)]
    active = [record for record in protective if any(record)]
    check(mbr[-2:] == b'\x55\xaa' and len(active) == 1 and active[0][0] == 0
          and active[0][4] == 0xee and struct.unpack_from('<II', active[0], 8)
          == (1, min(sectors - 1, 0xffffffff)), 'missing protective MBR')
    a, b = gpt_header(primary, sectors, False), gpt_header(backup, sectors, True)
    check(a[:3] == b[:3] and a[4:] == b[4:], 'GPT copies disagree')
    first, last, disk_guid, ptable, blocks, count, crc = a
    reads = read(list(range(ptable, ptable + blocks)) + list(range(b[3], b[3] + blocks)))
    table = b''.join(reads[:blocks])[:count * 128]
    check(table == b''.join(reads[blocks:])[:count * 128] and zlib.crc32(table) == crc, 'GPT entry mirrors/checksum')
    partitions = []
    for pos in range(0, len(table), 128):
        entry = table[pos:pos + 128]
        if not any(entry):
            continue
        begin, end = struct.unpack_from('<QQ', entry, 32)
        check(any(entry[:16]) and any(entry[16:32]) and first <= begin <= end <= last,
              'invalid GPT partition')
        partitions.append((begin, end, entry[:16], entry[16:32]))
    partitions.sort()
    check(all(a[1] < b[0] for a, b in zip(partitions, partitions[1:]))
          and len({p[3] for p in partitions}) == len(partitions), 'ambiguous GPT partition')
    esp = [p for p in partitions if p[2] == ESP_TYPE]
    check(len(esp) == 1, 'expected exactly one ESP')
    start, end, _, guid = esp[0]
    return start, end - start + 1, disk_guid, guid


def fat_geometry(read, start, sectors):
    raw, = read([start])
    reserved, = struct.unpack_from('<H', raw, 14)
    total, fat_size = struct.unpack_from('<II', raw, 32)
    flags, version, root, fsinfo, backup = struct.unpack_from('<HHIHH', raw, 40)
    spc = raw[13]
    check(raw[-2:] == b'\x55\xaa' and raw[11:13] == b'\x00\x02'
          and raw[0] in (0xeb, 0xe9) and raw[21] == 0xf8 and raw[82:90] == b'FAT32   '
          and spc in (1, 2, 4, 8, 16, 32, 64) and raw[16] == 2
          and raw[17:21] == bytes(4) and raw[22:24] == bytes(2)
          and flags == version == 0 and fsinfo == 1 and backup == 6 < reserved
          and total == sectors and 0 < fat_size and reserved + 2 * fat_size < total,
          'unsupported FAT32 geometry')
    clusters = (total - reserved - 2 * fat_size) // spc
    check(65526 <= clusters <= 0x0fffffee and 2 <= root < clusters + 2
          and fat_size * (SECTOR // 4) >= clusters + 2, 'invalid FAT32 geometry')
    mirror, info = read([start + backup, start + fsinfo])
    check(mirror == raw, 'FAT32 boot copies disagree')
    check(info[:4] == b'RRaA' and info[484:488] == b'rrAa' and info[508:] == b'\x00\x00\x55\xaa',
          'invalid FAT32 FSInfo')
    return start + reserved, fat_size, spc, clusters, root


def names(entry, pending):
    short = entry[:11].decode('ascii', errors='replace').upper()
    if not pending:
        return short, None
    # One to twenty LFN records precede their owning short entry.
    count = len(pending)
    checksum = 0
    for byte in entry[:11]:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xff
    check(count <= 20 and all(
        part[0] == ((count - i) | (0x40 if i == 0 else 0)) and part[12] == 0
        and part[13] == checksum and part[26:28] == b'\x00\x00'
        for i, part in enumerate(pending)), 'malformed FAT32 long name')
    units = b''.join(part[1:11] + part[14:26] + part[28:32] for part in reversed(pending))
    try:
        long = units.decode('utf-16le')
    except UnicodeDecodeError:
        check(False, 'invalid FAT32 long name')
    if '\x00' in long:
        long, padding = long.split('\x00', 1)
        check(all(c == '\uffff' for c in padding), 'malformed FAT32 long-name padding')
    check(bool(long) and '\uffff' not in long, 'malformed FAT32 long name')
    return short, long.rstrip(' .').upper()


def named_entries(read, geometry, start_cluster, short_name, long_name, forbid=()):
    fat_start, fat_size, spc, clusters, _ = geometry
    data_start = fat_start + 2 * fat_size
    checked, pending, found, visited = set(forbid), [], [], set()
    scanned = 0
    cluster = start_cluster
    while True:
        check(2 <= cluster < clusters + 2 and cluster not in checked, 'invalid or cyclic directory chain')
        checked.add(cluster)
        visited.add(cluster)
        offset = cluster * 4
        lba, offset = divmod(offset, SECTOR)
        a, b = read([fat_start + lba, fat_start + fat_size + lba])
        check(a == b, 'FAT32 directory chain mirrors disagree')
        next_cluster = struct.unpack_from('<I', a, offset)[0] & 0x0fffffff
        terminal = next_cluster >= 0x0ffffff8
        check(terminal or (2 <= next_cluster < clusters + 2 and next_cluster not in checked),
              'invalid or cyclic directory chain')
        data = data_start + (cluster - 2) * spc
        for sector in range(data, data + spc):
            check(scanned < MAX_DIRECTORY_SECTORS, 'directory exceeds read budget')
            scanned += 1
            raw, = read([sector])
            for off in range(0, SECTOR, 32):
                entry = raw[off:off + 32]
                if entry[0] == 0:
                    check(not pending, 'incomplete FAT32 long name')
                    return found, visited
                if entry[0] == 0xe5:
                    pending.clear()
                    continue
                if entry[11] == 0x0f:
                    pending.append(entry)
                    continue
                short, long = names(entry, pending)
                pending.clear()
                if short == short_name or long == long_name:
                    found.append(entry)
        cluster = next_cluster
        if terminal:
            check(not pending, 'incomplete FAT32 long name')
            return found, visited


def cluster_of(entry):
    return (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]


def inspect_sectors(read, sectors):
    start, size, disk_guid, esp_guid = esp_partition(read, sectors)
    geometry = fat_geometry(read, start, size)
    parents, root_chain = named_entries(read, geometry, geometry[-1], 'RECOVERY   ', 'RECOVERY')
    # A directory, not a volume label or device; the archive, read-only,
    # hidden and system bits are ignored (FAT writers set archive on
    # directories: the Dell's RECOVERY entry is 0x30).
    check(len(parents) == 1 and parents[0][11] & 0xd8 == 0x10, 'RECOVERY directory unproven')
    entries, _ = named_entries(read, geometry, cluster_of(parents[0]), 'BOOTSTATBIN', 'BOOTSTAT.BIN', root_chain)
    check(len(entries) <= 1, 'ambiguous BOOTSTAT pathname')
    if entries:
        entry = entries[0]
        check(entry[11] & 0xd8 == 0 and cluster_of(entry) == 0
              and entry[28:32] == bytes(4), 'BOOTSTAT is not an empty ordinary file')
    return {'state': 'empty' if entries else 'absent', 'esp_first_lba': start,
            'esp_sectors': size, 'disk_guid': str(uuid.UUID(bytes_le=disk_guid)),
            'esp_guid': str(uuid.UUID(bytes_le=esp_guid))}


def inspect_bootstat(bridge, session, boot):
    # Every command goes through Hardware.guest_batch's boot epoch guard.
    # Cache no disk data across passes and never read any file's contents.
    with bridge.session_lock(session) as data:
        check(data.get('guest_boot_id') == boot, 'stale bridge session')
        status = bridge.guest(data, 'bridge hardware ahci-ro')
        sectors = status.get('sectors')
        check(status.get('ahci_ro_api') == 1 and status.get('requested') is True
              and status.get('ready') is True and status.get('quarantined') is False
              and status.get('logical_sector_bytes') == SECTOR
              and type(sectors) is int and 68 <= sectors < 2**64
              and status.get('reason') == 'configured ATA device writable',
              'boot-admitted writable AHCI device is unavailable')

        def read(lbas):
            check(all(0 <= lba < sectors for lba in lbas), 'sector outside admitted AHCI device')
            result = []
            for pos in range(0, len(lbas), 8):
                batch = lbas[pos:pos + 8]
                replies = bridge.guest_batch(data, [f'bridge hardware ahci-sector {lba}' for lba in batch])
                check(len(replies) == len(batch), 'incomplete AHCI sector reply')
                for lba, reply in zip(batch, replies):
                    raw = reply.get('hex')
                    check(reply.get('ahci_ro_api') == 1 and reply.get('lba') == lba
                          and isinstance(raw, str) and re.fullmatch('[0-9a-f]{1024}', raw),
                          'invalid AHCI sector reply')
                    result.append(bytes.fromhex(raw))
            return result

        return inspect_sectors(read, sectors)
