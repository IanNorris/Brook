"""Contract tests: MCP wire protocol, framing, input cleanup, and image/audio data."""
import contextlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

import common
import server


class ProfileLossTests(unittest.TestCase):
    def test_producer_drops_are_visible_despite_complete_export(self):
        result = server.profile_event_summary(
            'PROF_BEGIN 8 100\nP 0 1 0 0 ffff\nCS 1 0 1 2\nPROF_END 2 725094\n')
        self.assertEqual(result['dropped_event_count'], 725094)
        self.assertEqual(result['sample_count'], 1)
        self.assertEqual(result['context_switch_count'], 1)
        self.assertTrue(result['event_count_matches'])
        self.assertTrue(result['lossy'])

    def test_missing_records_are_distinct_from_producer_drops(self):
        result = server.profile_event_summary('PROF_BEGIN 1 0\nP 0 1 0 1 1000\nPROF_END 2 0\n')
        self.assertEqual(result['dropped_event_count'], 0)
        self.assertFalse(result['event_count_matches'])
        self.assertTrue(result['lossy'])

    def test_complete_lossless_capture(self):
        result = server.profile_event_summary('PROF_BEGIN 1 0\nP 0 1 0 1 1000\nPROF_END 1 0\n')
        self.assertFalse(result['lossy'])

    def test_incomplete_reversed_or_multiple_captures_rejected(self):
        for text in ('PROF_BEGIN 1 0\n', 'PROF_END 0 0\nPROF_BEGIN 1 0\n',
                     'PROF_BEGIN 1 0\nPROF_END 0 0\n' * 2):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                server.profile_event_summary(text)


class ProfilePathTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / 'serial.log'

    def test_export_reference_survives_long_compiler_output(self):
        self.log.write_bytes(b'PROFILER: writing to /boot/PROF_123.TXT\r\n'
                             + b'compiler output\n' * 100000)
        self.assertNotIn('PROFILER:', server.tail(self.log))
        self.assertEqual(server.latest_profile_path(self.log), '/boot/PROF_123.TXT')

    def test_latest_complete_capture_wins(self):
        self.log.write_bytes(b'PROFILER: writing to /boot/PROF_1.TXT\n'
                             + b'x' * 200000
                             + b'\nPROFILER: writing to /boot/PROF_2.TXT\n')
        self.assertEqual(server.latest_profile_path(self.log), '/boot/PROF_2.TXT')

    def test_filename_crossing_read_boundary_is_complete(self):
        record = b'PROFILER: writing to /boot/PROF_123.TXT\r\n'
        # Exercise splits in both the prefix and filename/newline.
        for split in range(1, len(record)):
            with self.subTest(split=split):
                self.log.write_bytes(b'prefix\n' + record + b'x' * (65536 - split))
                self.assertEqual(server.latest_profile_path(self.log), '/boot/PROF_123.TXT')

    def test_previous_generation_cannot_supply_capture(self):
        old = b'PROFILER: writing to /boot/PROF_OLD.TXT\n'
        self.log.write_bytes(old + b'new boot without a capture\n')
        with self.assertRaisesRegex(RuntimeError, 'this guest generation'):
            server.latest_profile_path(self.log, len(old))
        with self.log.open('ab') as log:
            log.write(b'PROFILER: writing to /boot/PROF_NEW.TXT\n')
        self.assertEqual(server.latest_profile_path(self.log, len(old)), '/boot/PROF_NEW.TXT')

    def test_partial_filename_is_not_exportable(self):
        self.log.write_bytes(b'PROFILER: writing to /boot/PROF_12')
        with self.assertRaises(RuntimeError):
            server.latest_profile_path(self.log)


class CpuSelectionTests(unittest.TestCase):
    def test_hmp_request_carries_cpu_for_every_command(self):
        qmp = object.__new__(common.QMP)
        with patch.object(qmp, "call", return_value="registers") as call:
            self.assertEqual(qmp.hmp("info registers", cpu=3), "registers")
            call.assert_called_once_with("human-monitor-command",
                                         {"command-line": "info registers", "cpu-index": 3})


class ShutdownSyncTests(unittest.TestCase):
    @contextlib.contextmanager
    def fixture(self, reply=None, error=None):
        events = []
        class FakeQMP:
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def call(self, command): events.append(command)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 's').mkdir()
            log = root / 'serial.log'
            log.write_text('old boot\n')
            data = {'generation': 'before', 'serial_log': str(log)}
            @contextlib.contextmanager
            def lock(_): yield data
            def guest(_, command, timeout):
                events.append(command)
                self.assertEqual(timeout, 30)
                if error is not None: raise error
                return reply
            def reset(session,mutable,registry,guest_fn,qmp_factory,wait_s,cancel):
                mutable['generation']='after';common.write_json(registry/session/'session.json',mutable)
                with qmp_factory(session) as q:q.call('system_reset')
                return {'restart_verified':True}
            with patch.object(server, 'REGISTRY', root), \
                 patch.object(server, 'session_lock', lock), \
                 patch.object(server, 'guest', guest), \
                 patch.object(server.reset_control,'perform',reset), \
                 patch.object(server, 'QMP', lambda _: FakeQMP()):
                yield events, data, root

    def test_stop_and_reset_wait_for_sync(self):
        for action, qmp in [('stop', 'quit'), ('reset', 'system_reset')]:
            with self.subTest(action=action), self.fixture({'guest_sync_completed': True}) as (events, data, root):
                result = server.control('s', action)
                self.assertEqual(events, ['bridge sync', qmp])
                self.assertIs(result['guest_sync_completed'], True)
                if action == 'reset':
                    self.assertNotEqual(data['generation'], 'before')
                    self.assertTrue((root / 's' / 'session.json').exists())

    def test_missing_or_negative_ack_blocks_qmp(self):
        for reply in [None, [], {}, {'launched': True}, {'guest_sync_completed': False},
                      {'guest_sync_completed': 1}]:
            for action in ['stop', 'reset']:
                with self.subTest(reply=reply, action=action), self.fixture(reply) as (events, data, _):
                    with self.assertRaisesRegex(RuntimeError, 'aborted before QMP'):
                        server.control('s', action)
                    self.assertEqual(events, ['bridge sync'])
                    self.assertEqual(data['generation'], 'before')

    def test_preserves_guest_sync_guarantee(self):
        for guarantee in ['best_effort_metadata_flush', 'checked_filesystem_and_device_flush']:
            with self.subTest(guarantee=guarantee), self.fixture({
                    'guest_sync_completed': True, 'sync_guarantee': guarantee}) as (events, _, _):
                result = server.control('s', 'stop')
                self.assertEqual(result['sync_guarantee'], guarantee)
                self.assertEqual(events, ['bridge sync', 'quit'])
        with self.fixture({'guest_sync_completed': True}) as (_, _, _):
            self.assertEqual(server.control('s', 'stop')['sync_guarantee'],
                             'best_effort_metadata_flush')

    def test_unreachable_or_panicked_guest_requires_explicit_force(self):
        for error in [TimeoutError('unknown completion'), ConnectionError('unreachable'),
                      RuntimeError('Guest has panicked')]:
            for action, qmp in [('stop', 'quit'), ('reset', 'system_reset')]:
                with self.subTest(error=error, action=action), self.fixture(error=error) as (events, _, _):
                    with self.assertRaisesRegex(RuntimeError, 'force=true'):
                        server.control('s', action)
                    self.assertEqual(events, ['bridge sync'])
                    events.clear()
                    result = server.control('s', action, force=True)
                    self.assertEqual(events, ['bridge sync', qmp])
                    self.assertIs(result['guest_sync_completed'], False)
                    self.assertIs(result['forced'], True)
                    self.assertEqual(result['sync_error'], str(error))

    def test_force_still_syncs_reachable_guest(self):
        with self.fixture({'guest_sync_completed': True}) as (events, _, _):
            result = server.control('s', 'stop', force=True)
            self.assertEqual(events, ['bridge sync', 'quit'])
            self.assertIs(result['guest_sync_completed'], True)

    def test_pause_resume_do_not_require_guest(self):
        with self.fixture(error=AssertionError('unexpected guest request')) as (events, _, _):
            server.control('s', 'pause')
            server.control('s', 'resume')
            self.assertEqual(events, ['stop', 'cont'])

    def test_force_is_typed_and_limited_to_shutdown(self):
        with patch.object(server, 'session_lock') as lock:
            for force in ['true', 1, None]:
                with self.assertRaises(ValueError):
                    server.call_tool('vm_control', {'session': 's', 'action': 'stop', 'force': force})
            with self.assertRaises(ValueError):
                server.control('s', 'pause', force=True)
            lock.assert_not_called()


class Contracts(unittest.TestCase):
    def test_png_pixels(self):
        png=server.png_rgb(2,1,b'\xff\0\0\0\xff\0')
        self.assertEqual(png[:8],b'\x89PNG\r\n\x1a\n')
        pos=8;compressed=b''
        while pos<len(png):
            size=struct.unpack('>I',png[pos:pos+4])[0]
            if png[pos+4:pos+8]==b'IDAT':compressed+=png[pos+8:pos+8+size]
            pos+=12+size
        self.assertEqual(zlib.decompress(compressed),b'\0\xff\0\0\0\xff\0')

    def test_text_preflight(self):
        self.assertEqual(server.text_keys('A!\n'),[('a',True),('1',True),('ret',False)])
        with self.assertRaises(ValueError):server.text_keys('hello café')

    def test_identity_rejects_escape(self):
        for bad in ['../other','f'*31,'g'*32, '/tmp/x']:
            with self.assertRaises(ValueError):common.load_session(bad)

    def test_guest_fragmented_response(self):
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen()
        seen=[]
        def endpoint():
            connection,_=listener.accept()
            with connection:
                request=connection.recv(256).decode();seen.append(request)
                token=request.split(' ',1)[0][1:]
                reply=f'noise\r\nBRIDGE_BEGIN {token}\n{{"pid":42}}\nBRIDGE_END {token}\n'.encode()
                for i in range(0,len(reply),3):connection.sendall(reply[i:i+3])
        thread=threading.Thread(target=endpoint);thread.start()
        try:
            with patch.object(server,'panic_state',return_value=False):
                value=server.guest({'guest_debug_port':listener.getsockname()[1]},'bridge stats')
            self.assertEqual(value,{'pid':42});self.assertEqual(len(seen),1)
        finally:thread.join();listener.close()

    def test_census_keeps_records_and_requires_footer(self):
        header={'kind':'memory_census','identity_ceiling':42}
        records=[{'as_id':7,'mapping_kind':'image','backed_bytes':8192},
                 {'as_id':8,'mapping_kind':'anonymous','backed_bytes':4096}]
        footer={'snapshot':'rolling','spaces':2}
        wire='noise\r\n'+'\n'.join(json.dumps(v) for v in [header,*records,footer])+'\n'
        result=server.parse_guest_reply(wire)
        self.assertEqual(result['records'],records)
        self.assertEqual(result['summary'],footer)
        self.assertEqual(result['identity_ceiling'],42)
        with self.assertRaisesRegex(ValueError,'Incomplete memory census'):
            server.parse_guest_reply(json.dumps(header)+'\n'+json.dumps(records[0]))
        self.assertEqual(server.parse_guest_reply('ordinary reply\n'),{'reply':'ordinary reply'})

    def test_stats_census_is_opt_in(self):
        data={'guest_debug_port':1234}
        with patch.object(server,'session_lock',return_value=contextlib.nullcontext(data)), \
             patch.object(server,'guest',return_value={'used_memory_bytes':4096}) as request:
            result=server.stats('fixture')
            self.assertNotIn('memory_census',result)
            request.assert_called_once_with(data,'bridge stats')
        census={'kind':'memory_census','records':[],'summary':{'spaces':0}}
        with patch.object(server,'session_lock',return_value=contextlib.nullcontext(data)), \
             patch.object(server,'guest',side_effect=[{'used_memory_bytes':4096},census]) as request:
            result=server.stats('fixture',memory_details=True)
            self.assertEqual(result['memory_census'],census)
            self.assertTrue(result['memory_census']['supported'])
            self.assertGreaterEqual(result['memory_census']['round_trip_ms'],0)
            self.assertEqual(request.call_args_list[1].args,(data,'mem census'))
            self.assertEqual(request.call_args_list[1].kwargs,{'timeout':30})
        with patch.object(server,'session_lock',return_value=contextlib.nullcontext(data)), \
             patch.object(server,'guest',side_effect=[{}, {'reply':'Unknown command'}]):
            self.assertFalse(server.stats('fixture',memory_details=True)['memory_census']['supported'])

    def test_stats_audio_is_opt_in_and_handles_older_guests(self):
        data={'guest_debug_port':1234}
        with patch.object(server,'session_lock',return_value=contextlib.nullcontext(data)), \
             patch.object(server,'guest',return_value={}) as request:
            self.assertNotIn('audio',server.stats('fixture'))
            request.assert_called_once_with(data,'bridge stats')
        audio={'guest_clock_ns':123456,'streams':[], 'clocked':1}
        with patch.object(server,'session_lock',return_value=contextlib.nullcontext(data)), \
             patch.object(server,'guest',side_effect=[{},audio]) as request:
            result=server.stats('fixture',audio_details=True)
            self.assertTrue(result['audio']['supported'])
            self.assertEqual(result['audio']['guest_clock_ns'],123456)
            self.assertEqual(result['audio']['streams'],[])
            self.assertGreaterEqual(result['audio']['round_trip_ms'],0)
            self.assertEqual(request.call_args_list[1].args,(data,'audio stats'))
        with patch.object(server,'session_lock',return_value=contextlib.nullcontext(data)), \
             patch.object(server,'guest',side_effect=[{}, {'reply':'Unknown command'}]):
            result=server.stats('fixture',audio_details=True)
            self.assertFalse(result['audio']['supported'])
            self.assertEqual(result['audio']['reply'],'Unknown command')

    def test_holds_released_on_cancel(self):
        events=[];cancel=threading.Event()
        class FakeQMP:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def call(self,name,args=None):
                if name=='query-qmp-schema':
                    return [{'name':'784','meta-type':'enum','members':[{'name':x} for x in ['ret','ctrl','shift']]}]
                events.extend(args['events'])
                if events[-1]['data'].get('down'):cancel.set()
                return {}
        @contextlib.contextmanager
        def lock(_):yield {'generation':'g'}
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'s').mkdir()
            with patch.object(server,'REGISTRY',root),patch.object(server,'session_lock',lock),patch.object(server,'panic_state',return_value=False),patch.object(server,'QMP',lambda _:FakeQMP()):
                with self.assertRaisesRegex(RuntimeError,'cancelled'):
                    server.input_actions('s',[{'type':'key_down','key':'CTRL'},{'type':'wait','seconds':1}],_cancel=cancel)
        self.assertEqual([e['data']['down'] for e in events],[True,False])

    def test_invalid_later_action_never_sends(self):
        with patch.object(server,'session_lock') as lock:
            with self.assertRaises(ValueError):
                server.input_actions('s',[{'type':'keypress','keys':['a']},{'type':'scroll','dy':500}])
            lock.assert_not_called()

    def test_typed_holds_keep_shift_and_release_on_cancel(self):
        clock=[0.0];edges=[];cancel=threading.Event()
        class FakeQMP:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def call(self,name,args=None):
                if name=='query-qmp-schema':
                    return [{'meta-type':'enum','members':[{'name':x} for x in ['ret','ctrl','shift','a','b']]}]
                for event in args['events']:
                    if event['type']=='key':edges.append((clock[0],event['data']['key']['data'],event['data']['down']))
                return {}
        @contextlib.contextmanager
        def lock(_):yield {}
        def sleep(seconds):clock[0]+=seconds
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'s').mkdir()
            with patch.object(server,'REGISTRY',root),patch.object(server,'session_lock',lock), \
                 patch.object(server,'panic_state',return_value=False),patch.object(server,'QMP',lambda _:FakeQMP()), \
                 patch.object(server.time,'monotonic',side_effect=lambda:clock[0]),patch.object(server.time,'sleep',side_effect=sleep):
                server.input_actions('s',[{'type':'type','text':'Ab','hold_seconds':.15}])
                self.assertEqual([(k,d) for _,k,d in edges],
                                 [('shift',True),('a',True),('a',False),('shift',False),('b',True),('b',False)])
                self.assertAlmostEqual(edges[2][0]-edges[1][0],.15)
                self.assertAlmostEqual(edges[5][0]-edges[4][0],.15)
                edges.clear()
                def interrupt(seconds):
                    clock[0]+=seconds;cancel.set()
                with patch.object(server.time,'sleep',side_effect=interrupt):
                    with self.assertRaisesRegex(RuntimeError,'cancelled'):
                        server.input_actions('s',[{'type':'type','text':'Ab','hold_seconds':.15}],_cancel=cancel)
                self.assertEqual({k for _,k,d in edges if d},{'shift','a'})
                self.assertEqual({k for _,k,d in edges if not d},{'shift','a'})

    def test_typed_hold_budget_rejected_before_input(self):
        with patch.object(server,'session_lock') as lock:
            for actions in ([{'type':'type','text':'a'*201,'hold_seconds':.1}],
                            [{'type':'wait','seconds':5}]*3+[{'type':'type','text':'a'*300}]):
                with self.subTest(actions=actions),self.assertRaisesRegex(ValueError,'holds exceed'):
                    server.input_actions('s',actions)
            for value in [True,0,float('nan'),float('inf'),2]:
                with self.subTest(value=value),self.assertRaises(ValueError):
                    server.input_actions('s',[{'type':'type','text':'a','hold_seconds':value}])
            lock.assert_not_called()

    def test_schema_rejects_boolean_number(self):
        with self.assertRaises(ValueError):
            server.call_tool('vm_logs',{'session':'f'*32,'offset':True})

    def test_reset_history_does_not_poison_new_boot(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'serial.log'
            path.write_text('KERNEL PANIC old boot\n')
            data={'serial_log':str(path),'generation_log_offset':path.stat().st_size}
            with path.open('a') as log:log.write('new boot ready\n')
            self.assertFalse(server.panic_state(data))
            with path.open('a') as log:log.write('KERNEL FAULT new boot\n')
            self.assertTrue(server.panic_state(data))

    def test_memory_error_restores_running_state(self):
        calls=[]
        class FakeQMP:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def call(self,name,args=None):
                calls.append(name)
                if name=='query-cpus-fast':return [{'cpu-index':0}]
                if name=='query-status':return {'running':True}
                return {}
            def hmp(self,command,cpu=None):
                if command=='info mtree -f':return 'FlatView #6\n AS "memory", root: system\n0000000000000000-000000000000ffff (prio 0, ram)'
                if command.startswith('gva2gpa'):return 'unmapped'
                return ''
        @contextlib.contextmanager
        def lock(_):yield {}
        with patch.object(server,'session_lock',lock),patch.object(server,'QMP',lambda _:FakeQMP()):
            with self.assertRaisesRegex(ValueError,'cannot be translated'):
                server.debug('s',action='virtual_memory',address='0x1000')
        self.assertEqual(calls[-2:],['stop','cont'])

    def test_ram_map_selects_system_view_and_excludes_mmio(self):
        tree='''FlatView #0
 AS "device-memory", root: device
  0000000000000000-000000000000ffff (prio 0, ram)
FlatView #6
 AS "memory", root: system
  0000000000100000-000000007fffffff (prio 0, ram): pc.ram
  0000000080840000-0000000080841fff (prio 0, i/o): intel-hda
FlatView #7
 AS "device-memory", root: device
  0000000000000000-000000000000ffff (prio 0, ram)
'''
        self.assertEqual(server.ram_ranges(tree),[('0000000000100000','000000007fffffff')])

    def test_scaled_crop_pointer_maps_back_to_guest(self):
        events=[]
        class FakeQMP:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def call(self,name,args=None):
                if name=='query-qmp-schema':
                    return [{'meta-type':'enum','members':[{'name':x} for x in ['ret','ctrl','shift']]}]
                events.extend(args['events']);return {}
        @contextlib.contextmanager
        def lock(_):yield {'generation':'g'}
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'s').mkdir()
            (root/'s'/'last-frame.json').write_text(json.dumps({'frame_id':'f','width':100,'height':50,
                'crop_width':200,'crop_height':100,'crop_origin':[400,100],
                'coordinate_width':1001,'coordinate_height':501}))
            with patch.object(server,'REGISTRY',root),patch.object(server,'session_lock',lock),patch.object(server,'panic_state',return_value=False),patch.object(server,'QMP',lambda _:FakeQMP()):
                server.input_actions('s',[{'type':'move','x':50,'y':25}],frame_id='f')
        self.assertEqual([e['data']['value'] for e in events],[round(500*32767/1000),round(150*32767/500)])

    def test_click_spans_guest_poll_interval_and_allows_explicit_fast_pulse(self):
        # Model a guest that samples button state at 100 ms intervals. The old
        # 20 ms pulse vanished between polls even though QMP accepted both events.
        clock=[0.0];edges=[]
        class FakeQMP:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def call(self,name,args=None):
                if name=='query-qmp-schema':
                    return [{'meta-type':'enum','members':[{'name':x} for x in ['ret','ctrl','shift']]}]
                for event in args['events']:
                    if event['type']=='btn':edges.append((clock[0],event['data']['down']))
                return {}
        @contextlib.contextmanager
        def lock(_):yield {}
        def sleep(seconds):clock[0]+=seconds
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'s').mkdir()
            (root/'s'/'last-frame.json').write_text(json.dumps({'frame_id':'f','width':100,'height':50,
                'crop_origin':[0,0],'coordinate_width':100,'coordinate_height':50}))
            with patch.object(server,'REGISTRY',root),patch.object(server,'session_lock',lock),patch.object(server,'panic_state',return_value=False),patch.object(server,'QMP',lambda _:FakeQMP()),patch.object(server.time,'monotonic',lambda:clock[0]),patch.object(server.time,'sleep',sleep):
                server.input_actions('s',[{'type':'click','x':50,'y':25}],frame_id='f')
                self.assertTrue(edges[0][0] <= .1 < edges[1][0])
                edges.clear();clock[0]=0
                server.input_actions('s',[{'type':'click','x':50,'y':25,'hold_seconds':.03}],frame_id='f')
                self.assertFalse(edges[0][0] <= .1 < edges[1][0])
                self.assertAlmostEqual(edges[1][0]-edges[0][0],.03)

    def test_invalid_click_hold_is_rejected_before_delivery(self):
        with patch.object(server,'session_lock') as lock:
            for value in [True,float('nan'),0,2]:
                with self.subTest(value=value),self.assertRaises(ValueError):
                    server.input_actions('s',[{'type':'click','x':1,'y':1,'hold_seconds':value}])
            lock.assert_not_called()

    def test_mcp_stdio(self):
        process=subprocess.Popen([sys.executable,str(Path(server.__file__))],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            def call(method,params):
                process.stdin.write(json.dumps({'jsonrpc':'2.0','id':1,'method':method,'params':params})+'\n');process.stdin.flush()
                return json.loads(process.stdout.readline())
            reply=call('initialize',{'protocolVersion':'2025-03-26','capabilities':{},'clientInfo':{'name':'test','version':'1'}})
            self.assertEqual(reply['result']['protocolVersion'],'2025-03-26')
            tools=call('tools/list',{})['result']['tools']
            self.assertTrue({'vm_stats','vm_guest_run','vm_profile','vm_observe'}.issubset({t['name'] for t in tools}))
            result=call('tools/call',{'name':'vm_stats','arguments':{'session':'invalid'}})
            self.assertTrue(result['result']['isError'])
            process.stdin.write('[]\n');process.stdin.flush()
            self.assertEqual(json.loads(process.stdout.readline())['error']['code'],-32600)
            self.assertEqual(call('ping',{})['result'],{})
        finally:
            process.stdin.close();process.wait(timeout=10)
            error=process.stderr.read();process.stdout.close();process.stderr.close()
            self.assertEqual(process.returncode,0,error)




class CompletedFrameTests(unittest.TestCase):
    def ready(self, **changes):
        return dict(capture_api=2, token=7, state=2, address='0x1000',
                    width=2, height=1, stride=2, frame_epoch=41,
                    frame_tick_ms=123, panic_address='0x2000', panic_stride=2,
                    **changes)

    def test_waits_for_completed_frame_and_releases_it(self):
        ready = self.ready()
        replies = [dict(capture_api=2, token=7, state=1), ready, ready,
                   dict(released=True)]
        with patch.object(server, 'guest', side_effect=replies) as guest, \
             patch.object(server.time, 'sleep'):
            with server.completed_frame_source('s', {}) as source:
                self.assertEqual(source['frame_epoch'], 41)
            self.assertEqual([call.args[1] for call in guest.call_args_list],
                             ['bridge frame request', 'bridge frame status 7',
                              'bridge frame status 7', 'bridge frame release 7'])

    def test_bundle_frame_leases_are_boot_guarded_including_release(self):
        boot='a'*32
        with patch.object(server,'guest',side_effect=[self.ready(),self.ready(),{'released':True}]) as guest:
            with server.completed_frame_source('s',{},boot_id=boot):pass
        self.assertEqual([x.args[1] for x in guest.call_args_list],[
            f'bridge frame guarded {boot} request',f'bridge frame guarded {boot} status 7',f'bridge frame guarded {boot} release 7'])

    def test_reboot_cannot_release_a_new_boots_reused_frame_token(self):
        with patch.object(server,'guest',side_effect=[self.ready(),{'frame_error':'Stale guest boot'},
                {'frame_error':'Stale guest boot'}]) as guest:
            with self.assertRaisesRegex(RuntimeError,'Stale guest boot'):
                with server.completed_frame_source('s',{},boot_id='a'*32):pass
        self.assertTrue(all('guarded '+('a'*32) in x.args[1] for x in guest.call_args_list))

    def test_refuses_mutable_legacy_frame(self):
        with patch.object(server, 'guest', return_value={'address': '0x1000'}):
            with self.assertRaisesRegex(RuntimeError, 'lacks completed-frame'):
                with server.completed_frame_source('s', {}):
                    self.fail('legacy buffer must never be read')

    def test_does_not_steal_an_existing_capture(self):
        with patch.object(server, 'guest', return_value=dict(capture_api=2, token=0, state=4)) as guest:
            with self.assertRaisesRegex(RuntimeError, 'busy or unavailable'):
                with server.completed_frame_source('s', {}):
                    self.fail('busy capture must not be read')
            self.assertEqual(guest.call_count, 1)

    def test_expired_lease_is_not_publishable(self):
        with patch.object(server, 'guest', side_effect=[self.ready(),
                dict(capture_api=2, token=7, state=5), dict(released=False)]) as guest:
            with self.assertRaisesRegex(RuntimeError, 'expired during capture'):
                with server.completed_frame_source('s', {}):
                    pass
            self.assertEqual(guest.call_args_list[-1].args[1], 'bridge frame release 7')

    def test_release_after_read_failure_preserves_original_error(self):
        with patch.object(server, 'guest', side_effect=[self.ready(), OSError('disconnected')]) as guest:
            with self.assertRaisesRegex(ValueError, 'read failed'):
                with server.completed_frame_source('s', {}):
                    raise ValueError('read failed')
            self.assertEqual(guest.call_args_list[-1].args[1], 'bridge frame release 7')

    def test_wrong_token_while_pending_is_rejected_and_released(self):
        with patch.object(server, 'guest', side_effect=[dict(capture_api=2, token=7, state=1),
                dict(capture_api=2, token=8, state=2), dict(released=True)]), \
             patch.object(server.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'token changed'):
                with server.completed_frame_source('s', {}):
                    self.fail('wrong token must not be read')

    def test_panic_capture_does_not_reuse_old_frame_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'s').mkdir()
            (root/'s'/'frame-source.json').write_text(json.dumps(self.ready()))
            (root/'s'/'artifacts').mkdir()
            image_path=root/'s'/'artifacts'/('d'*32+'.png')
            with patch.object(server, 'REGISTRY', root), \
                 patch.object(server, 'panic_state', return_value=True), \
                 patch.object(server, 'artifact', return_value=image_path), \
                 patch.object(server, 'read_frame_bytes', return_value=bytes(8)) as read:
                result=server.capture('s',{'generation':'g'})['data']
            self.assertTrue(result['panic'])
            self.assertIsNone(result['frame_epoch'])
            self.assertIsNone(result['frame_tick_ms'])
            self.assertEqual(read.call_args.args[1]['address'],'0x2000')

    def test_panic_geometry_from_known_other_host_generation_is_not_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'s').mkdir()
            (root/'s'/'frame-source.json').write_text(json.dumps(self.ready(session_generation='old')))
            with patch.object(server,'REGISTRY',root),patch.object(server,'panic_state',return_value=True), \
                 patch.object(server,'read_frame_bytes') as read:
                with self.assertRaisesRegex(RuntimeError,'another host generation'):
                    server.capture('s',{'generation':'new'})
                read.assert_not_called()

    def test_memsave_error_resumes_vm_and_removes_partial_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'partial.xrgb'
            events = []
            class FakeQMP:
                def __init__(self, *args, **kwargs): pass
                def __enter__(self): return self
                def __exit__(self, *args): pass
                def call(self, command):
                    events.append(command)
                    return {'running': True} if command == 'query-status' else {}
                def hmp(self, command):
                    path.write_bytes(b'partial')
                    raise RuntimeError('memsave failed')
            with patch.object(server, 'QMP', FakeQMP), patch.object(server, 'artifact', return_value=path):
                with self.assertRaisesRegex(RuntimeError, 'memsave failed'):
                    server.read_frame_bytes('s', self.ready())
            self.assertEqual(events, ['query-status', 'stop', 'cont'])
            self.assertFalse(path.exists())


if __name__=='__main__':unittest.main()
