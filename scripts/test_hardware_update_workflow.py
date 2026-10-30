#!/usr/bin/env python3
"""State/receipt regression tests; no network, VM or physical device."""
import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_workflow import UpdateRun

class Clock:
    def __init__(self):self.now=0
    def time(self):return self.now
    def sleep(self,n):self.now+=n

class Bridge:
    def __init__(self):
        self.calls=[];self.tx=None;self.newboot=False;self.activated=False;self.job_polls=0
        self.reset=True;self.stale=False;self.boot_changes=True;self.payload='candidate';self.mismatch=False;self.missing_commit=False
    def load_session(self,session):return {'guest_boot_id':'a'*32,'host':'192.0.2.1','port':1234}
    def doctor(self,session):return {'reachable':True,'identity_matches':not (self.stale or self.newboot),
        'capabilities':{'power':{'reset':self.reset}}}
    def guest_run(self,session,ident,argv,**kwargs):
        self.calls.append(('run',session,ident,argv))
        if session=='original':self.tx=ident
        return {'state':'queued'}
    def job(self,session,ident,**kwargs):
        self.calls.append(('job',session,ident))
        if session=='fresh':return {'state':'completed','exit':{'code':0},'stdout':{'text':'HARDWARE_PAYLOAD_ID '+self.payload+'\n'}}
        self.job_polls+=1
        if self.job_polls==1:return {'state':'running','stdout':{'text':'HWUPDATE_LISTENING port=1235'}}
        code=0 if self.activated else 22
        generation=9 if self.mismatch else 2
        return {'state':'completed','exit':{'code':code},'stdout':{'text':
            f'HWUPDATE_ACTIVATED generation={generation} slot=1 transaction={self.tx} reboot_required=1\n' if self.activated else ''}}
    def power(self,session,ident,operation):
        self.calls.append(('power',session,ident,operation))
        if operation=='restart':return {'state':'closing'}
        if operation=='status':return {'state':'ready','error':0}
        assert operation=='commit'
        self.newboot=self.boot_changes
        if self.missing_commit:raise ConnectionError('Reset dropped reply')
        return {'state':'ready','error':0}
    def attach(self,host,port):
        self.calls.append(('attach',host,port));return {'session':'fresh','guest_boot_id':'b'*32}

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.key=self.root/'key';self.key.write_text('1'*32)
        self.kernel=self.root/'kernel';self.kernel.write_bytes(bytes(64))
        self.initrd=self.root/'initrd';self.initrd.write_bytes(bytes(512))
        self.bridge=Bridge();self.clock=Clock();self.output=self.root/'run';self.lose_upload=False
    def tearDown(self):self.tmp.cleanup()
    def uploader(self,host,port,key,boot,tx,kernel,initrd,activate):
        self.assertEqual((boot,tx,key),('a'*32,self.bridge.tx,'1'*32));self.bridge.activated=activate
        if self.lose_upload:raise ConnectionError('Lost activation reply')
        return {'state':'activated' if activate else 'staged','generation':2,'slot':1}
    def execute(self,activate=True,restart=False):
        return UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,activate,restart,'candidate' if restart else None)
    def receipt(self):return json.loads((self.output/'receipt.json').read_text())
    def test_activation_without_reboot(self):
        result=self.execute();self.assertEqual(result['state'],'activated')
        self.assertFalse(any(call[0]=='power' for call in self.bridge.calls))
    def test_staged_only_does_not_restart(self):
        self.assertEqual(self.execute(False)['state'],'staged_only')
        self.assertFalse(self.receipt()['storage_modified'])
    def test_complete_restart_verifies_epoch_and_payload(self):
        result=self.execute(restart=True);self.assertEqual(result['state'],'boot_verified')
        calls=[x for x in self.bridge.calls if x[0]=='power']
        self.assertEqual([x[3] for x in calls],['restart','status','commit'])
        self.assertEqual(len({x[2] for x in calls}),1)
        self.assertNotEqual(result['guest_boot_id'],result['new_guest_boot_id'])
    def test_lost_commit_reply_is_observed_without_replay(self):
        self.bridge.missing_commit=True;self.assertEqual(self.execute(restart=True)['state'],'boot_verified')
        self.assertEqual(sum(x[0]=='power' and x[3]=='commit' for x in self.bridge.calls),1)
    def test_no_boot_change_is_not_success(self):
        self.bridge.boot_changes=False
        with self.assertRaises(TimeoutError):self.execute(restart=True)
        self.assertEqual(self.receipt()['state'],'needs_inspection')
        self.assertEqual(sum(x[0]=='power' and x[3]=='commit' for x in self.bridge.calls),1)
    def test_wrong_new_payload_is_not_success(self):
        self.bridge.payload='recovery'
        with self.assertRaises(RuntimeError):self.execute(restart=True)
        self.assertEqual(self.receipt()['last_stage'],'new_boot_observed')
    def test_lost_upload_reply_never_restarts(self):
        self.lose_upload=True
        with self.assertRaises(ConnectionError):self.execute(restart=True)
        self.assertFalse(any(x[0]=='power' for x in self.bridge.calls))
        self.assertEqual(self.receipt()['job_observation']['exit']['code'],0)
        self.assertEqual(sum(x[0]=='run' for x in self.bridge.calls),1)
    def test_mismatched_job_receipt_never_restarts(self):
        self.bridge.mismatch=True
        with self.assertRaises(RuntimeError):self.execute(restart=True)
        self.assertFalse(any(x[0]=='power' for x in self.bridge.calls))
    def test_stale_boot_fails_before_receiver(self):
        self.bridge.stale=True
        with self.assertRaises(RuntimeError):self.execute()
        self.assertEqual(self.bridge.calls,[]);self.assertFalse(self.output.exists())
    def test_unsupported_reset_fails_before_receiver(self):
        self.bridge.reset=False
        with self.assertRaises(RuntimeError):self.execute(restart=True)
        self.assertEqual(self.bridge.calls,[])
    def test_existing_receipt_is_not_reused(self):
        self.execute();self.bridge.calls.clear()
        with self.assertRaises(FileExistsError):self.execute()
        self.assertEqual(self.bridge.calls,[])
    def test_invalid_key_never_launches(self):
        self.key.write_text('secret\n')
        with self.assertRaises(ValueError):self.execute()
        self.assertEqual(self.bridge.calls,[])

if __name__=='__main__':unittest.main()
