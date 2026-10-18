import fcntl
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import lifecycle

SESSION='1'*32
REQUEST='2'*32
BOOT='3'*32


class Channel(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.directory=self.root/SESSION;self.directory.mkdir()
        self.data={'generation':SESSION,'lifecycle_channel':{'api':1}}
        self.registry=patch.object(lifecycle,'REGISTRY',self.root);self.registry.start()
        self.loader=patch.object(lifecycle,'load_session',return_value=self.data);self.load=self.loader.start()
        self.listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.listener.bind(str(self.directory/'lifecycle.sock'));self.listener.listen(1)
        self.listener.settimeout(.5);self.received=[];self.errors=[];self.thread=None

    def tearDown(self):
        if self.thread:self.thread.join(1);self.assertFalse(self.thread.is_alive())
        self.listener.close();self.loader.stop();self.registry.stop();self.temp.cleanup()
        self.assertEqual(self.errors,[])

    def serve(self,response,fragment=False,delay=0):
        def run():
            try:
                conn,_=self.listener.accept()
                with conn:
                    conn.settimeout(.5);data=b''
                    while not data.endswith(b'\n'):data+=conn.recv(512)
                    self.received.append(data)
                    if delay:time.sleep(delay)
                    if response is not None:
                        chunks=[bytes([c]) for c in response] if fragment else [response]
                        for chunk in chunks:conn.sendall(chunk)
            except (BrokenPipeError,ConnectionResetError):pass
            except Exception as error:self.errors.append(repr(error))
        self.thread=threading.Thread(target=run);self.thread.start()

    def reply(self,state='done',result=0,boot=BOOT,request=REQUEST):
        return f'BLC1 {request} {boot} {state} {result}\n'.encode()

    def call(self,verb='sync',timeout=.4):
        return lifecycle.exchange(SESSION,verb,REQUEST,BOOT,timeout=timeout)

    def test_sync_fragmented_response_and_exact_request(self):
        self.serve(self.reply(),fragment=True);result=self.call()
        self.assertTrue(result['guest_sync_completed']);self.assertEqual(result['guest_boot_id'],BOOT)
        self.assertEqual(self.received,[f'BLC1 {SESSION} {REQUEST} {BOOT} sync\n'.encode()])

    def test_failed_flush_is_not_success(self):
        self.serve(self.reply(result=-5));self.assertFalse(self.call()['guest_sync_completed'])

    def test_missing_status_is_not_retried_or_dispatched_as_sync(self):
        self.serve(self.reply('missing',-2));result=self.call('status')
        self.assertEqual(result['state'],'missing');self.assertFalse(result['guest_sync_completed'])
        self.assertEqual(len(self.received),1);self.assertTrue(self.received[0].endswith(b' status\n'))

    def test_stale_boot_explicit(self):
        self.serve(self.reply('stale',-116,boot='4'*32));result=self.call()
        self.assertEqual(result['state'],'stale');self.assertFalse(result['guest_sync_completed'])

    def test_wrong_boot_rejected(self):
        self.serve(self.reply(boot='4'*32))
        with self.assertRaisesRegex(RuntimeError,'another guest boot'):self.call()

    def test_prior_reply_does_not_match_current_request(self):
        self.serve(self.reply(request='4'*32)+self.reply());self.assertTrue(self.call()['guest_sync_completed'])

    def test_disconnect_is_unknown_without_retry(self):
        self.serve(None)
        with self.assertRaisesRegex(ConnectionError,'completion unknown'):self.call()
        self.assertEqual(len(self.received),1)

    def test_timeout_is_not_retried(self):
        self.serve(None,delay=.1)
        with self.assertRaises(TimeoutError):self.call(timeout=.03)
        self.assertEqual(len(self.received),1)

    def test_large_reply_rejected(self):
        self.serve(b'x'*200)
        with self.assertRaisesRegex(ValueError,'bound'):self.call()

    def test_bad_result_rejected(self):
        self.serve(self.reply(result='-99999999999'))
        with self.assertRaises(ValueError):self.call()

    def test_invalid_operation_never_connects(self):
        with self.assertRaises(ValueError):self.call('exec')
        self.assertEqual(self.received,[])

    def test_busy_channel_does_not_dispatch(self):
        with (self.directory/'lifecycle.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            with self.assertRaisesRegex(TimeoutError,'nothing dispatched'):self.call(timeout=.02)
        self.assertEqual(self.received,[])

    def test_old_session_has_no_implicit_tcp_or_input_fallback(self):
        self.data.clear()
        with self.assertRaisesRegex(RuntimeError,'no network-independent'):self.call()
        self.assertEqual(self.received,[])

    def test_replaced_endpoint_rejected(self):
        (self.directory/'lifecycle.sock').unlink();(self.directory/'lifecycle.sock').write_text('')
        with self.assertRaisesRegex(RuntimeError,'owned Unix socket'):self.call()


class Arguments(unittest.TestCase):
    def test_power_ports_only_for_known_q35_policy(self):
        for suffix,power in [([], 'q35-reset'),(['-no-reboot'],'q35-stop'),(['-no-shutdown'],None),(['-readconfig','custom'],None)]:
            args=['qemu','-serial','stdio','-machine','q35']+suffix
            result=lifecycle.launch_args(args,SESSION,Path('/tmp')/SESSION)
            values=[arg for arg in result if 'opt/brook/power' in arg]
            self.assertEqual(values,[] if power is None else ['name=opt/brook/power,string='+power])
        result=lifecycle.launch_args(['qemu','-serial','stdio','-machine','pc'],SESSION,Path('/tmp')/SESSION)
        self.assertFalse(any('opt/brook/power' in arg for arg in result))


    def test_optout_preserves_existing_serial_devices(self):
        original=['qemu','-serial','stdio','-serial','null']
        self.assertEqual(lifecycle.launch_args(original,SESSION,Path('/tmp'),enabled=False),original)

    def test_preserves_com1_adds_private_com2_and_optin(self):
        original=['qemu','-serial','file:serial.log','-netdev','user,id=net0']
        result=lifecycle.launch_args(original,SESSION,Path('/tmp')/SESSION)
        self.assertEqual(original,['qemu','-serial','file:serial.log','-netdev','user,id=net0'])
        self.assertEqual(result[:len(original)],original)
        self.assertEqual(result.count('-serial'),2)
        self.assertIn(f'name=opt/brook/lifecycle,string={SESSION}',result)
        self.assertIn('chardev:brook_lifecycle',result)

    def test_serial_conflicts_rejected(self):
        for command in [['qemu'],['qemu','-serial=stdio'],['qemu','-serial','stdio','-serial','null'],['qemu','-serial','stdio','-fw_cfg','name=opt/brook/lifecycle,string=bad']]:
            with self.assertRaises(ValueError):lifecycle.launch_args(command,SESSION,Path('/tmp'))

    def test_long_socket_path_rejected(self):
        with self.assertRaises(ValueError):lifecycle.launch_args(['qemu','-serial','stdio'],SESSION,Path('/tmp')/('a'*110))

    def test_invalid_id_rejected(self):
        for value in [None,'0'*32,'A'*32,'1'*31,'1'*33,'../oops']:
            with self.assertRaises(ValueError):lifecycle.valid_id(value)


if __name__=='__main__':unittest.main()
