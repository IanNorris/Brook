import unittest
from analyze_memory_trace import analyze


def mmap(pid, size, result, flags='0x22', comm='cc1plus'):
    return (f'[strace:{pid}] mmap(0x0, {size}, 0x3, {flags}, -1, 0x0)'
            f' = {result} comm="{comm}"\n')


class MemoryTrace(unittest.TestCase):
    def test_success_error_and_process_attribution(self):
        report = analyze(mmap(2, 65536, '0x10000') +
                         mmap(3, 2097152, '-12 (err)') +
                         mmap(2, 4096, '0x20000'))
        self.assertTrue(report['complete'])
        first, second = report['processes']
        self.assertEqual(first['mmap_successes'], 2)
        self.assertEqual(first['candidate_successful_requested_bytes'], 65536)
        self.assertEqual(second['candidate_requests'], 1)
        self.assertEqual(second['candidate_successes'], 0)
        self.assertEqual(second['errors']['mmap'], 1)

    def test_fixed_populate_and_bounds_excluded(self):
        trace = ''.join(mmap(2, size, '0x10000', flags) for size, flags in
                        [(65536, '0x32'), (65536, '0x100022'),
                         (65536, '0x8022'), (4096, '0x22'),
                         (32*1024*1024, '0x22')])
        self.assertEqual(analyze(trace)['processes'][0]['candidate_requests'], 0)

    def test_split_truncated_and_suppressed_rejected(self):
        for trace in ['', '[strace:2] mmap(0x0, 65536, 0x3, 0x22, -1, 0x0) = ',
                      mmap(2, 65536, '0x10000')[:-1],
                      '[strace:2] ... suppressing repeated mmap(0) = -12\n']:
            self.assertFalse(analyze(trace)['complete'])

    def test_nonallocation_memory_and_malformed(self):
        trace = ('[strace:2] munmap(0x10000, 0x1000, 0x0, 0x0, 0x0, 0x0)'
                 ' = 0 comm="cc1plus"\n'
                 '[strace:2] brk(0x0, 0x0, 0x0) = 0x40000 comm="cc1plus"\n')
        report = analyze(trace)
        self.assertTrue(report['complete'])
        self.assertEqual(report['processes'][0]['mmap_requests'], 0)
        malformed = trace.replace('0x10000, ', 'invalid, ')
        self.assertFalse(analyze(malformed)['complete'])


if __name__ == '__main__':
    unittest.main()
