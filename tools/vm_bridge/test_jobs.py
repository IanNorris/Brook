"""Command-job identities, actual transport payloads and cursor semantics."""
import contextlib
import json
from pathlib import Path
import struct
import time
import tempfile
import unittest

from common import write_json
from jobs import Jobs, request_bytes


class JobTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.session='a'*32;self.ident='b'*32
        (self.root/self.session).mkdir()
        self.data={'generation':'first','state':'running'}
        self.boot='e'*32
        self.calls=[];self.payload=bytearray();self.commits=0
        self.state='uploading';self.stdout=b'hello\n';self.stderr=b'warning\n';self.complete=True
        self.fail_commit=False;self.begun=False;self.fail_put=False;self.fail_begin=False
        @contextlib.contextmanager
        def lock(session,timeout=10):yield self.data
        self.client=Jobs(self.root,lambda *args,**kw:self.data,lock,self.guest,write_json)

    def snapshot(self):
        return {'state':self.state,'received':len(self.payload),'pid':55,'incarnation':999,
                'leader_exited':self.complete,'exit_collected':self.state=='completed',
                'wait_status':7<<8,'live_tasks':0 if self.complete else 2,
                'stdout_eof':self.complete,'stderr_eof':self.complete}

    def guest(self,data,command,timeout):
        self.calls.append(command)
        args=command.split(' ')
        if args[1]=='epoch':
            if len(args)>2 and self.boot=='0'*32:self.boot=args[3]
            return {'boot_id':self.boot}
        action=args[2]
        if action=='caps':return {'job_api':2,'boot_id':self.boot}
        if args[-1]!=self.boot:return {'job_error':'Stale guest boot'}
        args=args[:-1]
        if action=='begin':
            self.begun=True
            if self.fail_begin:
                self.fail_begin=False;raise TimeoutError('begin reply lost')
            return self.snapshot()
        if action=='put':
            offset=int(args[4]);block=bytes.fromhex(args[5])
            if offset<len(self.payload):self.assertEqual(bytes(self.payload[offset:offset+len(block)]),block)
            else:
                self.assertEqual(offset,len(self.payload));self.payload.extend(block)
            if self.fail_put:
                self.fail_put=False;raise TimeoutError('put reply lost')
            return self.snapshot()
        if action=='commit':
            self.commits+=1;self.state='completed' if self.complete else 'running'
            if self.fail_commit:raise TimeoutError('reply lost after commit')
            return self.snapshot()
        if action=='status':return self.snapshot() if self.begun else {'job_error':'Unknown job'}
        if action=='read':
            stream=self.stdout if args[4]=='out' else self.stderr
            offset,limit=map(int,args[5:7]);block=stream[offset:offset+limit]
            return {'start':offset,'next':offset+len(block),'dropped':0,
                    'eof':self.complete and offset+len(block)==len(stream),'hex':block.hex()}
        raise AssertionError(command)

    def submit(self,**kwargs):
        return self.client.submit(self.session,self.ident,['/bin/test','space ; $() "'],wait_s=1,**kwargs)

    def test_structured_payload_is_not_shell_source_and_survives_reconnect(self):
        result=self.submit(env={'CUSTOM':'hello\nworld'})
        self.assertEqual(result['state'],'completed')
        self.assertEqual(result['exit']['code'],7)
        self.assertEqual(result['stdout']['text'],'hello\n')
        self.assertEqual(result['stderr']['text'],'warning\n')
        self.assertTrue(result['stdout']['eof'])
        self.assertEqual(struct.unpack('<III',self.payload[4:16]),(1000,2,1))
        self.assertIn(b'space ; $() "\0',self.payload)
        self.assertTrue(all(len(command.encode())<=220 for command in self.calls))
        other=Jobs(self.root,self.client.load_session,self.client.session_lock,self.guest,write_json)
        recovered=other.status(self.session,self.ident)
        self.assertEqual(recovered['operation_id'],self.ident)
        self.assertEqual(recovered['exit']['code'],7)
        self.assertEqual(self.commits,1)

    def test_repeated_request_is_deduplicated_but_changed_args_rejected(self):
        self.submit();self.submit()
        self.assertEqual(self.commits,1)
        with self.assertRaisesRegex(ValueError,'different arguments'):
            self.client.submit(self.session,self.ident,['/bin/other'])
        self.assertEqual(self.commits,1)

    def test_unknown_commit_is_inspected_without_retry(self):
        self.fail_commit=True
        result=self.submit()
        self.assertIn(result['state'],('unknown','completed'))
        recovered=self.client.status(self.session,self.ident)
        self.assertEqual(recovered['state'],'completed')
        self.assertEqual(self.commits,1)

    def test_explicit_resubmit_recovers_lost_begin_and_put_replies(self):
        for failure in ('fail_begin','fail_put'):
            with self.subTest(failure=failure):
                self.ident=('c' if failure=='fail_begin' else 'd')*32
                self.payload.clear();self.begun=False;self.state='uploading';before=self.commits
                setattr(self,failure,True)
                begins=sum(' job begin ' in call for call in self.calls)
                self.submit()
                self.assertEqual(sum(' job begin ' in call for call in self.calls),begins+1)
                result=self.submit()
                self.assertEqual(result['state'],'completed')
                self.assertEqual(self.commits,before+1)

    def test_explicit_resubmit_recovers_created_descriptor_without_dispatch(self):
        import threading
        from unittest.mock import patch
        with patch.object(threading.Thread,'start'):
            result=self.client.submit(self.session,self.ident,['/bin/test','space ; $() "'])
        self.assertEqual(result['stage'],'created');self.assertEqual(self.commits,0)
        self.assertEqual(self.submit()['state'],'completed')
        self.assertEqual(self.commits,1)
        self.submit()
        self.assertEqual(self.commits,1)

    def test_generation_change_loses_old_operation(self):
        self.submit();self.data['generation']='second'
        result=self.client.status(self.session,self.ident)
        self.assertEqual(result['state'],'lost')
        self.assertEqual(self.commits,1)
        with self.assertRaisesRegex(ValueError,'different arguments or generation'):
            self.submit()

    def test_guest_self_reboot_loses_job_without_replay_or_new_output(self):
        self.submit();before=self.commits
        self.boot='f'*32;self.stdout=b'WRONG NEW BOOT'
        result=self.client.status(self.session,self.ident)
        self.assertEqual(result['state'],'lost')
        self.assertNotIn('stdout',result)
        with self.assertRaisesRegex(ValueError,'different arguments'):
            self.submit()
        self.assertEqual(self.commits,before)

    def test_unset_epoch_initializes_once_and_old_id_never_rebinds(self):
        self.boot='0'*32
        self.submit();old=self.boot
        self.assertNotEqual(old,'0'*32)
        self.boot='0'*32
        with self.assertRaisesRegex(ValueError,'different arguments'):
            self.submit()
        self.assertNotEqual(self.boot,old)
        self.assertEqual(self.commits,1)

    def test_abandoned_created_job_is_lost_after_guest_self_reboot(self):
        import threading
        from unittest.mock import patch
        with patch.object(threading.Thread,'start'):
            pending=self.client.submit(self.session,self.ident,['/bin/test'])
        self.assertEqual(pending['stage'],'created')
        self.boot='f'*32
        result=self.client.status(self.session,self.ident)
        self.assertEqual(result['state'],'lost')
        self.assertEqual(self.commits,0)

    def test_reboot_between_status_and_output_cannot_read_new_boot_streams(self):
        self.submit()
        original=self.client.guest
        def guest(data,command,timeout):
            if command.startswith('bridge job read '):self.boot='f'*32
            return original(data,command,timeout)
        self.client.guest=guest
        result=self.client.status(self.session,self.ident)
        self.assertEqual(result['state'],'lost')
        self.assertEqual(result['stdout']['text'],'')
        self.assertEqual(result['stdout']['read_error'],'Stale guest boot')

    def test_stopped_guest_returns_labelled_cached_evidence(self):
        self.submit();self.data['state']='stopped'
        result=self.client.status(self.session,self.ident)
        self.assertTrue(result['cached']);self.assertEqual(result['state'],'completed')

    def test_leader_exit_does_not_imply_job_or_stream_completion(self):
        self.complete=False
        result=self.client.submit(self.session,self.ident,['/bin/test'],wait_s=.1)
        # Upload is asynchronous; wait for the operation's upload lock to release.
        path=self.client.path(self.session,self.ident)
        with self.client.lock(path):pass
        self.complete=False
        result=self.client.status(self.session,self.ident)
        self.assertEqual(result['state'],'running')
        self.assertFalse(result['stdout']['eof'])
        self.assertEqual(result['guest']['live_tasks'],2)
        self.assertNotIn('exit',result)

    def test_utf8_tail_is_not_silently_consumed(self):
        self.stdout='A€Z'.encode();self.complete=False
        result=self.client.stream(self.session,'first',self.ident,'out',0,3,boot_id=self.boot)
        self.assertEqual(result['text'],'A')
        self.assertEqual(result['next_offset'],1)
        self.assertEqual(result['pending_utf8_bytes'],2)
        result=self.client.stream(self.session,'first',self.ident,'out',1,4,boot_id=self.boot)
        self.assertEqual(result['text'],'€Z')
        self.assertEqual(result['next_offset'],5)

    def test_retention_loss_is_explicit(self):
        def dropped(data,command,timeout):
            return {'start':100,'next':103,'dropped':100,'eof':True,'hex':b'end'.hex()}
        self.client.guest=dropped
        result=self.client.stream(self.session,'first',self.ident,'out',0,1024)
        self.assertEqual(result['text'],'end');self.assertEqual(result['dropped_bytes'],100)
        self.assertEqual(result['next_offset'],103)

    def test_utf8_cannot_bridge_a_mid_read_retention_gap(self):
        replies=iter([
            {'start':0,'next':1024,'dropped':0,'eof':False,'hex':(b'a'*1023+b'\xe2').hex()},
            {'start':2000,'next':2002,'dropped':976,'eof':True,'hex':b'\x82\xac'.hex()}])
        self.client.guest=lambda *a,**k:next(replies)
        result=self.client.stream(self.session,'first',self.ident,'out',0,2048)
        self.assertNotIn('€',result['text'])
        self.assertEqual(result['dropped_bytes'],976)
        self.assertEqual(result['next_offset'],2002)
        self.assertEqual(result['gaps'],[{'after_offset':1024,'before_offset':2000,'dropped_bytes':976}])

    def test_invalid_request_rejected_before_guest_mutations(self):
        cases=[([],{}),(['/bin/a\0'],{}),(['relative'],{}),(['/bin/a'],{'cwd':'relative'}),
               (['/bin/a'],{'uid':True}),(['/bin/a'],{'env':{'A=B':'x'}}),
               (['/bin/a'],{'env':{'A':'x\0'}}),(['/bin/a','x'*32768],{})]
        for argv,kwargs in cases:
            with self.subTest(argv=argv[:1],kwargs=kwargs),self.assertRaises(ValueError):
                request_bytes(argv,**kwargs)
        self.assertEqual(self.calls,[])

    def test_expired_read_budget_returns_cursor_without_another_exchange(self):
        result=self.client.stream(self.session,'first',self.ident,'out',9,1024,time.monotonic()-1)
        self.assertTrue(result['read_timed_out'])
        self.assertEqual(result['next_offset'],9)
        self.assertEqual(self.calls,[])

    def test_readonly_transport_timeout_retries_without_commit_replay(self):
        self.submit();before=self.commits
        original=self.client.guest;failed=False
        def guest(data,command,timeout):
            nonlocal failed
            if command.startswith('bridge job status '+self.ident) and not failed:
                failed=True;raise TimeoutError('temporary read timeout')
            return original(data,command,timeout)
        self.client.guest=guest
        result=self.client.status(self.session,self.ident)
        self.assertEqual(result['state'],'completed')
        self.assertEqual(result['readonly_transport_retries'],1)
        self.assertEqual(self.commits,before)


class ControlCapacityTests(unittest.TestCase):
    def test_normal_cli_exit_does_not_abandon_queued_upload(self):
        import subprocess
        import sys
        source='''
import sys,time
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from test_jobs import JobTests
case=JobTests();case.setUp()
original=case.client.upload
def upload(path,payload):
    time.sleep(.1)
    original(path,payload)
    Path(sys.argv[2]).write_text(str(case.commits))
case.client.upload=upload
result=case.client.submit(case.session,case.ident,['/bin/test'])
print(result['operation_id'],flush=True)
'''
        with tempfile.TemporaryDirectory() as temp:
            marker=Path(temp)/'commits'
            result=subprocess.run([sys.executable,'-c',source,str(Path(__file__).parent),str(marker)],
                capture_output=True,text=True,timeout=5,check=True)
            self.assertIn('b'*32,result.stdout)
            self.assertEqual(marker.read_text(),'1')

    def test_four_waiters_cannot_starve_control(self):
        import selectors
        import subprocess
        import sys
        source='''
import sys
sys.path.insert(0,sys.argv[1])
import server
fn,spec=server.TOOLS['vm_wait']
server.TOOLS['vm_wait']=(lambda _cancel=None,**kw: {'cancelled':_cancel.wait(10)},spec)
fn,spec=server.TOOLS['vm_control']
server.TOOLS['vm_control']=(lambda **kw: {'control_served':True},spec)
server.serve()
'''
        child=subprocess.Popen([sys.executable,'-c',source,str(Path(__file__).parent)],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            for ident in range(4):
                child.stdin.write(json.dumps({'jsonrpc':'2.0','id':ident,'method':'tools/call',
                    'params':{'name':'vm_wait','arguments':{'session':'a'*32,'text':'waiting'}}})+'\n')
            child.stdin.write(json.dumps({'jsonrpc':'2.0','id':99,'method':'tools/call',
                'params':{'name':'vm_control','arguments':{'session':'a'*32,'action':'stop'}}})+'\n')
            child.stdin.flush()
            with selectors.DefaultSelector() as selector:
                selector.register(child.stdout,selectors.EVENT_READ)
                self.assertTrue(selector.select(timeout=2),'Control was starved by four waiters')
                reply=json.loads(child.stdout.readline())
            self.assertEqual(reply['id'],99)
            self.assertTrue(json.loads(reply['result']['content'][0]['text'])['control_served'])
        finally:
            child.stdin.close()
            try:child.wait(timeout=3)
            except subprocess.TimeoutExpired:child.kill();child.wait()
            child.stdout.close();child.stderr.close()


if __name__=='__main__':unittest.main()
