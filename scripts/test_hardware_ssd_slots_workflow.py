#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from hardware_ssd_slots_workflow import SlotRun

PATHS = ('/media/system/EFI/BOOT/BOOTX64.EFI', '/media/system/KERNEL/BROOK.ELF', '/media/system/INITRD.IMG')
HASHES = {p: str(i) * 64 for i, p in enumerate(PATHS, 1)}
IDENTITY = dict(port=3, sectors=500118192, logical_sector_bytes=512, physical_sector_bytes=512, model='TEST', serial='TEST')


class Bridge:
    def __init__(self): self.calls=[]; self.args={}; self.fail_stage=None; self.stale=False; self.ahci={'ready':True,'quarantined':False,**IDENTITY}
    def load_session(self, session): return {'guest_boot_id':'a'*32}
    def doctor(self, session): return {'reachable':True,'identity_matches':not self.stale,'guest':{'writable_desktop':{'ready':True,'source_pinned':True,'state_uncertain':False}}}
    def command(self, session, command): return self.ahci
    def guest_run(self, session, ident, argv, **kwargs):
        self.calls.append(argv); self.args[ident]=argv
        if argv[0]=='/boot/SSDSLOT.ELF' and self.fail_stage=='dispatch': raise ConnectionError('unknown delivery')
    def job(self, session, ident, **kwargs):
        argv=self.args[ident]
        if argv[0]=='/boot/HWSTATUS.ELF': text='HARDWARE_PAYLOAD_ID candidate\n'
        elif argv[0]=='/nix/profile/bin/sha256sum': text=''.join(HASHES[p]+'  '+p+'\n' for p in PATHS)
        else: text='SSD_SLOT_SETUP_OK generation=2\n'
        if self.fail_stage=='hash' and argv[0]=='/nix/profile/bin/sha256sum': text='mismatch\n'
        return {'state':'completed','exit':{'code':0},'stdout':{'text':text}}


class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.out=Path(self.tmp.name)/'run'; self.b=Bridge()
    def tearDown(self): self.tmp.cleanup()
    def test_success_dispatches_once(self):
        result=SlotRun(self.b,self.out).execute('s',IDENTITY,'candidate',HASHES)
        self.assertEqual(result['state'],'slots_initialized')
        self.assertEqual(sum(x[0]=='/boot/SSDSLOT.ELF' for x in self.b.calls),1)
    def test_bad_hash_never_mutates(self):
        self.b.fail_stage='hash'
        with self.assertRaises(RuntimeError): SlotRun(self.b,self.out).execute('s',IDENTITY,'candidate',HASHES)
        self.assertFalse(any(x[0]=='/boot/SSDSLOT.ELF' for x in self.b.calls))
        self.assertEqual(json.loads((self.out/'receipt.json').read_text())['state'],'needs_inspection')
    def test_lost_mutation_reply_is_not_retried(self):
        self.b.fail_stage='dispatch'
        with self.assertRaises(ConnectionError): SlotRun(self.b,self.out).execute('s',IDENTITY,'candidate',HASHES)
        receipt=json.loads((self.out/'receipt.json').read_text())
        self.assertEqual(receipt['last_stage'],'mutation_dispatched')
        self.assertEqual(sum(x[0]=='/boot/SSDSLOT.ELF' for x in self.b.calls),1)
    def test_wrong_identity_fails_before_any_job(self):
        with self.assertRaises(RuntimeError): SlotRun(self.b,self.out).execute('s',{**IDENTITY,'port':4},'candidate',HASHES)
        self.assertFalse(self.b.calls)
    def test_stale_boot_fails_before_any_job(self):
        self.b.stale=True
        with self.assertRaises(RuntimeError): SlotRun(self.b,self.out).execute('s',IDENTITY,'candidate',HASHES)
        self.assertFalse(self.b.calls)
    def test_malformed_disk_state_is_rejected(self):
        for change in ({'ready':'false'}, {'quarantined':None}, {'port':True}):
            self.b.ahci.update(change)
            with self.assertRaises(RuntimeError): SlotRun(self.b,self.out).execute('s',IDENTITY,'candidate',HASHES)
            self.assertFalse(self.b.calls)
            self.b.ahci={'ready':True,'quarantined':False,**IDENTITY}
    def test_existing_receipt_directory_is_not_reused(self):
        self.out.mkdir()
        with self.assertRaises(FileExistsError): SlotRun(self.b,self.out).execute('s',IDENTITY,'candidate',HASHES)
        self.assertFalse(self.b.calls)


if __name__=='__main__': unittest.main()
