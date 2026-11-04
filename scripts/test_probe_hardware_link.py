import errno
import unittest
from unittest.mock import patch

from scripts import probe_hardware_link as probe


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.host=patch.object(probe,'run_host_command',return_value={'returncode':0}).start()
        self.addCleanup(patch.stopall)

    def test_partial_frame_does_not_claim_bridge_is_healthy(self):
        def partial(host,port,commands,timeout,progress):
            progress('connected',0);progress('request_sent',0);progress('frame_started',0)
            raise TimeoutError('partial reply')
        with patch.object(probe,'exchange',partial):result=probe.probe('192.168.85.106')
        self.assertEqual(result['bridge']['stage'],'frame_started')
        self.assertIn('partial reply',result['bridge']['error'])
        self.assertNotIn('boot_id',result['bridge'])

    def test_connect_error_preserves_errno(self):
        def fail(host,port,commands,timeout,progress):
            progress('connecting',0)
            raise OSError(errno.EHOSTUNREACH,'No route to host')
        with patch.object(probe,'exchange',fail):result=probe.probe('192.168.85.106')
        self.assertEqual(result['bridge']['stage'],'connecting')
        self.assertEqual(result['bridge']['errno'],errno.EHOSTUNREACH)

    def test_caps_and_no_mutating_commands(self):
        def healthy(host,port,commands,timeout,progress):
            self.assertEqual(commands,['bridge hardware caps'])
            progress('frame_complete',1)
            return [{'hardware_api':1,'boot_guard':True,'boot_id':'a'*32}]
        with patch.object(probe,'exchange',healthy):result=probe.probe('192.168.85.106')
        self.assertEqual(result['bridge']['stage'],'capabilities_received')
        self.assertEqual(result['bridge']['boot_id'],'a'*32)
        self.assertEqual(self.host.call_count,3)

    def test_requires_explicit_unicast_ip(self):
        for address in ('localhost','0.0.0.0','224.0.0.1'):
            with self.subTest(address=address),self.assertRaises(ValueError):probe.probe(address)
        self.assertFalse(self.host.called)


if __name__=='__main__':unittest.main()
