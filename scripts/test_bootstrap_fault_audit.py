import unittest
import copy
from audit_bootstrap_faults import audit_bytes, completed_test_passes

S = dict(handler=0x201e50, from_instruction=0x202054, from_fixup=0x202059,
         to_instruction=0x202064, to_fixup=0x202069)
FIXTURE = b"""sys_execve: path='/data/bootstrap/Brook/build/release/host_tests/test_user_copy'
[FAULT] User fault vec=0x000e
  RIP 0x202054
  RSI 0x10496000
  RDI 0x7fffffffcc00
  ERR 0x4
[SIG] Delivering signal 11 to pid 14 (handler=0x201e50 faultAddr=0x10496000 RIP=0x202054)
SIGRETURN: pid 14 rip=0x202059 rax=0
"""

class FaultAuditTests(unittest.TestCase):
    def test_exact_recovered_probe_and_non_utf8_noise(self):
        result = audit_bytes(b'\xe2\xff\n' + FIXTURE, S)
        self.assertEqual(result['visible_user_faults'], 1)
        self.assertEqual(result['recognised_recovered_probes'][0]['direction'], 'from')

    def test_wrong_image_instruction_vector_handler_operand_or_fixup_rejected(self):
        for old, new in [(b'test_user_copy', b'chromium'),
                         (b'RIP 0x202054', b'RIP 0x202055'),
                         (b'vec=0x000e', b'vec=0x000d'),
                         (b'handler=0x201e50', b'handler=0x0'),
                         (b'faultAddr=0x10496000', b'faultAddr=0x10496001'),
                         (b'rip=0x202059', b'rip=0x202060')]:
            with self.subTest(new=new), self.assertRaises(ValueError):
                audit_bytes(FIXTURE.replace(old, new), S)

    def test_missing_recovery_and_unknown_second_fault_rejected(self):
        with self.assertRaises(ValueError):
            audit_bytes(FIXTURE.split(b'SIGRETURN:')[0], S)
        with self.assertRaises(ValueError):
            audit_bytes(FIXTURE + FIXTURE.replace(b'test_user_copy', b'gimp'), S)

    def test_copy_to_probe(self):
        data = FIXTURE.replace(b'0x202054', b'0x202064').replace(b'0x202059', b'0x202069')
        data = data.replace(b'ERR 0x4', b'ERR 0x6').replace(b'faultAddr=0x10496000', b'faultAddr=0x7fffffffcc00')
        self.assertEqual(audit_bytes(data, S)['recognised_recovered_probes'][0]['direction'], 'to')

    def test_unlogged_large_rax_case_requires_exact_sentinels_and_test_pass(self):
        data = FIXTURE.split(b'SIGRETURN:')[0] + b'  RAX 0x1122334455667788\n  R14 0x8877665544332211\n'
        with self.assertRaises(ValueError):
            audit_bytes(data, S)
        self.assertIn('completed_test_assertions', audit_bytes(data, S, 1)['recognised_recovered_probes'][0]['recovery_evidence'])
        with self.assertRaises(ValueError):
            audit_bytes(data.replace(b'0x8877665544332211', b'0x0'), S, 1)

    def test_pass_text_requires_real_drained_command_and_complete_output(self):
        valid = {'stdout': '9/39 Test #9: test_user_copy .... Passed 0.3 sec\n',
                 'status': {'state': 'completed', 'effect': 'confirmed',
                     'exit': {'code': 0, 'signal': None},
                     'guest': {'live_tasks': 0, 'leader_exited': True, 'exit_collected': True,
                               'stdout_eof': True, 'stderr_eof': True},
                     'stdout': {'eof': True}, 'stderr': {'eof': True}}}
        self.assertEqual(completed_test_passes(valid), 1)
        for section, field, value in [('exit', 'code', 1), ('exit', 'signal', 11),
                ('guest', 'live_tasks', 1), ('guest', 'exit_collected', False),
                ('stdout', 'eof', False), ('stderr', 'gaps', [{'start': 0, 'end': 20}]),
                ('stdout', 'read_timed_out', True)]:
            failed = copy.deepcopy(valid); failed['status'][section][field] = value
            with self.subTest(section=section, field=field), self.assertRaises(ValueError):
                completed_test_passes(failed)

    def test_later_exec_cannot_supply_recovery_for_an_earlier_fault(self):
        data = FIXTURE.split(b'SIGRETURN:')[0] + b"sys_execve: path='/other'\nSIGRETURN: pid 14 rip=0x202059\n"
        with self.assertRaises(ValueError):
            audit_bytes(data, S)

if __name__ == '__main__':
    unittest.main()
