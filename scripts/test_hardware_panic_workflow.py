#!/usr/bin/env python3
"""Exercise panic-export receipt, transfer integrity and no-replay behavior."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from hardware_panic_workflow import PanicRun

class Bridge:
    def __init__(self):self.launches=[];self.payload=b'crash packet';self.mismatch=False;self.stale=False
    def load_session(self,session):return {'guest_boot_id':'a'*32,'host':'127.0.0.1'}
    def doctor(self,session):return {'probe_stage':'verified','reachable':True,'identity_matches':not self.stale,'guest':{'bulk_helper_api':1}}
    def guest_run(self,session,request,argv,**kw):self.launches.append((request,argv))
    def job(self,session,request,**kw):
        assert request==self.launches[0][0]
        crc=zlib.crc32(self.payload)
        return {'state':'completed','exit':{'code':0},'stdout':{'text':f'PANICLOG: sequence=3 bytes={len(self.payload)} crc32={crc:08x} output={request}\n'}}

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.bridge=Bridge()
        self.key=self.root/'key';self.key.write_text('c'*32)
    def transfer(self,run,session,key,action,**kw):
        self.assertEqual(action,'get');self.assertEqual(kw['artifact_id'],'b'*32)
        run.output.mkdir();artifact=run.output/'artifact.bin'
        artifact.write_bytes(b'wrong' if self.bridge.mismatch else self.bridge.payload)
        return {'artifact_path':str(artifact)}
    def execute(self):return PanicRun(self.bridge,self.root/'result').execute('session',self.key,'b'*32)
    def test_receipt_and_integrity(self):
        with patch('hardware_panic_workflow.BulkRun.execute',autospec=True,side_effect=self.transfer):r=self.execute()
        self.assertEqual(r['state'],'completed');self.assertEqual(r['sequence'],3)
        self.assertEqual(Path(r['artifact_path']).read_bytes(),self.bridge.payload)
        self.assertEqual(self.bridge.launches[0][1],['/boot/PANICLOG.ELF','--bridge','b'*32])
    def test_mismatch_retains_receipt_and_does_not_replay(self):
        self.bridge.mismatch=True
        with patch('hardware_panic_workflow.BulkRun.execute',autospec=True,side_effect=self.transfer):
            with self.assertRaisesRegex(RuntimeError,'does not match'):self.execute()
        receipt=json.loads((self.root/'result/receipt.json').read_text())
        self.assertEqual(receipt['state'],'needs_inspection');self.assertEqual(receipt['last_stage'],'transfer_dispatched')
        with self.assertRaises(FileExistsError):self.execute()
        self.assertEqual(len(self.bridge.launches),1)
    def test_stale_boot_prevents_launch(self):
        self.bridge.stale=True
        with self.assertRaises(RuntimeError):self.execute()
        self.assertFalse(self.bridge.launches)

if __name__=='__main__':unittest.main()
