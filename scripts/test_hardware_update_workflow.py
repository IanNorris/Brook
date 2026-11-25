#!/usr/bin/env python3
"""State/receipt regression tests; no network, VM or physical device."""
import json,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_workflow import RestartRejected, UpdateRun, wait_restart
from hardware_update_protocol import upload

class Clock:
    def __init__(self):self.now=0
    def time(self):return self.now
    def sleep(self,n):self.now+=n

class Bridge:
    def __init__(self):
        self.calls=[];self.tx=None;self.newboot=False;self.activated=False;self.job_polls=0
        self.reset=True;self.stale=False;self.boot_changes=True;self.payload='candidate';self.mismatch=False;self.missing_commit=False
        self.blocked=None
        self.cap_job=None;self.cap_legacy=False;self.cap_fault=None;self.launch_code=None
        self.guard_caps={'boot_id':'a'*32,'ac_shutdown_record':None,
                         'writable_desktop':{'ready':True,'source_pinned':True}}
    def load_session(self,session):return {'auth_mode':'legacy','guest_boot_id':'a'*32,'host':'192.0.2.1','port':1234}
    def doctor(self,session):return {'probe_stage':'verified','reachable':True,'identity_matches':not (self.stale or self.newboot),
        'capabilities':{'power':{'reset':self.reset}},'guest':self.guard_caps}
    def guest_run(self,session,ident,argv,**kwargs):
        self.calls.append(('run',session,ident,argv))
        if argv==['/boot/HWUPSYS.ELF','--caps']:self.cap_job=ident
        elif session=='original':self.tx=ident
        return {'state':'queued'}
    def job(self,session,ident,**kwargs):
        self.calls.append(('job',session,ident))
        if session=='fresh':return {'state':'completed','exit':{'code':0},'stdout':{'text':'HARDWARE_PAYLOAD_ID '+self.payload+'\n'}}
        if ident==self.cap_job:
            stream=lambda text:{'text':text,'eof':True,'gaps':[],'read_error':None}
            result={'session':session,'guest_boot_id':'a'*32,'operation_id':ident,'state':'completed',
                    'exit':{'code':2 if self.cap_legacy else 0},
                    'stdout':stream('' if self.cap_legacy else 'HWUPDATE_CAPS slot_aware=1\n'),
                    'stderr':stream('usage: HWUPDATE.ELF BOOT_ID32 TRANSACTION_ID32\n' if self.cap_legacy else '')}
            if self.cap_fault=='stale':result['guest_boot_id']='b'*32
            if self.cap_fault=='operation':result['operation_id']='c'*32
            if self.cap_fault=='unsupported':result['stdout']['text']='HWUPDATE_CAPS slot_aware=2\n'
            if self.cap_fault=='partial':result['stderr']['eof']=False
            if self.cap_fault=='slot_usage':
                result['exit']['code']=2
                result['stdout']['text']=''
                result['stderr']['text']='usage: HWUPDATE.ELF BOOT_ID32 TRANSACTION_ID32 [--require-slot-aware]\n'
            if self.cap_fault=='unknown_old':result['stderr']['text']='usage: unknown receiver\n'
            return result
        if self.launch_code is not None:return {'state':'completed','exit':{'code':self.launch_code}}
        self.job_polls+=1
        if self.job_polls==1:return {'state':'running','session':session,'operation_id':ident,'guest_boot_id':'a'*32,
                                      'stdout':{'text':'HWUPDATE_LISTENING port=1235'}}
        code=0 if self.activated else 22
        generation=9 if self.mismatch else 2
        return {'state':'completed','exit':{'code':code},'stdout':{'text':
            f'HWUPDATE_ACTIVATED generation={generation} slot=1 transaction={self.tx} reboot_required=1\n' if self.activated else ''}}
    def power(self,session,ident,operation):
        self.calls.append(('power',session,ident,operation))
        if operation=='restart':return {'state':'closing'}
        if operation=='status':return dict(self.blocked) if self.blocked else {'state':'ready','error':0}
        assert operation=='commit'
        self.newboot=self.boot_changes
        if self.missing_commit:raise ConnectionError('Reset dropped reply')
        return {'state':'ready','error':0}
    def attach(self,host,port,*,auth_mode):
        assert auth_mode=='legacy'
        self.calls.append(('attach',host,port));return {'session':'fresh','guest_boot_id':'b'*32}

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.key=self.root/'key';self.key.write_text('1'*32)
        self.kernel=self.root/'kernel';self.kernel.write_bytes(bytes(64))
        self.initrd=self.root/'initrd';self.initrd.write_bytes(bytes(512))
        self.bridge=Bridge();self.clock=Clock();self.output=self.root/'run';self.lose_upload=False
        self.bridge.registry=self.root/'registry';self.bridge.registry.mkdir()
        bootstat={'state':'absent','esp_guid':'fixture','esp_first_lba':2048}
        probe=patch('hardware_update_workflow.inspect_bootstat',return_value=bootstat)
        self.bootstat=probe.start()
        self.addCleanup(probe.stop)
    def tearDown(self):self.tmp.cleanup()
    def uploader(self,host,port,key,boot,tx,kernel,initrd,activate):
        self.assertEqual((boot,tx,key),('a'*32,self.bridge.tx,'1'*32));self.bridge.activated=activate
        if self.lose_upload:raise ConnectionError('Lost activation reply')
        return {'state':'activated' if activate else 'staged','generation':2,'slot':1}
    def execute(self,activate=True,restart=False):
        return UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,activate,restart,'candidate' if restart else None)
    def receipt(self):return json.loads((self.output/'receipt.json').read_text())
    def test_prompt_rejection_stops_before_commit(self):
        self.bridge.blocked={'state':'rejected','prompt_state':'rejected'}
        with self.assertRaisesRegex(RestartRejected,'remains pending'):
            wait_restart(self.bridge,'session','original',{'state':'prompting','prompt_seconds':60},
                         sleep=lambda _:None)
        self.assertEqual(self.bridge.calls,[('power','session','original','status')])

    def test_prompt_timeout_leaves_commit_to_guest(self):
        self.bridge.blocked={'state':'ready','prompt_state':'timeout','restart_automatic':True}
        self.assertTrue(wait_restart(self.bridge,'session','original',
                                     {'state':'prompting','prompt_seconds':60},sleep=lambda _:None))
        self.assertEqual(self.bridge.calls,[('power','session','original','status')])

    def test_approved_restart_can_drop_its_last_reply(self):
        approved={'state':'closing','prompt_state':'accepted','restart_automatic':True}
        with patch.object(self.bridge,'power',side_effect=RuntimeError('Stale guest boot')):
            self.assertTrue(wait_restart(self.bridge,'session','original',approved,sleep=lambda _:None))
            with self.assertRaisesRegex(RuntimeError,'Stale guest boot'):
                wait_restart(self.bridge,'session','original',{'state':'prompting','prompt_seconds':60},
                             sleep=lambda _:None)

    def test_activation_without_reboot(self):
        result=self.execute();self.assertEqual(result['state'],'activated')
        self.assertFalse(any(call[0]=='power' for call in self.bridge.calls))
    def test_partial_doctor_result_prevents_mutation(self):
        with patch.object(self.bridge,'doctor',return_value={'reachable':True,'identity_matches':True,'probe_stage':'frame_complete','error':'connection lost'}):
            with self.assertRaisesRegex(RuntimeError,'probe is incomplete'):self.execute()
        self.assertEqual(self.bridge.calls,[])
    def test_system_helper_is_explicit(self):
        result=UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,True,helper='system')
        self.assertEqual(result['helper'],'system')
        caps,launch=[x[3] for x in self.bridge.calls if x[0]=='run']
        self.assertEqual(caps,['/boot/HWUPSYS.ELF','--caps'])
        self.assertEqual(launch[0],'/boot/HWUPSYS.ELF')
        self.assertEqual(launch[-1],'--require-slot-aware')
        self.assertEqual(result['receiver_mode'],'slot_aware')
    def test_original_slot_aware_usage_advertises_strict_argument(self):
        self.bridge.cap_fault='slot_usage'
        result=UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,True,helper='system')
        self.assertEqual(result['receiver_mode'],'slot_aware')
        self.assertEqual([x[3][-1] for x in self.bridge.calls if x[0]=='run'],
                         ['--caps','--require-slot-aware'])
    def first_install(self,boot='a'*32):
        return UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,True,helper='system',first_slot_aware_install=boot)
    def test_system_refuses_old_helper_without_first_install(self):
        self.bridge.cap_legacy=True
        with self.assertRaisesRegex(RuntimeError,'lacks running-slot support'):
            UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
                'original',self.key,self.kernel,self.initrd,True,helper='system')
        self.assertFalse(self.bridge.activated)
        self.assertEqual(self.receipt()['last_stage'],'receiver_caps_dispatched')
        self.assertEqual(sum(call[0]=='run' for call in self.bridge.calls),1)
    def test_explicit_old_first_install_uses_plain_receiver_once(self):
        self.bridge.cap_legacy=True
        result=self.first_install()
        caps,launch=[x[3] for x in self.bridge.calls if x[0]=='run']
        self.assertEqual(caps,['/boot/HWUPSYS.ELF','--caps'])
        self.assertEqual(launch,['/boot/HWUPSYS.ELF','a'*32,self.bridge.tx])
        self.assertEqual(result['receiver_mode'],'first_install')
        self.assertEqual(self.bootstat.call_count,2)
        self.assertEqual(result['first_install_bootstat'],result['first_install_bootstat_locked'])
    def test_old_first_install_refuses_unverified_journal_before_any_claim(self):
        self.bridge.cap_legacy=True
        self.bootstat.side_effect=RuntimeError('Cannot verify first-install BOOTSTAT')
        with self.assertRaisesRegex(RuntimeError,'Cannot verify first-install BOOTSTAT'):
            self.first_install()
        self.assertFalse(self.bridge.activated)
        self.assertFalse(list(self.bridge.registry.iterdir()))
        self.assertEqual(sum(call[0]=='run' for call in self.bridge.calls),1)
    def test_old_first_install_refuses_journal_identity_change_after_lock(self):
        self.bridge.cap_legacy=True
        self.bootstat.side_effect=[{'state':'absent'},{'state':'empty'}]
        with self.assertRaisesRegex(RuntimeError,'BOOTSTAT evidence changed'):
            self.first_install()
        self.assertFalse(self.bridge.activated)
        self.assertEqual(self.receipt()['last_stage'],'receiver_listening')
    def test_old_first_install_accepts_an_empty_journal_in_both_checks(self):
        self.bridge.cap_legacy=True
        self.bootstat.return_value={'state':'empty'}
        result=self.first_install()
        self.assertEqual(result['first_install_bootstat_locked']['state'],'empty')
    def test_first_install_claim_prevents_second_attempt_on_same_boot(self):
        self.bridge.cap_legacy=True
        result=self.first_install()
        self.assertTrue(Path(result['first_install_claim']).is_file())
        self.output=self.root/'another-output'
        with self.assertRaisesRegex(RuntimeError,'first-install attempt already exists'):
            self.first_install()
        launches=[x for x in self.bridge.calls if x[0]=='run' and x[3][-1]!='--caps']
        self.assertEqual(len(launches),1)
    def test_first_install_refuses_stale_attestation_before_any_job(self):
        with self.assertRaisesRegex(RuntimeError,'does not match the verified boot'):
            self.first_install('b'*32)
        self.assertEqual(self.bridge.calls,[])
    def test_first_install_refuses_the_modern_receiver(self):
        with self.assertRaisesRegex(RuntimeError,'already has a slot-aware'):
            self.first_install()
        self.assertFalse(self.bridge.activated)
        self.assertEqual(sum(call[0]=='run' for call in self.bridge.calls),1)
    def test_first_install_refuses_unknown_old_layout_and_incomplete_caps(self):
        self.bridge.cap_legacy=True
        for fault in ('stale','operation','partial','unknown_old'):
            self.output=self.root/fault
            self.bridge.cap_fault=fault
            with self.subTest(fault=fault),self.assertRaises(RuntimeError):
                self.first_install()
            self.assertFalse(self.bridge.activated)
        self.bridge.cap_fault=None
        self.bridge.guard_caps={}
        self.output=self.root/'old-unguarded'
        with self.assertRaisesRegex(RuntimeError,'requires an explicit boot ID'):
            self.first_install(boot='a'*32)
    def test_modern_launch_failure_never_downgrades(self):
        self.bridge.launch_code=2
        with self.assertRaisesRegex(RuntimeError,'refused its negotiated launch'):
            UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
                'original',self.key,self.kernel,self.initrd,True,helper='system')
        self.assertEqual(len([x for x in self.bridge.calls if x[0]=='run']),2)
        self.assertFalse(self.bridge.activated)
    def test_invalid_pacing_prevents_launch(self):
        for rate in (0,-1,True,1<<31):
            with self.subTest(rate=rate),self.assertRaises(ValueError):
                UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep).execute(
                    'original',self.key,self.kernel,self.initrd,pace_bytes_per_second=rate)
        self.assertEqual(self.bridge.calls,[])
    @patch('hardware_bulk_protocol.sys.platform','linux')
    @patch('hardware_bulk_protocol.platform.machine',return_value='x86_64')
    def test_paced_update_passes_cap_and_records_it(self,_machine):
        def paced(*args,**kwargs):
            self.assertEqual(kwargs,{'pace_bytes_per_second':16000000})
            return self.uploader(*args)
        result=UpdateRun(self.bridge,self.output,paced,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,True,helper='system',pace_bytes_per_second=16000000)
        self.assertEqual(result['pace_bytes_per_second'],16000000)
    @patch('hardware_bulk_protocol.sys.platform','linux')
    @patch('hardware_bulk_protocol.platform.machine',return_value='x86_64')
    def test_paced_staging_never_activates_or_restarts(self,_machine):
        def paced(*args,**kwargs):
            self.assertEqual(kwargs,{'pace_bytes_per_second':16000000})
            return self.uploader(*args)
        result=UpdateRun(self.bridge,self.output,paced,self.clock.time,self.clock.sleep).execute(
            'original',self.key,self.kernel,self.initrd,helper='system',pace_bytes_per_second=16000000)
        self.assertEqual(result['state'],'staged_only');self.assertFalse(result['storage_modified'])
        self.assertFalse(self.bridge.activated)
        self.assertFalse(any(call[0]=='power' for call in self.bridge.calls))
    @patch('hardware_bulk_protocol.sys.platform','linux')
    @patch('hardware_bulk_protocol.platform.machine',return_value='x86_64')
    def test_socket_pacing_failure_precedes_authentication(self,_machine):
        class Conn:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def setsockopt(self,*args):raise OSError('injected pacing failure')
        with patch('hardware_update_protocol.socket.create_connection',return_value=Conn()), \
             patch('hardware_update_protocol.authenticate') as auth,self.assertRaises(OSError):
            upload('127.0.0.1',1235,'1'*32,'a'*32,'b'*32,bytes(64),bytes(512),pace_bytes_per_second=16000000)
        auth.assert_not_called()
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
    def test_configured_boot_observation_timeout_never_replays(self):
        self.bridge.boot_changes=False
        run=UpdateRun(self.bridge,self.output,self.uploader,self.clock.time,self.clock.sleep)
        with self.assertRaises(TimeoutError):
            run.execute('original',self.key,self.kernel,self.initrd,True,True,'candidate',boot_timeout_s=3)
        self.assertLess(self.clock.now,5)
        self.assertEqual(sum(x[0]=='power' and x[3]=='commit' for x in self.bridge.calls),1)
        self.assertEqual(self.receipt()['boot_timeout_s'],3)

    def test_restart_timeout_names_stage_and_blockers(self):
        self.bridge.blocked={'state':'closing','error':0,'stage':'sigkill','blocker_count':3,'next_signal_ms':0,
            'status':'Closing apps - no window: pulseaudio pid 51, sleep pid 60 (+1); killed, waiting for exit, or Cancel',
            'blockers':[{'pid':51,'name':'pulseaudio','window':False,'service':False},{'pid':60,'name':'sleep','window':False,'service':False}]}
        with self.assertRaises(TimeoutError) as caught:self.execute(restart=True)
        message=str(caught.exception)
        for text in ('stage=sigkill','pulseaudio pid 51 (no window)','sleep pid 60','(+1 more)','killed, waiting for exit','cancel'):
            self.assertIn(text,message)
        receipt=self.receipt()
        self.assertEqual((receipt['state'],receipt['last_stage']),('needs_inspection','restart_dispatched'))
        self.assertIn('pulseaudio pid 51',receipt['error']);self.assertEqual(receipt['power']['stage'],'sigkill')
        self.assertFalse(any(x[0]=='power' and x[3]=='commit' for x in self.bridge.calls))
    def test_cancelled_restart_is_reported_not_retried(self):
        self.bridge.blocked={'state':'cancelled','error':0,'stage':'cancelled','status':'Shutdown cancelled','blockers':[]}
        with self.assertRaises(RuntimeError) as caught:self.execute(restart=True)
        self.assertIn('state=cancelled',str(caught.exception))
        self.assertEqual(sum(x[0]=='power' and x[3]=='restart' for x in self.bridge.calls),1)

    def test_invalid_key_never_launches(self):
        self.key.write_text('secret\n')
        with self.assertRaises(ValueError):self.execute()
        self.assertEqual(self.bridge.calls,[])

if __name__=='__main__':unittest.main()
