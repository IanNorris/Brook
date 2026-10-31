#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
import pack_ssd_install_source as packer


def fixture(path):
    sectors, first, count = 4096, 2048, 129
    image = bytearray(sectors * 512)
    image[510:512] = b'\x55\xaa'
    entry = bytearray(16); entry[4] = 0xee; struct.pack_into('<II', entry, 8, 1, sectors - 1)
    image[446:462] = entry
    entries = bytearray(16384); entries[:16] = packer.ESP; entries[16:32] = bytes(range(1, 17))
    struct.pack_into('<QQ', entries, 32, first, first + count - 1)
    crc = zlib.crc32(entries)
    def header(current, alternate, table):
        h = bytearray(512); h[:8] = b'EFI PART'; struct.pack_into('<IIII', h, 8, 0x10000, 92, 0, 0)
        struct.pack_into('<QQQQ', h, 24, current, alternate, 34, sectors - 34)
        h[56:72] = bytes(range(16, 32)); struct.pack_into('<QIII', h, 72, table, 128, 128, crc)
        struct.pack_into('<I', h, 16, zlib.crc32(h[:92])); return h
    image[512:1024] = header(1, sectors - 1, 2); image[1024:17408] = entries
    image[(sectors - 33)*512:(sectors - 1)*512] = entries; image[-512:] = header(sectors - 1, 1, sectors - 33)
    for i in range(first * 512, (first + count) * 512): image[i] = (i * 17) & 255
    path.write_bytes(image)
    return image, sectors, first, count


class CompactPackage(unittest.TestCase):
    def test_pack_and_detect_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root/'source'; image, sectors, first, count = fixture(source)
            package = root/'package'; manifest = packer.pack(source, package, 32768)
            self.assertEqual(packer.verify_package(package), manifest)
            actual = b''.join((package / item['name']).read_bytes() for item in manifest['segments'])
            expected = image[:34*512] + image[first*512:(first+count)*512] + image[(sectors-33)*512:]
            self.assertEqual(actual, expected)
            self.assertEqual(hashlib.sha256(actual).hexdigest(), manifest['packed_sha256'])
            record=(package/'INSTHASH.BIN').read_bytes()
            self.assertEqual(hashlib.sha256(record[:40]+actual).digest(),record[40:])
            self.assertEqual(hashlib.sha256(record).hexdigest(),manifest['install_record_sha256'])
            segment = package/manifest['segments'][0]['name']; original = segment.read_bytes(); data = bytearray(original); data[0] ^= 1; segment.write_bytes(data)
            with self.assertRaises(ValueError): packer.verify_segment(segment,manifest['segments'][0])
            with self.assertRaises(ValueError): packer.verify_package(package)
            segment.write_bytes(original)
            record_path=package/'INSTHASH.BIN';tampered=bytearray(record);tampered[8]^=1;record_path.write_bytes(tampered)
            with self.assertRaises(ValueError): packer.verify_package(package)
            record_path.write_bytes(record)
            doc=json.loads((package/'manifest.json').read_text());doc['geometry']['payload_first']+=1
            (package/'manifest.json').write_text(json.dumps(doc))
            with self.assertRaises(ValueError): packer.verify_package(package)
    def test_invalid_source_leaves_no_complete_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'source';fixture(source)
            data=bytearray(source.read_bytes());data[1024] ^= 1;source.write_bytes(data)
            with self.assertRaises(ValueError): packer.pack(source,root/'bad',32768)
            self.assertFalse((root/'bad'/'manifest.json').exists())
            with self.assertRaises(ValueError): packer.pack(root,root/'bad2',32768)


if __name__ == '__main__': unittest.main()
