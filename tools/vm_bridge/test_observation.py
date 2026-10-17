"""Bundle identity, partial results, bounded serial cursors and reference-first media."""
import contextlib
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from observation import Observations,StaleObservation,read_errors
from media import Delivery
import server


class ErrorCursorTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.path=Path(self.tmp.name)/'serial';self.path.write_bytes(b'normal\nerror: first\nnormal\nerror: second\n')
        self.data={'session':'a'*32,'generation':'g','serial_log':str(self.path)};self.boot='b'*32
    def read(self,cursor=None,**kw):return read_errors(self.data,self.boot,cursor,**kw)
    def test_event_limit_cursor_is_lossless_across_reads(self):
        first=self.read(limit_events=1);self.assertEqual(len(first['events']),1)
        self.assertTrue(first['more']);self.assertEqual(first['dropped_bytes'],0)
        second=self.read(first['next_cursor']);self.assertEqual(second['events'][0]['text'],'error: second')
        last=self.read(second['next_cursor']);self.assertEqual(last['events'],[]);self.assertFalse(last['more'])
    def test_unfinished_utf8_line_is_retained_until_newline(self):
        self.path.write_bytes(b'error: caf\xc3')
        r=self.read();self.assertEqual(r['events'],[]);self.assertEqual(r['next_cursor']['offset'],'0')
        with self.path.open('ab') as f:f.write(b'\xa9\n')
        r=self.read(r['next_cursor']);self.assertEqual(r['events'][0]['text'],'error: caf\xe9')
    def test_oversize_line_discards_its_suffix_with_explicit_loss(self):
        self.path.write_bytes(b'');cursor=self.read()['next_cursor']
        self.path.write_bytes(b'x'*2500+b'error: not a separate line\nerror: real\n')
        a=self.read(cursor,limit_bytes=1024);self.assertEqual(a['dropped_bytes'],1024)
        self.assertTrue(a['next_cursor']['discard_until_newline'])
        b=self.read(a['next_cursor'],limit_bytes=1024);self.assertEqual(b['events'],[])
        c=self.read(b['next_cursor'],limit_bytes=1024)
        self.assertEqual([x['text'] for x in c['events']],['error: real'])
        self.assertFalse(c['next_cursor']['discard_until_newline'])
    def test_actual_brook_user_fault_marker_is_not_classified_as_panic(self):
        self.path.write_bytes(b'[FAULT] User fault vec=14 err=0x4 rip=0x4012 pid=21\n')
        r=self.read();self.assertEqual(r['events'][0]['classification'],'user_fault')
    def test_rotation_ignores_previous_files_generation_offset(self):
        self.path.write_bytes(b'x'*4096+b'\nerror: before\n');self.data['generation_log_offset']=4097
        a=self.read();self.path.rename(self.path.with_suffix('.old'));self.path.write_bytes(b'error: replacement\n')
        b=self.read(a['next_cursor']);self.assertEqual(b['events'][0]['text'],'error: replacement')
        self.path.write_bytes(b'error: short\n')
        c=self.read(b['next_cursor']);self.assertEqual(c['events'][0]['text'],'error: short')

    def test_event_text_truncation_does_not_lose_following_event(self):
        self.path.write_bytes(b'error: '+b'x'*3000+b'\nerror: after\n')
        r=self.read();self.assertGreater(r['events'][0]['text_truncated_bytes'],0)
        self.assertEqual(r['events'][1]['text'],'error: after');self.assertEqual(r['dropped_bytes'],0)
    def test_rotation_truncation_and_same_inode_rewrite_are_explicit(self):
        a=self.read();self.path.rename(self.path.with_suffix('.old'));self.path.write_bytes(b'error: rotated\n')
        b=self.read(a['next_cursor']);self.assertEqual(b['reset_reason'],'file_rotated');self.assertIsNone(b['dropped_bytes'])
        self.path.write_bytes(b'error: truncate\n')
        c=self.read(b['next_cursor']);self.assertEqual(c['reset_reason'],'file_rewritten')
        self.path.write_bytes(b'x\n')
        d=self.read(c['next_cursor']);self.assertEqual(d['reset_reason'],'file_truncated')
    def test_initial_tail_is_bounded_and_reports_excluded_history(self):
        self.path.write_bytes(b'normal\n'*50000+b'error: last\n')
        r=self.read(limit_bytes=262144)
        self.assertGreater(r['history_excluded_bytes'],0);self.assertGreater(r['dropped_bytes'],0)
        self.assertEqual(r['events'][-1]['text'],'error: last')
    def test_initial_default_scan_starts_at_recent_tail_not_oldest_quarter(self):
        self.path.write_bytes(b'error: old\n'+b'normal\n'*15000+b'error: newest\n')
        r=self.read();self.assertEqual([x['text'] for x in r['events']],['error: newest'])
        self.assertGreater(r['history_excluded_bytes'],0)

    def test_boot_transition_keeps_log_continuity_but_reports_it(self):
        a=self.read();self.boot='c'*32
        with self.path.open('ab') as f:f.write(b'error: new boot\n')
        r=self.read(a['next_cursor']);self.assertTrue(r['guest_boot_changed']);self.assertEqual(len(r['events']),1)
    def test_malformed_or_foreign_cursor_and_limits_are_rejected(self):
        a=self.read()['next_cursor']
        for fields in [{'session':'c'*32},{'generation':'old'},{'offset':'-1'},{'anchor_sha256':'bad'}]:
            with self.assertRaises(ValueError):self.read({**a,**fields})
        for kw in [{'limit_bytes':True},{'limit_bytes':1023},{'limit_events':0},{'limit_events':129}]:
            with self.assertRaises(ValueError):self.read(**kw)


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.session='a'*32;self.boot='b'*32
        self.art=self.root/self.session/'artifacts';self.art.mkdir(parents=True)
        self.log=self.root/'serial';self.log.write_bytes(b'error: sample\n')
        self.data={'session':self.session,'generation':'g','state':'running','serial_log':str(self.log)}
        self.calls=[];self.panic=False;self.capture_hook=None
        test=self
        class Ui:
            read_retries=0
            @staticmethod
            def load_session(*a,**kw):return test.data
            @staticmethod
            def capabilities(*a):
                test.calls.append('caps');return test.data,{'observation_api':1,'frame_epoch_guard':1,'boot_id':test.boot}
            @staticmethod
            @contextlib.contextmanager
            def session_lock(*a,**kw):yield test.data
            @staticmethod
            def query(session,generation,command,deadline,boot_id=None):
                test.calls.append(command)
                if command=='bridge epoch':return {'boot_id':test.boot}
                if boot_id!=test.boot:raise RuntimeError('Stale guest boot')
                return copy.deepcopy(test.reply)
        self.reply={'observation_api':1,
            'windows':{'windows':[{'window_id':1,'incarnation':'123','client':{'pid':7,'incarnation':'456'},'provider':{'pid':9,'incarnation':'789'}}]},
            'processes':{'tasks':[{'pid':7,'incarnation':'456','tgid':7,'group_incarnation':'456','launch_id':'1'}]},
            'launches':{'launches':[{'token':'1','window':{'window_id':1,'incarnation':'123'}}]}}
        def capture(session,data,crop,max_width,boot_id):
            test.calls.append('capture')
            if test.capture_hook:test.capture_hook()
            png=server.png_rgb(2,1,b'\xff\0\0\0\xff\0');path=test.art/('d'*32+'.png');path.write_bytes(png)
            return {'image':png,'data':{'artifact':str(path),'frame_epoch':123,'guest_boot_id':boot_id}}
        self.observer=Observations(Ui(),capture,lambda d:self.panic)
    def collect(self,include,**kw):return self.observer.collect(self.session,include,**kw)
    def test_metadata_components_use_one_exchange_no_frame_and_bound_references(self):
        r=self.collect(['windows','processes','launches'])
        self.assertEqual(self.calls,['caps','bridge ui observe 7','bridge epoch'])
        self.assertEqual(r['state'],'completed');self.assertFalse(r['consistency']['atomic'])
        c=r['components'];ref=c['processes']['result']['tasks'][0]['reference']
        self.assertEqual(c['windows']['result']['windows'][0]['client']['reference'],ref)
        self.assertEqual(c['launches']['result']['launches'][0]['window_reference']['incarnation'],'123')
        self.assertTrue(all(x['age_at_bundle_end_s']>=0 for x in c.values()))
    def test_frame_and_metadata_survive_recursive_reference_delivery_without_base64(self):
        r=self.collect(['frame','windows','errors']);d=Delivery(self.root,self.session)
        text=json.dumps(d.adapt(r));self.assertNotIn('image":',text);self.assertFalse(d.images)
        self.assertEqual(len(d.descriptors),1)
        self.assertEqual(r['components']['frame']['result']['data']['guest_boot_id'],self.boot)
    def test_partial_allocation_failure_keeps_other_components(self):
        self.reply['processes']={'error':'Snapshot allocation failed'}
        r=self.collect(['windows','processes','errors']);self.assertEqual(r['state'],'partial')
        self.assertEqual(r['components']['windows']['state'],'ok');self.assertEqual(r['components']['errors']['state'],'ok')
        self.assertEqual(r['components']['processes']['state'],'unavailable')
    def test_reboot_between_components_marks_frame_stale(self):
        self.capture_hook=lambda:setattr(self,'boot','c'*32)
        r=self.collect(['frame','windows']);self.assertEqual(r['state'],'partial')
        self.assertEqual(r['components']['frame']['state'],'stale')
        self.assertEqual(r['components']['windows']['state'],'unavailable')
        self.assertFalse(r['identity_verified_at_end'])
    def test_panicked_guest_never_queries_guest_transport(self):
        self.panic=True;r=self.collect(['frame','windows','errors'])
        self.assertEqual(self.calls,['capture']);self.assertEqual(r['state'],'partial')
        self.assertEqual(r['components']['frame']['state'],'unverified');self.assertEqual(r['components']['windows']['state'],'unavailable')
    def test_errors_only_reports_unreachable_guest_identity_explicitly(self):
        def unavailable(*args):raise TimeoutError('Guest disconnected')
        self.observer.ui.capabilities=unavailable
        r=self.collect(['errors']);self.assertEqual(r['components']['errors']['state'],'ok')
        self.assertEqual(r['guest_status']['state'],'unavailable');self.assertEqual(r['state'],'partial')
        self.assertIn('Guest disconnected',r['identity_error'])

    def test_missing_log_during_panic_check_becomes_partial_components(self):
        def missing(data):raise FileNotFoundError('serial rotating')
        self.observer.panic_state=missing
        r=self.collect(['frame','windows','errors']);self.assertEqual(r['state'],'partial')
        self.assertEqual(r['components']['errors']['state'],'ok');self.assertEqual(self.calls,[])
    def test_errors_only_bundle_is_partial_after_identity_changed(self):
        original=self.observer.ui.query
        def changed(session,generation,command,deadline,**kw):
            if command=='bridge epoch':return {'boot_id':'c'*32}
            return original(session,generation,command,deadline,**kw)
        self.observer.ui.query=changed
        r=self.collect(['errors']);self.assertEqual(r['components']['errors']['state'],'ok')
        self.assertEqual(r['state'],'partial');self.assertIn('identity_error',r)

    def test_stopped_guest_reads_errors_without_capture_or_guest_calls(self):
        self.data['state']='stopped';r=self.collect(['frame','processes','errors'])
        self.assertEqual(self.calls,[]);self.assertEqual(r['components']['errors']['state'],'ok')
    def test_changed_host_generation_marks_returned_metadata_stale(self):
        original=self.observer.references
        def refs(*args):original(*args);self.data['generation']='new'
        self.observer.references=refs
        r=self.collect(['windows']);self.assertEqual(r['components']['windows']['state'],'stale')
    def test_invalid_arguments_do_not_capture_or_query(self):
        for include,kw in [([],{}),(['windows','windows'],{}),(['bad'],{}),(['windows'],{'crop':{}}),(['errors'],{'error_limit_events':True}),(['frame'],{'cursor':{}})]:
            with self.assertRaises(ValueError):self.collect(include,**kw)
        self.assertEqual(self.calls,[])
    def test_action_expected_generation_is_checked_before_guest_queries(self):
        with self.assertRaises(StaleObservation):self.collect(['frame'],expected_generation='old')
        self.assertEqual(self.calls,[])
    def test_action_expected_boot_is_checked_before_frame_or_metadata_work(self):
        with self.assertRaises(StaleObservation):self.collect(['frame','windows'],expected_boot_id='c'*32)
        self.assertEqual(self.calls,['caps'])
    def test_action_expected_identity_is_retained_in_result(self):
        r=self.collect(['windows'],expected_generation='g',expected_boot_id=self.boot)
        self.assertEqual(r['expected_identity'],{'generation':'g','guest_boot_id':self.boot})

    def test_frame_only_legacy_shape_remains_available(self):
        @contextlib.contextmanager
        def lock(s):yield self.data
        with patch.object(server,'session_lock',lock),patch.object(server,'capture',return_value={'image':b'old','data':{}}):
            self.assertEqual(server.observe(self.session)['image'],b'old')
