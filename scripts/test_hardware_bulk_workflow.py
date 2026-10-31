#!/usr/bin/env python3
"""Recovery semantics: a lost reply must never cause a second guest launch."""
import hashlib,json,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from hardware_bulk_workflow import BulkRun
class Bridge:
    def __init__(self):self.launches=[];self.done=False;self.result={};self.matches=True;self.bulk=1;self.capture=1
    def load_session(self,session):return {'guest_boot_id':'a'*32,'host':'127.0.0.1'}
    def doctor(self,session):return {'reachable':True,'identity_matches':self.matches,'guest':{'bulk_helper_api':self.bulk,'completed_frame_copy_api':self.capture}}
    def guest_run(self,session,request,argv,**kwargs):self.launches.append((request,argv))
    def job(self,session,request,**kwargs):
        assert request==self.launches[0][0]
        if self.done:return {'state':'completed','exit':{'code':0},'stdout':{'text':'HWBULK_COMPLETED '+json.dumps(self.result)+'\n'}}
        return {'state':'running','stdout':{'text':'HWBULK_LISTENING bulk_api=1\n'}}
class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.source=self.root/'source';self.source.write_bytes(b'abc');self.key=self.root/'key';self.key.write_text('b'*32);self.bridge=Bridge()
    def upload(self,host,port,key,boot,tx,source):
        self.bridge.result={'state':'completed','transaction_id':tx,'action':'put','bytes':3,'sha256':hashlib.sha256(b'abc').hexdigest(),'guest_path':'/data/.brook-transfer/'+tx+'.bin'}
        self.bridge.done=True;return self.bridge.result
    def run_put(self,out='result'):
        return BulkRun(self.bridge,self.root/out).execute('session',self.key,'put',self.source,request_id='c'*32)
    def test_success_receipts_are_small_metadata(self):
        with patch('hardware_bulk_workflow.upload',self.upload):r=self.run_put()
        self.assertEqual(r['state'],'completed');self.assertEqual(len(self.bridge.launches),1)
        self.assertLess(len(json.dumps(r)),4096)
        self.assertNotIn('base64',json.dumps(r))
    def test_lost_final_reply_observes_same_job_never_redispatches(self):
        def lost(*args):self.upload(*args);raise ConnectionError('lost final reply')
        with patch('hardware_bulk_workflow.upload',lost),self.assertRaises(ConnectionError):self.run_put()
        r=json.loads((self.root/'result/receipt.json').read_text())
        self.assertEqual(r['state'],'needs_inspection');self.assertEqual(r['job_observation']['state'],'completed')
        self.assertEqual(len(self.bridge.launches),1)
        with self.assertRaises(FileExistsError):self.run_put()
        self.assertEqual(len(self.bridge.launches),1)
    def test_guest_receipt_mismatch_rejected(self):
        def mismatch(*args):r=dict(self.upload(*args));self.bridge.result['sha256']='0'*64;return r
        with patch('hardware_bulk_workflow.upload',mismatch),self.assertRaisesRegex(RuntimeError,'receipt mismatch'):self.run_put()
        self.assertEqual(len(self.bridge.launches),1)
    def test_stale_boot_prevents_launch(self):
        self.bridge.matches=False
        with self.assertRaises(RuntimeError):self.run_put()
        self.assertEqual(self.bridge.launches,[])
    def test_absent_helper_prevents_launch(self):
        self.bridge.bulk=0
        with self.assertRaises(RuntimeError):self.run_put()
        self.assertEqual(self.bridge.launches,[])
    def test_absent_capture_prevents_launch(self):
        self.bridge.capture=0
        with self.assertRaises(RuntimeError):BulkRun(self.bridge,self.root/'result').execute('session',self.key,'frame')
        self.assertEqual(self.bridge.launches,[])
    def test_disk_published_sync_failure_preserves_guest_receipt(self):
        identity={'sectors':1,'logical_sector_bytes':512,'physical_sector_bytes':512,'port':0,'model':'test','serial':'test'}
        def fail(host,port,key,boot,tx,path,expected,max_bytes,event):
            event('guest_receipt',{'state':'completed','transaction_id':tx,'sha256':'1'*64})
            event('published',{'artifact_path':str(path)})
            event('published_durability_uncertain',{'artifact_path':str(path)})
            raise OSError('directory sync failed')
        with patch('hardware_bulk_workflow.disk_download',fail),self.assertRaises(OSError):
            BulkRun(self.bridge,self.root/'result').execute('session',self.key,'disk',disk_identity=identity,max_bytes=512)
        receipt=json.loads((self.root/'result/receipt.json').read_text())
        self.assertEqual(receipt['state'],'published_needs_inspection')
        self.assertEqual(receipt['disk_guest_receipt']['sha256'],'1'*64)
        self.assertEqual(receipt['last_stage'],'disk_published_durability_uncertain')
        self.assertEqual(len(self.bridge.launches),1)
    def test_oversize_source_prevents_launch(self):
        with self.source.open('wb') as f:f.truncate(1024**3+1)
        with self.assertRaises(ValueError):self.run_put()
        self.assertEqual(self.bridge.launches,[])
    def frame_download(self,host,port,key,boot,tx,path,action):
        raw=bytes([0,0,255,255,255,0,0,255]);Path(path).write_bytes(raw)
        self.bridge.result={'state':'completed','transaction_id':tx,'action':'frame','bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'guest_path':None}
        self.bridge.done=True
        return {**self.bridge.result,'artifact_path':str(path),'payload':{'width':2,'height':1,'format':1}}
    def test_frame_defaults_to_verified_png_without_raw_disk_copy(self):
        with patch('hardware_bulk_workflow.download',self.frame_download):
            r=BulkRun(self.bridge,self.root/'frame').execute('session',self.key,'frame')
        self.assertEqual(r['state'],'completed');self.assertTrue(Path(r['artifact_path']).is_file())
        self.assertFalse((self.root/'frame/artifact.bin').exists());self.assertIsNone(r['transfer']['raw_artifact_path'])
        self.assertEqual(hashlib.sha256(Path(r['artifact_path']).read_bytes()).hexdigest(),r['artifact_sha256'])
    def test_frame_raw_is_explicit_opt_in(self):
        with patch('hardware_bulk_workflow.download',self.frame_download):
            r=BulkRun(self.bridge,self.root/'frame').execute('session',self.key,'frame',keep_raw=True)
        self.assertTrue(Path(r['transfer']['raw_artifact_path']).is_file())
if __name__=='__main__':unittest.main()
