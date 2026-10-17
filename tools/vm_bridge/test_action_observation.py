"""Action evidence remains durable when optional later observation fails or changes."""
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch
import test_ui
import server
import media
from observation import StaleObservation


class ActionObservationTests(unittest.TestCase):
    def setUp(self):
        self.fixture=test_ui.UiTests('test_action_returns_observation_and_large_incarnation_without_precision_loss')
        self.fixture.setUp();self.addCleanup(self.fixture.doCleanups)
        self.s=self.fixture.session;self.ident=self.fixture.ident;self.calls=[];self.fail=None
        test=self
        class Collector:
            def __init__(self,*args):pass
            def collect(self,session,**options):
                test.calls.append(options)
                if test.fail:raise test.fail
                data=server.png_rgb(2,1,b'\xff\0\0\0\xff\0')
                p=test.fixture.root/session/'artifacts'/('d'*32+'.png');p.parent.mkdir(exist_ok=True);p.write_bytes(data)
                if getattr(test,'remove_artifact',False):p.unlink()
                return {'state':'completed','components':{'frame':{'state':'ok','result':{
                    'image':data,'data':{'artifact':str(p),'frame_epoch':8}}}},'expected_identity':{
                    'generation':options['expected_generation'],'guest_boot_id':options['expected_boot_id']}}
        for name,value in [('window_ui',lambda:self.fixture.ui),('Observations',Collector),('REGISTRY',self.fixture.root)]:
            p=patch.object(server,name,value);p.start();self.addCleanup(p.stop)
    def window(self,**kw):
        return server.window_action(self.s,self.fixture.ref,'minimize',self.ident,wait_s=0,**kw)
    def test_no_include_preserves_default_and_never_starts_capture(self):
        r=self.window();self.assertNotIn('post_action_observation',r);self.assertEqual(self.calls,[])
    def test_action_and_bound_post_observation_are_separate(self):
        r=self.window(include=['frame','windows'])
        self.assertTrue(r['wait_condition_met']);self.assertEqual(r['state'],'completed')
        self.assertEqual(self.calls[0]['expected_generation'],'generation')
        self.assertEqual(self.calls[0]['expected_boot_id'],self.fixture.boot)
        self.assertEqual(r['post_action_observation']['state'],'completed')
    def test_confirmed_wm_action_does_not_require_a_later_windows_reply(self):
        with patch.object(self.fixture.ui,'windows',side_effect=TimeoutError('windows unavailable')):
            r=self.window(include=['frame'])
        self.assertEqual(r['state'],'completed');self.assertTrue(r['wait_condition_met'])
        self.assertIn('windows unavailable',r['observation_error'])
        self.assertEqual(r['post_action_observation']['state'],'completed')

    def test_optional_failure_cannot_hide_dispatch_or_repeat_it_on_recovery(self):
        self.fail=TimeoutError('capture unavailable');r=self.window(include=['frame'])
        self.assertEqual(r['state'],'completed');self.assertTrue(r['wait_condition_met'])
        self.assertEqual(r['post_action_observation']['state'],'unavailable')
        self.assertEqual(self.fixture.mutations,1)
        self.fail=None
        r=server.ui_status(self.s,self.ident,include=['frame'],wait_s=0)
        self.assertEqual(r['post_action_observation']['state'],'completed')
        self.assertEqual(self.fixture.mutations,1)
    def test_different_include_or_crop_is_a_read_not_new_mutation(self):
        self.window(include=['errors'])
        r=self.window(include=['frame'],crop={'x':0,'y':0,'width':100,'height':100})
        self.assertEqual(r['state'],'completed');self.assertEqual(self.fixture.mutations,1)
        self.assertEqual(len(self.calls),2)
    def test_stale_observation_does_not_relabel_confirmed_action(self):
        self.fail=StaleObservation('Action guest boot changed before observation')
        r=self.window(include=['frame'])
        self.assertEqual(r['effect'],'confirmed');self.assertEqual(r['post_action_observation']['state'],'stale')
    def test_bad_selection_is_rejected_before_mutation(self):
        for kw in [{'include':[]},{'include':['frame','frame']},{'include':['bad']},
                   {'include':['windows'],'crop':{}},{'include_image':True},
                   {'include':['frame'],'crop':{'x':True,'y':0,'width':10,'height':10}},
                   {'include':['frame'],'max_width':True},{'include':['errors'],'error_limit_bytes':False}]:
            with self.assertRaises(ValueError):self.window(**kw)
        self.assertEqual(self.fixture.mutations,0);self.assertEqual(self.calls,[])
    def test_cancelled_wait_skips_post_bundle_without_undoing_action(self):
        cancel=threading.Event();cancel.set();r=self.window(include=['frame'],_cancel=cancel)
        self.assertEqual(r['state'],'completed');self.assertEqual(r['post_action_observation']['state'],'cancelled')
        self.assertEqual(self.fixture.mutations,1);self.assertEqual(self.calls,[])
    def test_default_mcp_delivery_extracts_nested_action_frame(self):
        r=server.call_tool('vm_window',{'session':self.s,'window':self.fixture.ref,'action':'minimize',
            'request_id':self.ident,'wait_s':0,'include':['frame']})
        self.assertEqual([x['type'] for x in r['content']],['text'])
        text=r['content'][0]['text'];self.assertNotIn('data:image',text)
        data=json.loads(text);image=data['post_action_observation']['components']['frame']['result']['image_artifact']
        self.assertEqual((image['width'],image['height']),(2,1))
    def test_oversized_optional_preview_falls_back_without_losing_action(self):
        with patch.object(media,'MAX_PREVIEW_BYTES',16):
            r=server.call_tool('vm_window',{'session':self.s,'window':self.fixture.ref,'action':'minimize',
                'request_id':self.ident,'wait_s':0,'include':['frame'],'include_image':True})
        self.assertEqual([x['type'] for x in r['content']],['text'])
        data=json.loads(r['content'][0]['text']);self.assertEqual(data['state'],'completed')
        self.assertIn('preview_error',data['post_action_observation'])
        self.assertIn('image_artifact',data['post_action_observation']['components']['frame']['result'])
        self.assertEqual(self.fixture.mutations,1)
    def test_missing_optional_artifact_keeps_confirmed_dispatch_and_no_raw_bytes(self):
        self.remove_artifact=True
        r=server.call_tool('vm_window',{'session':self.s,'window':self.fixture.ref,'action':'minimize',
            'request_id':self.ident,'wait_s':0,'include':['frame']})
        data=json.loads(r['content'][0]['text']);self.assertEqual(data['effect'],'confirmed')
        self.assertTrue(data['wait_condition_met'])
        self.assertEqual(data['post_action_observation']['state'],'unavailable')
        self.assertIn('delivery_error',data['post_action_observation'])
        self.assertEqual(self.fixture.mutations,1)

    def test_explicit_preview_is_native_content_and_metadata_stays_reference_only(self):
        r=server.call_tool('vm_window',{'session':self.s,'window':self.fixture.ref,'action':'minimize',
            'request_id':self.ident,'wait_s':0,'include':['frame'],'include_image':True})
        self.assertEqual([x['type'] for x in r['content']],['image','text'])
        self.assertNotIn(r['content'][0]['data'],r['content'][1]['text'])
