"""Retry/reboot/identity and truthful completion tests for semantic window hooks."""
import contextlib
import json
from pathlib import Path
import tempfile
import unittest

from common import write_json
from ui import Ui


class UiTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.session='a'*32;self.ident='b'*32;self.boot='c'*32
        (self.root/self.session).mkdir()
        self.data={'generation':'generation','state':'running'}
        self.window={'window_id':1,'incarnation':'9007199254740993','title':'Client',
            'client':{'pid':8,'incarnation':'1000'},'maximized':False,'minimized':False}
        self.rows=[self.window];self.calls=[];self.requests={};self.mutations=0
        self.drop_reply=False;self.client_tasks=1;self.queue=False;self.fail_reads=False
        @contextlib.contextmanager
        def lock(session,timeout=10):yield self.data
        self.ui=Ui(self.root,lambda *a,**k:self.data,lock,self.guest,write_json)
        self.ref=self.ui.windows(self.session)['windows'][0]['reference']

    def guest(self,data,command,timeout):
        self.calls.append(command);parts=command.split()
        if parts[1]=='epoch':
            if len(parts)>2 and self.boot=='0'*32:self.boot=parts[3]
            return {'boot_id':self.boot}
        if parts[2]=='caps':return {'ui_api':1,'boot_id':self.boot}
        if parts[-1]!=self.boot:return {'ui_error':'Stale guest boot'}
        action=parts[2]
        if action=='windows':return {'windows':[dict(row) for row in self.rows],'sample_tick_ms':99}
        if action=='process':return {'live_group_tasks':self.client_tasks,'sample_tick_ms':100}
        if action=='status':
            if self.fail_reads:raise TimeoutError('read unavailable')
            return self.requests.get(parts[3],{'ui_error':'Unknown UI request'})
        if action=='window':
            self.mutations+=1
            reply={'request_id':parts[3],'state':'queued' if self.queue else 'completed',
                'applied_window':dict(self.window),'error_code':0}
            if parts[5]!=self.window['incarnation']:
                reply.update(state='rejected',error_code=-2,applied_window=None)
            self.requests[parts[3]]=reply
            if self.drop_reply:raise TimeoutError('lost mutation reply')
            return reply
        raise AssertionError(command)

    def submit(self,action='minimize',**options):
        return self.ui.submit(self.session,self.ref,action,self.ident,wait_s=0,**options)

    def test_action_returns_observation_and_large_incarnation_without_precision_loss(self):
        result=self.submit()
        self.assertTrue(result['wait_condition_met'])
        self.assertEqual(result['observation']['windows'][0]['reference'],self.ref)
        self.assertEqual(result['state'],'completed')
        self.assertEqual(self.mutations,1)

    def test_identical_reconnect_recovers_without_mutation_replay(self):
        self.drop_reply=True
        result=self.submit()
        self.assertEqual(result['state'],'completed')
        self.assertEqual(self.submit()['state'],'completed')
        self.assertEqual(self.mutations,1)
        with self.assertRaisesRegex(ValueError,'different arguments'):self.submit('focus')

    def test_lost_reply_and_status_do_not_trigger_mutation_retry(self):
        self.drop_reply=True;self.fail_reads=True
        result=self.submit()
        self.assertEqual(result['state'],'unknown')
        self.assertFalse(result['wait_condition_met'])
        self.submit();self.assertEqual(self.mutations,1)

    def test_close_dispatch_does_not_prove_disappearance_or_client_exit(self):
        result=self.submit('close',wait_for='closed')
        self.assertEqual(result['state'],'completed')
        self.assertFalse(result['wait_condition_met'])
        self.rows=[]
        result=self.ui.status(self.session,self.ident,wait_for='closed')
        self.assertTrue(result['wait_condition_met'])
        self.assertFalse(self.ui.status(self.session,self.ident,wait_for='client_group_exit')['wait_condition_met'])
        self.client_tasks=0
        self.assertTrue(self.ui.status(self.session,self.ident,wait_for='client_group_exit')['wait_condition_met'])
        self.assertEqual(self.mutations,1)

    def test_guest_self_reboot_invalidates_status_and_window_reference(self):
        self.submit();self.boot='d'*32
        result=self.ui.status(self.session,self.ident)
        self.assertEqual(result['state'],'lost')
        self.assertIsNone(result['observation'])
        with self.assertRaisesRegex(ValueError,'Stale guest boot'):self.submit()
        self.assertEqual(self.mutations,1)

    def test_reused_window_slot_is_rejected_at_dispatch(self):
        self.window['incarnation']='9007199254740994'
        result=self.submit()
        self.assertEqual(result['state'],'rejected')
        self.assertIsNone(result['current_window'])
        self.assertFalse(result['wait_condition_met'])

    def test_restore_requires_explicit_target_and_takeover_blocks_dispatch(self):
        with self.assertRaisesRegex(ValueError,'restore_target'):self.submit('restore')
        self.assertEqual(self.mutations,0)
        write_json(self.root/self.session/'control.json',{'user_takeover':True})
        result=self.submit()
        self.assertEqual(self.mutations,0)
        self.assertEqual(result['state'],'rejected')
        self.assertIn('takeover',result['error'])

    def test_stopped_guest_returns_only_labelled_cached_dispatch_evidence(self):
        self.submit();self.data['state']='stopped';before=len(self.calls)
        result=self.ui.status(self.session,self.ident)
        self.assertTrue(result['cached']);self.assertFalse(result['wait_condition_met'])
        self.assertEqual(len(self.calls),before)

    def test_wait_timeout_does_not_cancel_queued_action(self):
        self.queue=True
        result=self.ui.submit(self.session,self.ref,'focus',self.ident,wait_s=.01)
        self.assertTrue(result['wait_timed_out'])
        self.assertEqual(result['state'],'queued');self.assertEqual(self.mutations,1)


class TakeoverOrderingTests(unittest.TestCase):
    def test_two_clients_cannot_reverse_host_and_guest_takeover_order(self):
        import threading
        from unittest.mock import patch
        import server
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);session='a'*32;(root/session).mkdir()
            entered=threading.Event();allow=threading.Event();release_started=threading.Event();release_entered=threading.Event();events=[];latch={}
            @contextlib.contextmanager
            def lock(*args,**kwargs):yield {'generation':'g','state':'running'}
            class Client(Ui):
                def takeover(self,session,enabled):
                    events.append(enabled)
                    if enabled:
                        entered.set();allow.wait(2)
                    else:release_entered.set()
                    latch['user_takeover']=enabled
                    return {'user_takeover':enabled}
            def client():return Client(root,lambda *a,**k:{},lock,lambda *a,**k:{},write_json)
            def release():
                release_started.set();server.control(session,'release')
            with patch.object(server,'REGISTRY',root),patch.object(server,'load_session',return_value={}),patch.object(server,'window_ui',side_effect=client):
                first=threading.Thread(target=server.control,args=(session,'takeover'))
                second=threading.Thread(target=release)
                first.start();self.assertTrue(entered.wait(1))
                second.start();self.assertTrue(release_started.wait(1))
                # The guest takeover is deliberately blocked. Release must not
                # publish opposite host intent until that transition completes.
                self.assertFalse(release_entered.wait(.05),'Release entered guest sync before takeover completed')
                self.assertTrue(json.loads((root/session/'control.json').read_text())['user_takeover'])
                allow.set();first.join(2);second.join(2)
                self.assertFalse(first.is_alive());self.assertFalse(second.is_alive())
            self.assertEqual(events,[True,False])
            self.assertFalse(latch['user_takeover'])
            self.assertFalse(json.loads((root/session/'control.json').read_text())['user_takeover'])


if __name__=='__main__':unittest.main()
