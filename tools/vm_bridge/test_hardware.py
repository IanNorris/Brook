import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

from hardware import Hardware,HardwareBootChanged

class Endpoint:
    def __init__(self):
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen();self.listener.settimeout(.1)
        self.port=self.listener.getsockname()[1];self.epoch='0'*32;self.stop=False;self.effects=0;self.commands=[];self.power_requests={};self.power_effects=0;self.power_drop=False
        self.thread=threading.Thread(target=self.serve);self.thread.start()
    def serve(self):
        while not self.stop:
            try:c,_=self.listener.accept()
            except socket.timeout:continue
            except OSError:break
            with c:
                c.settimeout(1);f=c.makefile('rb')
                while True:
                    try:line=f.readline()
                    except (OSError,ValueError):break
                    if not line:break
                    token,command=line.decode().rstrip('\n').split(' ',1);token=token[1:]
                    self.commands.append(command)
                    if command.startswith('bridge guard '):
                        _,_,boot,command=command.split(' ',3)
                        if boot!=self.epoch:
                            result={'boot_error':'Guest boot changed'};command=None
                    if command is not None:
                        if command.startswith('bridge epoch init '):
                            if self.epoch=='0'*32:self.epoch=command.split()[-1]
                            result={'boot_id':self.epoch}
                        elif command=='bridge hardware caps':result={'hardware_api':1,'boot_guard':True,'logs_api':1,'boot_id':self.epoch,'power_api':1,'power':{'graceful_quiescence':True,'reset':True,'power_off':False}}
                        elif command=='bridge job caps':result={'job_api':2}
                        elif command=='bridge ui caps':result={'ui_api':1,'apps_api':1,'boot_id':self.epoch}
                        elif command=='bridge stats':result={'process_count':7}
                        elif command.startswith('bridge hardware power '):
                            _,_,_,request,verb,boot=command.split()
                            if boot!=self.epoch:result={'boot_error':'Guest boot changed'}
                            else:
                                if verb in ('shutdown','restart') and request not in self.power_requests:
                                    self.power_requests[request]='ready';self.power_effects+=1
                                if self.power_drop and verb in ('shutdown','restart'):
                                    self.power_drop=False;break
                                result={'power_api':1,'request_id':request,'operation':verb,'state':self.power_requests.get(request,'missing'),'error':-95 if verb=='commit' else 0}
                        elif command.startswith('bridge hardware logs '):result={'logs_api':1,'lines':['hello "world"'],'next_cursor':2,'dropped_lines':1,'more':False}
                        elif command=='effect':self.effects+=1;break
                        else:result={'reply':command}
                    response=f'BRIDGE_BEGIN {token}\n{json.dumps(result)}\nBRIDGE_END {token}\n'.encode()
                    try:
                        for i in range(0,len(response),7):c.sendall(response[i:i+7])
                    except OSError:break
                f.close()
    def close(self):
        self.stop=True;self.listener.close();self.thread.join(3)

class HardwareTests(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.endpoint=Endpoint();self.hardware=Hardware(self.tmp.name)
    def tearDown(self):self.endpoint.close();self.tmp.cleanup()
    def attach(self):return self.hardware.attach('127.0.0.1',self.endpoint.port)['session']
    def test_hardware_does_not_need_vm_or_qmp(self):
        with patch('common.process_identity',side_effect=AssertionError('No VM process')):
            session=self.attach();doctor=self.hardware.doctor(session)
            self.assertTrue(doctor['reachable']);self.assertTrue(doctor['identity_matches'])
            self.assertFalse(doctor['capabilities']['qmp']);self.assertFalse(doctor['capabilities']['panic_transport'])
            self.assertTrue(doctor['capabilities']['command_jobs']);self.assertTrue(doctor['capabilities']['semantic_windows'])
            self.assertEqual(self.hardware.stats(session)['process_count'],7)
            logs=self.hardware.logs(session);self.assertEqual(logs['next_cursor'],2);self.assertEqual(logs['dropped_lines'],1)
    def test_reboot_blocks_commands_and_preserves_old_session(self):
        session=self.attach();old=self.hardware.load_session(session)['guest_boot_id'];self.endpoint.epoch='b'*32
        with self.assertRaises(HardwareBootChanged):self.hardware.stats(session)
        doctor=self.hardware.doctor(session);self.assertTrue(doctor['reachable']);self.assertFalse(doctor['identity_matches'])
        self.assertEqual(self.hardware.load_session(session)['guest_boot_id'],old)
        fresh=self.attach();self.assertNotEqual(fresh,session);self.assertEqual(self.hardware.stats(fresh)['process_count'],7)
    def test_unknown_delivery_is_not_replayed(self):
        session=self.attach()
        with self.assertRaises(TimeoutError):self.hardware.command(session,'effect')
        self.assertEqual(self.endpoint.effects,1)
    def test_persisted_session_can_reconnect_without_scan(self):
        session=self.attach();count=len(self.endpoint.commands)
        other=Hardware(self.tmp.name);rows=other.sessions()['sessions'];self.assertEqual(len(self.endpoint.commands),count)
        self.assertEqual(rows[0]['session'],session);self.assertEqual(other.stats(session)['process_count'],7)
    def test_input_validation_happens_before_connection(self):
        for host,port in [('localhost',1234),('0.0.0.0',1234),('224.0.0.1',1234),('127.0.0.1',0),('127.0.0.1',True)]:
            with self.subTest(host=host,port=port),self.assertRaises(ValueError):self.hardware.attach(host,port)
        session=self.attach();count=len(self.endpoint.commands)
        for command in ['x\ny','x\0y','x'*175]:
            with self.assertRaises(ValueError):self.hardware.command(session,command)
        for cursor,limit in [(True,64),(-1,64),(0,65)]:
            with self.assertRaises(ValueError):self.hardware.logs(session,cursor,limit)
        self.assertEqual(len(self.endpoint.commands),count)
    def test_existing_job_boot_envelope_is_not_expanded(self):
        session=self.attach();data=self.hardware.load_session(session);command='bridge job put '+'a'*32+' 0 '+'00'*64+' '+data['guest_boot_id']
        self.assertEqual(self.hardware.guarded(data,command),command)
        self.assertEqual(self.hardware.guarded(data,'bridge stats'),'bridge guard '+data['guest_boot_id']+' bridge stats')
    def test_hardware_power_status_and_manual_commit(self):
        session=self.attach();request='1'*32
        caps=self.hardware.doctor(session)['capabilities'];self.assertTrue(caps['hardware_power']);self.assertFalse(caps['power']['power_off'])
        self.assertEqual(self.hardware.power(session,request,'shutdown')['state'],'ready')
        self.assertEqual(self.hardware.power(session,request,'status')['state'],'ready')
        self.assertEqual(self.hardware.power(session,request,'commit')['error'],-95)
        self.assertEqual(self.endpoint.power_effects,1)
    def test_hardware_power_unknown_delivery_is_recovered_without_replay(self):
        session=self.attach();request='2'*32;self.endpoint.power_drop=True
        with self.assertRaises(TimeoutError):self.hardware.power(session,request,'shutdown')
        self.assertEqual(self.endpoint.power_effects,1)
        self.assertEqual(self.hardware.power(session,request,'status')['state'],'ready')
        self.assertEqual(self.endpoint.power_effects,1)
        self.endpoint.epoch='b'*32
        with self.assertRaises(HardwareBootChanged):self.hardware.power(session,request,'commit')
        self.assertEqual(self.endpoint.power_effects,1)
    def test_hardware_power_validation_and_response_identity(self):
        session=self.attach();count=len(self.endpoint.commands)
        for request,operation in [('short','shutdown'),('1'*32,'invalid'),('1'*32,None)]:
            with self.assertRaises(ValueError):self.hardware.power(session,request,operation)
        self.assertEqual(len(self.endpoint.commands),count)
        from jobs import GuestJobError
        with patch.object(self.hardware,'guest',return_value={'reply':'unsupported'}),self.assertRaises(GuestJobError):
            self.hardware.power(session,'1'*32,'shutdown')

    def test_session_symlinks_rejected(self):
        session=self.attach();file=Path(self.tmp.name)/session/'session.json';backup=file.with_suffix('.saved');file.rename(backup);file.symlink_to(backup)
        with self.assertRaises(ValueError):self.hardware.load_session(session)

if __name__=='__main__':unittest.main()
