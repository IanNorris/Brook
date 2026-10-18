import common
import os
import contextlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import shutdown

SESSION='1'*32
ID='2'*32
BOOT='3'*32
NEW='4'*32
class Power(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);(self.root/SESSION).mkdir()
        self.log=self.root/'serial.log';self.log.write_text('owned fixture\n')
        self.data={'pid':123,'process_identity':'owned','generation':SESSION,'serial_log':str(self.log),'qemu_reboot_action':'reset','exit_code':0}
        self.alive=True;self.phase='closing';self.calls=[];self.boot=BOOT;self.fail=False
        self.patches=[patch.object(shutdown,'REGISTRY',self.root),patch.object(shutdown,'load_session',side_effect=lambda *a,**k:self.data),patch.object(shutdown,'session_lock',side_effect=lambda *a,**k:contextlib.nullcontext()),patch.object(shutdown,'process_is_alive',side_effect=lambda *a:self.alive),patch.object(shutdown,'exchange',side_effect=self.exchange)]
        for p in self.patches:p.start()
        self.addCleanup(self.tmp.cleanup)
        for p in self.patches:self.addCleanup(p.stop)
    def exchange(self,session,verb,request,boot,**kw):
        self.calls.append(verb)
        if verb=='hello':return {'guest_boot_id':self.boot,'state':'ok'}
        if self.fail and verb in {'shutdown','restart'}:raise TimeoutError('lost reply')
        if verb=='cancel':self.phase='cancelled'
        if verb=='commit':
            self.assertNotEqual(self.data['generation'],SESSION)
            self.phase='committing'
        return {'state':self.phase,'result':0,'guest_sync_completed':self.phase in {'ready','committing'},'guest_boot_id':self.boot}
    def call(self,action,**kw):return shutdown.operate(SESSION,action,ID,wait_s=0,guest=lambda *a,**k:{'boot_id':self.boot},**kw)
    def test_retained_start_status_cancel_without_replay(self):
        self.assertEqual(self.call('shutdown')['state'],'closing')
        self.assertEqual(self.call('shutdown')['state'],'closing')
        self.assertEqual(self.call('status')['state'],'closing')
        self.assertEqual(self.call('cancel')['state'],'cancelled')
        self.assertEqual(self.calls,['hello','shutdown','power_status','power_status','cancel'])
    def test_timeout_unknown_and_missing_never_redispatch(self):
        self.fail=True;r=self.call('shutdown');self.assertEqual(r['dispatch'],'unknown')
        self.phase='missing';self.assertEqual(self.call('status')['state'],'missing')
        self.call('shutdown');self.assertEqual(self.calls.count('shutdown'),1)
    def test_status_ready_is_read_only_and_commit_sent_once(self):
        self.call('shutdown');self.phase='ready';self.call('status');self.assertNotIn('commit',self.calls)
        self.call('commit');self.call('commit');self.assertEqual(self.calls.count('commit'),1)
        self.alive=False;r=self.call('status');self.assertTrue(r['qemu_exited']);self.assertFalse(r['original_launcher_exit_verified'])
    def test_post_commit_retained_failure_is_observed_without_replay(self):
        self.call('shutdown');self.phase='ready';self.call('status');self.call('commit')
        self.phase='failed';r=self.call('status');self.assertEqual(r['state'],'failed');self.assertEqual(self.calls.count('commit'),1)

    def test_mismatched_action_and_unready_commit_rejected(self):
        self.call('shutdown')
        with self.assertRaises(ValueError):self.call('restart')
        with self.assertRaises(RuntimeError):self.call('commit')
        self.assertNotIn('commit',self.calls)
    def test_restart_requires_new_boot_and_tcp_handshake(self):
        self.call('restart');self.phase='ready';self.call('status');self.call('commit')
        self.assertNotEqual(self.call('status')['state'],'restarted')
        self.boot=NEW;r=self.call('status');self.assertEqual(r['state'],'restarted');self.assertTrue(r['restart_verified'])
        self.assertEqual(self.calls.count('commit'),1)
    def test_stop_on_reboot_policy_is_not_silently_overridden(self):
        self.data['qemu_reboot_action']='shutdown'
        with self.assertRaises(RuntimeError):self.call('restart')
        self.assertEqual(self.calls,[])
    def test_real_session_lock_can_observe_owned_stopped_receipt(self):
        self.call('shutdown');self.alive=False
        self.data.update(session=SESSION,owner_uid=os.getuid(),state='stopped')
        (self.root/SESSION/'session.json').write_text(json.dumps(self.data))
        with patch.object(common,'REGISTRY',self.root),patch.object(shutdown,'session_lock',common.session_lock):
            r=self.call('status')
        self.assertTrue(r['qemu_exited']);self.assertEqual(r['state'],'stopped')

    def test_no_unknown_receipt_status_or_invalid_actions(self):
        with self.assertRaises(ValueError):self.call('status')
        with self.assertRaises(ValueError):self.call('force')
        with self.assertRaises(ValueError):shutdown.operate(SESSION,'status')
        self.assertEqual(self.calls,[])
if __name__=='__main__':unittest.main()
