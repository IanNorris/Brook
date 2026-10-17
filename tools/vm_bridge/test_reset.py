"""Real reset state machine with scripted QMP/guest transitions and failures."""
import contextlib
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import reset
import server


class ResetTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.session='a'*32;self.run=self.root/self.session;self.run.mkdir()
        self.log=self.run/'serial.log';self.log.write_text('KERNEL PANIC old boot\n')
        self.data={'generation':'before','serial_log':str(self.log),'qemu_reboot_action':'shutdown'}
        self.before='b'*32;self.after='c'*32;self.current=self.before
        self.events=[];self.running=True;self.reset_error=None;self.restore_error=None;self.policy_error=None
        self.preserve_boot=False;self.cancel=None;self.connections=0
        self.runstate='running';self.old_epoch_error=False;self.zero_after_reset=False
        self.active=False;self.states=[]
        self.status_error=None
        for name in ('last-frame.json','frame-source.json'):(self.run/name).write_text('{}')
    def guest(self,data,command,timeout):
        self.assertTrue(command=='bridge epoch' or command.startswith(('bridge epoch init ','bridge ui takeover 1 ')))
        self.events.append(('guest',self.current))
        self.assertGreater(timeout,0);self.assertLessEqual(timeout,3)
        if self.old_epoch_error:
            self.old_epoch_error=False;raise ConnectionError('old guest unreachable')
        if command.startswith('bridge epoch init '):self.current=command.rsplit(' ',1)[-1]
        if command.startswith('bridge ui takeover '):return {'user_takeover':True}
        return {'boot_id':self.current}
    @contextlib.contextmanager
    def qmp(self,session):
        self.assertFalse(self.active,'QMP monitor accepts one client at a time');self.active=True
        self.connections+=1;self.assertEqual(session,self.session)
        owner=self
        class Q:
            def call(self,name,args=None):
                owner.events.append((name,args))
                if name=='set-action':
                    if args=={'reboot':'shutdown'} and owner.restore_error:raise owner.restore_error
                    if args=={'reboot':'reset'} and owner.policy_error:raise owner.policy_error
                if name=='system_reset':
                    owner.assertNotEqual(owner.data['generation'],'before')
                    owner.assertFalse((owner.run/'last-frame.json').exists())
                    owner.assertFalse((owner.run/'frame-source.json').exists())
                    disk=json.loads((owner.run/'reset.json').read_text())
                    owner.assertEqual(disk['reset_delivery'],'unknown')
                    if not owner.preserve_boot:owner.current='0'*32 if owner.zero_after_reset else owner.after
                    if owner.cancel:owner.cancel.set()
                    if owner.reset_error:raise owner.reset_error
                if name=='cont':owner.running=True;owner.runstate='running'
                if name=='query-status':
                    if owner.status_error:raise owner.status_error
                    if owner.states:
                        owner.runstate=owner.states.pop(0);owner.running=owner.runstate=='running'
                    return {'running':owner.running,'status':owner.runstate if owner.running else owner.runstate if owner.runstate!='running' else 'shutdown'}
                return {}
        try:yield Q()
        finally:self.active=False
    def perform(self,**kw):
        return reset.perform(self.session,self.data,self.root,self.guest,self.qmp,wait_s=kw.pop('wait_s',0),**kw)
    def status(self):return reset.status(self.session,self.data,self.root,self.guest,self.qmp)
    def mutations(self):return [(name,args) for name,args in self.events if name not in {'guest','query-status'}]
    def test_reset_requires_changed_boot_running_and_restores_only_reboot_policy(self):
        r=self.perform();self.assertEqual(r['state'],'restarted');self.assertTrue(r['restart_verified'])
        self.assertTrue(r['guest_ready']);self.assertTrue(r['reboot_policy_restored'])
        self.assertEqual(self.mutations(),[('set-action',{'reboot':'reset'}),('system_reset',None),('set-action',{'reboot':'shutdown'})])
        self.assertEqual(self.data['generation_log_offset'],self.log.stat().st_size)
        self.assertEqual(json.loads((self.run/'reset.json').read_text()),r)
    def test_default_reset_policy_needs_no_override(self):
        self.data['qemu_reboot_action']='reset';r=self.perform()
        self.assertTrue(r['restart_verified']);self.assertEqual(self.mutations(),[('system_reset',None)])
    def test_qmp_ack_and_running_with_old_epoch_are_not_restart(self):
        self.preserve_boot=True;r=self.perform()
        self.assertEqual(r['state'],'pending');self.assertFalse(r['restart_verified']);self.assertTrue(r['guest_ready'])
        self.current=self.after;self.events=[];r=self.status()
        self.assertTrue(r['restart_verified']);self.assertEqual(self.mutations(),[])
    def test_nonrunning_qmp_prevents_guest_readiness_claim(self):
        self.running=False;r=self.perform()
        self.assertEqual(r['vm']['status'],'shutdown');self.assertFalse(r['restart_verified']);self.assertFalse(r['guest_ready'])
    def test_lost_reset_reply_invalidates_old_refs_and_never_replays_on_status(self):
        self.reset_error=TimeoutError('lost reset reply');r=self.perform()
        self.assertEqual(r['state'],'unknown');self.assertEqual(r['reset_delivery'],'unknown')
        self.assertTrue(r['reboot_policy_restored']);self.assertEqual(self.connections,2)
        self.assertEqual(sum(name=='system_reset' for name,args in self.events),1)
        self.events=[];r=self.status();self.assertTrue(r['restart_verified']);self.assertEqual(self.mutations(),[])
    def test_policy_restore_failure_is_explicit_and_status_can_verify_separately(self):
        self.restore_error=ConnectionError('restore unavailable');r=self.perform()
        self.assertFalse(r['reboot_policy_restored']);self.assertEqual(r['reset_delivery'],'acknowledged')
        self.assertEqual(r['policy_restore_error'],'restore unavailable')
        r=self.status();self.assertTrue(r['restart_verified']);self.assertFalse(r['reboot_policy_restored'])
    def test_unknown_policy_refuses_before_any_vm_action(self):
        for value in (None,'unknown'):
            self.data['qemu_reboot_action']=value
            with self.assertRaisesRegex(ValueError,'unknown QEMU reboot policy'):self.perform()
        self.assertEqual(self.events,[]);self.assertEqual(self.data['generation'],'before')
    def test_lost_policy_reply_still_attempts_restore_but_never_reset(self):
        self.policy_error=TimeoutError('lost policy reply');r=self.perform()
        self.assertEqual(r['reset_delivery'],'not_sent');self.assertTrue(r['reboot_policy_restored'])
        self.assertEqual(self.data['generation'],'before')
        self.assertNotIn(('system_reset',None),self.events)
    def test_pre_dispatch_cancel_preserves_generation_and_frames(self):
        flag=threading.Event();flag.set();r=self.perform(cancel=flag)
        self.assertEqual(r['state'],'cancelled');self.assertEqual(self.data['generation'],'before')
        self.assertTrue((self.run/'last-frame.json').exists());self.assertEqual(self.mutations(),[])
    def test_cancelled_wait_does_not_repeat_or_undo_reset(self):
        flag=threading.Event();self.cancel=flag;self.preserve_boot=True
        r=self.perform(cancel=flag);self.assertTrue(r['wait_cancelled']);self.assertFalse(r['restart_verified'])
        self.assertTrue(r['reboot_policy_restored']);self.assertEqual(sum(name=='system_reset' for name,args in self.events),1)
    def test_missing_old_epoch_can_report_running_but_not_prove_restart(self):
        self.old_epoch_error=True;r=self.perform()
        self.assertEqual(r['state'],'running_unverified');self.assertFalse(r['restart_verified']);self.assertTrue(r['guest_ready'])
    def test_fresh_zero_epoch_is_initialized_instead_of_polling_forever(self):
        self.zero_after_reset=True;r=self.perform()
        self.assertTrue(r['restart_verified']);self.assertNotEqual(r['current_guest_boot_id'],'0'*32)
        self.assertNotEqual(r['current_guest_boot_id'],self.before)
    def test_only_reset_eligible_pauses_are_resumed(self):
        for state in ('prelaunch','paused'):
            self.setUp();self.running=False;self.runstate=state;r=self.perform(wait_s=.02)
            self.assertTrue(r['restart_verified']);self.assertIn(('cont',None),self.events)
        for state in ('shutdown','guest-panicked','debug','io-error','internal-error'):
            self.setUp();self.running=False;self.runstate=state;r=self.perform()
            self.assertFalse(r['restart_verified']);self.assertNotIn(('cont',None),self.events)
    def test_takeover_prevents_auto_resume_and_is_reapplied_on_running_new_boot(self):
        (self.run/'control.json').write_text('{"user_takeover":true}')
        self.running=False;self.runstate='prelaunch';r=self.perform()
        self.assertTrue(r['resume_skipped_takeover']);self.assertNotIn(('cont',None),self.events)
        self.running=True;self.runstate='running';r=self.perform()
        self.assertEqual(r['guest_takeover_reply'],{'user_takeover':True})
    def test_stale_generation_status_does_not_contact_vm(self):
        self.perform();self.data['generation']='another';self.events=[]
        r=self.status();self.assertEqual(r['state'],'stale');self.assertTrue(r['restart_verified']);self.assertEqual(self.events,[])
    def test_delayed_prelaunch_is_resumed_once(self):
        self.states=['shutdown','prelaunch','running'];r=self.perform(wait_s=.6)
        self.assertTrue(r['restart_verified']);self.assertEqual(self.events.count(('cont',None)),1)
    def test_later_pause_preserves_historical_restart_and_reports_current_state(self):
        r=self.perform();self.running=False;self.runstate='paused';self.events=[]
        status=self.status();self.assertEqual(status['state'],'restarted');self.assertTrue(status['restart_verified'])
        self.assertFalse(status['guest_ready']);self.assertFalse(status['current_restart_observed'])
        self.assertEqual(status['verified_guest_boot_id'],r['current_guest_boot_id']);self.assertEqual(self.mutations(),[])
    def test_not_sent_failure_and_cancel_are_not_relabelled_pending_or_stale(self):
        self.policy_error=ConnectionError('policy unavailable');r=self.perform();self.events=[]
        status=self.status();self.assertEqual(status['state'],r['state']);self.assertEqual(self.events,[])
        self.policy_error=None;flag=threading.Event();flag.set();r=self.perform(cancel=flag)
        self.assertEqual(self.status()['state'],'cancelled')
    def test_stopped_status_returns_cached_attempt_without_qmp(self):
        r=self.perform();self.events=[]
        stopped=reset.status(self.session,self.data,self.root,self.guest,self.qmp,available=False)
        self.assertTrue(stopped['restart_verified']);self.assertFalse(stopped['guest_ready'])
        self.assertEqual(stopped['current_observation_state'],'vm_unavailable');self.assertEqual(self.events,[])
        with patch.object(server,'load_session',return_value={**self.data,'state':'stopped'}),patch.object(server,'REGISTRY',self.root):
            self.assertTrue(server.reset_status(self.session)['restart_verified'])
    def test_epoch_calls_share_one_budget(self):
        timeouts=[]
        def request(data,command,timeout):
            timeouts.append(timeout)
            if len(timeouts)==1:
                time.sleep(.01);return {'boot_id':'0'*32}
            return {'boot_id':self.after}
        self.assertEqual(reset.read_epoch(self.data,request,.05),self.after)
        self.assertLess(timeouts[1],.045)
    def test_failed_current_read_clears_old_observation_without_erasing_restart(self):
        self.perform();self.status_error=TimeoutError('QMP temporarily unavailable')
        r=self.status();self.assertTrue(r['restart_verified']);self.assertEqual(r['verified_guest_boot_id'],self.after)
        self.assertIsNone(r['vm']);self.assertIsNone(r['current_guest_boot_id'])
        self.assertFalse(r['guest_ready']);self.assertFalse(r['current_restart_observed'])
        self.assertEqual(r['current_observation_state'],'unavailable')
        self.status_error=None;r=self.status();self.assertTrue(r['current_restart_observed'])
        self.assertNotIn('current_error',r)
    def test_launch_policy_inference_is_conservative(self):
        self.assertEqual(reset.launch_reboot_action(['qemu']), 'reset')
        self.assertEqual(reset.launch_reboot_action(['qemu','-no-reboot','-no-shutdown']), 'shutdown')
        for args in [['qemu','-action','reboot=reset'],['qemu','-action=reboot=reset'],['qemu','-readconfig','custom'],
                     ['qemu','--action','reboot=reset'],['qemu','-readconfig=custom']]:
            self.assertIsNone(reset.launch_reboot_action(args))


if __name__=='__main__':unittest.main()
