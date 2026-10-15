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
            with patch.object(server, 'REGISTRY', root), \
                 patch.object(server, 'session_lock', lock), \
                 patch.object(server, 'guest', guest), \
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


if __name__=='__main__':unittest.main()
