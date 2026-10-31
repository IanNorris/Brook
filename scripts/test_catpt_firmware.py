#!/usr/bin/env python3
"""Boundaries and corruption checks for the offline firmware inventory."""
import os,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from catpt_firmware import inspect

def sample():
    block=struct.pack('<4I',1,4,0,0)+b'1234'
    module=struct.pack('<4sIIHHIII',b'$SST',len(block),1,0,0,0,0,0)+block
    return bytearray(struct.pack('<4s7I',b'$SST',len(module),1,254,0,0,0,0)+module)

class TestFirmware(unittest.TestCase):
    def test_valid(self):
        info=inspect(sample());self.assertEqual(info['modules'][0]['blocks'][0]['host_relative_offset'],0xa0000)
    def test_reject_mutations(self):
        cases=[]
        for off,val in ((4,0xffffffff),(8,0xffffffff),(12,0),(36,0xffffffff),(40,0xffffffff),(60,0xffffffff),(64,0xffffffff),(68,0xffffffff)):
            image=sample();struct.pack_into('<I',image,off,val);cases.append(image)
        cases.extend((sample()[:n] for n in (0,1,31,32,59,60,75,79)))
        for image in cases:
            with self.subTest(length=len(image),header=bytes(image[:16])):
                with self.assertRaises(ValueError):inspect(image)
    def test_overlapping_ranges(self):
        block=struct.pack('<4I',1,4,0,0)+b'1234';module=struct.pack('<4sIIHHIII',b'$SST',len(block)*2,2,0,0,0,0,0)+block*2
        image=struct.pack('<4s7I',b'$SST',len(module),1,254,0,0,0,0)+module
        with self.assertRaisesRegex(ValueError,'overlapping'):inspect(image)
    def test_ram_boundaries_and_adjacent_blocks(self):
        def image(blocks):
            body=b''.join(struct.pack('<4I',kind,4,offset,0)+b'1234' for kind,offset in blocks)
            module=struct.pack('<4sIIHHIII',b'$SST',len(body),len(blocks),0,0,0,0,0)+body
            return struct.pack('<4s7I',b'$SST',len(module),1,254,0,0,0,0)+module
        inspect(image([(1,0x4fffc),(2,0x9fffc)]))
        inspect(image([(2,0),(3,4)]))
        for blocks in ([(1,0x4fffd)],[(2,0x9fffd)],[(2,0),(3,0)]):
            with self.assertRaises(ValueError):inspect(image(blocks))
    def test_invalid_and_duplicate_module_ids(self):
        image=sample();struct.pack_into('<H',image,46,16)
        with self.assertRaisesRegex(ValueError,'module ID'):inspect(image)
    def test_cross_module_overlap(self):
        first=bytes(sample()[32:]);second=bytearray(first);struct.pack_into('<H',second,14,1)
        payload=first+bytes(second);image=struct.pack('<4s7I',b'$SST',len(payload),2,254,0,0,0,0)+payload
        with self.assertRaisesRegex(ValueError,'overlapping'):inspect(image)
        module=bytes(sample()[32:]);image=struct.pack('<4s7I',b'$SST',len(module)*2,2,254,0,0,0,0)+module*2
        with self.assertRaisesRegex(ValueError,'module ID'):inspect(image)
    @unittest.skipUnless(os.environ.get('BROOK_CATPT_FIRMWARE'),'real firmware path not supplied')
    def test_real_broadwell_firmware(self):
        info=inspect(Path(os.environ['BROOK_CATPT_FIRMWARE']).read_bytes())
        self.assertEqual(info['sha256'],'d1ae2cb6e0121c608adefac9d46a8b26e248fa6c7c39a0e26c6ad1e6c822932a')
        self.assertEqual(info['module_count'],8);self.assertEqual(sum(len(m['blocks']) for m in info['modules']),15)
if __name__=='__main__':unittest.main()
