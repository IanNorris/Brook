"""Semantic launcher recovery, boot/catalog identity and completion evidence."""
import threading
import unittest
from common import write_json
from test_ui import UiTests


class LaunchTests(UiTests):
    # Reuse only the fake transport/setup, not inherited window test cases.
    def guest(self,data,command,timeout):
        parts=command.split()
        if parts[1:3]==['ui','windows'] and getattr(self,'fail_windows',False):raise TimeoutError('windows unavailable')
        if parts[1:3]==['ui','caps']:
            return {'ui_api':1,'apps_api':1,'launch_api':1}
        if parts[1:3]==['ui','launch']:
            self.calls.append(command)
            if parts[-1]!=self.boot:return {'ui_error':'Stale guest boot'}
            self.mutations+=1
            reply={'request_id':parts[3],'kind':'launch','state':'completed',
                'coalesced':False,'error_code':0,'launch':dict(self.launch_record)}
            if int(parts[5])!=17:reply.update(state='rejected',error_code=-116,launch=None)
            self.requests[parts[3]]=reply
            if self.drop_reply:raise TimeoutError('lost launch reply')
            return reply
        return super().guest(data,command,timeout)

    def setUp(self):
        super().setUp()
        self.app=self.ui.apps(self.session)['apps'][0]['reference']
        self.launch_record={'token':'123','state':'waiting_for_window','started':True,
            'dispatch_finished':True,'dispatch_succeeded':True,'raw_dispatch_result':0,
            'window_observed':False,'window':None,'correlation':'none'}

    def launch(self,**kwargs):
        return self.ui.launch(self.session,self.app,self.ident,wait_s=0,**kwargs)

    def test_admission_is_not_window_or_application_readiness(self):
        result=self.launch()
        self.assertEqual(result['admission_state'],'completed')
        self.assertEqual(result['state'],'waiting_for_window')
        self.assertTrue(result['conditions']['dispatched'])
        self.assertFalse(result['wait_condition_met'])
        self.assertIsNone(result['current_window'])
        self.assertFalse(any('capture' in c for c in self.calls))

    def test_dispatch_evidence_survives_window_observation_failure(self):
        self.fail_windows=True
        result=self.launch(wait_for='dispatched')
        self.assertTrue(result['wait_condition_met'])
        self.assertIn('windows unavailable',result['observation_error'])
        self.assertIsNone(result['observation'])
        self.assertTrue(self.ui.status(self.session,self.ident,wait_for='admitted')['wait_condition_met'])
        self.assertFalse(self.ui.status(self.session,self.ident,wait_for='window')['wait_condition_met'])

    def test_reconnect_recovers_lost_reply_without_replaying_launch(self):
        self.drop_reply=True
        self.assertEqual(self.launch()['state'],'waiting_for_window')
        self.assertEqual(self.launch()['state'],'waiting_for_window')
        self.assertEqual(self.mutations,1)
        self.app['catalog_generation']=18
        with self.assertRaisesRegex(ValueError,'different arguments'):self.launch()

    def test_unknown_reply_and_status_never_replays(self):
        self.drop_reply=True;self.fail_reads=True
        self.assertEqual(self.launch()['state'],'unknown')
        self.launch();self.assertEqual(self.mutations,1)

    def test_early_window_does_not_hide_dispatch_failure(self):
        self.launch_record.update(state='failed',dispatch_succeeded=False,raw_dispatch_result=-2,
            window_observed=True,window={'window_id':1,'incarnation':self.window['incarnation']})
        result=self.launch()
        self.assertEqual(result['state'],'failed')
        self.assertEqual(result['launch']['raw_dispatch_result'],-2)
        self.assertIsNotNone(result['current_window'])
        self.assertFalse(result['wait_condition_met'])

    def test_prior_window_observation_requires_current_exact_incarnation(self):
        self.launch_record.update(state='window_observed',window_observed=True,
            window={'window_id':1,'incarnation':self.window['incarnation']},correlation='app_id_presence',existing_window=True)
        result=self.launch()
        self.assertTrue(result['wait_condition_met'])
        self.assertEqual(result['launch']['window_reference'],self.ref)
        self.rows=[]
        self.assertFalse(self.ui.status(self.session,self.ident,wait_for='window')['wait_condition_met'])
        self.rows=[{**self.window,'incarnation':'999'}]
        self.assertFalse(self.ui.status(self.session,self.ident,wait_for='window')['wait_condition_met'])

    def test_stale_catalog_rejected_without_claiming_dispatch(self):
        self.app['catalog_generation']=18
        result=self.launch()
        self.assertEqual(result['state'],'rejected')
        self.assertEqual(result['guest']['error_code'],-116)
        self.assertFalse(result['wait_condition_met'])

    def test_stopped_guest_retains_successful_dispatch_without_window(self):
        self.launch();self.data['state']='stopped';before=len(self.calls)
        result=self.ui.status(self.session,self.ident,wait_for='window')
        self.assertTrue(result['cached']);self.assertEqual(result['state'],'lost')
        self.assertEqual(result['recorded_state'],'waiting_for_window')
        self.assertEqual(result['effect'],'confirmed')
        self.assertTrue(result['launch']['dispatch_succeeded'])
        self.assertIsNone(result['current_window']);self.assertFalse(result['wait_condition_met'])
        self.assertEqual(len(self.calls),before)

    def test_cancelled_wait_does_not_cancel_launch_or_replay_it(self):
        cancel=threading.Event();cancel.set()
        result=self.ui.launch(self.session,self.app,self.ident,wait_s=1,cancel=cancel)
        self.assertTrue(result['wait_cancelled'])
        self.assertEqual(result['state'],'waiting_for_window')
        self.assertEqual(self.mutations,1)
        self.assertFalse(self.launch()['wait_condition_met'])
        self.assertEqual(self.mutations,1)

    def test_unsupported_guest_cannot_admit_launch(self):
        original=self.ui.capabilities
        def caps(*args):
            data,result=original(*args);result.pop('launch_api',None);return data,result
        self.ui.capabilities=caps
        with self.assertRaisesRegex(RuntimeError,'unsupported'):self.launch()
        self.assertEqual(self.mutations,0)
        self.assertFalse(self.ui.path(self.session,self.ident).exists())

    def test_cross_kind_request_id_cannot_mutate_twice(self):
        self.launch()
        with self.assertRaisesRegex(ValueError,'different arguments'):self.submit()
        self.assertEqual(self.mutations,1)

    def test_reboot_stopped_takeover_and_invalid_reference(self):
        write_json(self.root/self.session/'control.json',{'user_takeover':True})
        self.assertEqual(self.launch()['state'],'rejected')
        self.assertEqual(self.mutations,0)
        self.ident='e'*32
        write_json(self.root/self.session/'control.json',{'user_takeover':False})
        self.launch();self.boot='f'*32
        self.assertEqual(self.ui.status(self.session,self.ident)['state'],'lost')
        with self.assertRaisesRegex(ValueError,'Stale guest boot'):self.launch()
        self.data['state']='stopped'
        self.assertTrue(self.ui.status(self.session,self.ident)['cached'])
        self.app['catalog_generation']=True
        with self.assertRaisesRegex(ValueError,'catalog generation'):self.launch()

# Inherited tests exercise the shared window code in test_ui; do not run twice.
for _name in tuple(dir(LaunchTests)):
    if _name.startswith('test_') and _name not in LaunchTests.__dict__:
        setattr(LaunchTests,_name,None)
# Avoid unittest discovering imported test class in this module.
del UiTests
