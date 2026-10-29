#!/usr/bin/env python3
import copy
import unittest
from workloads.analyze_vulkan_timestamps import analyze
from workloads.analyze_cube_timing import analyze as cube_analyze


class Timestamps(unittest.TestCase):
    def fixture(self):
        # Counter wraps at 256; upper bits of query values need not be valid.
        return dict(passed=True, rounds=3, timing=dict(valid_bits=8, period_ns=2,
            rounds=[dict(ticks=[250, 254, 258, 260], cpu_ns=[100, 105, 140]) for _ in range(3)],
            calibrations=[dict(gpu_ticks=248, cpu_ns=100, max_deviation_ns=2),
                          dict(gpu_ticks=264, cpu_ns=132, max_deviation_ns=3)]))

    def test_wrapped_calibrated(self):
        result = analyze(self.fixture())
        self.assertEqual(result['rounds'][0]['gpu_interval_ns'], 20)
        self.assertEqual(result['rounds'][0]['gpu_segments_ns'], [8, 8, 4])
        self.assertEqual(result['rounds'][0]['calibrated_estimates'][0]['submit_to_gpu_start_ns'], 4)
        self.assertEqual(result['rounds'][0]['calibrated_estimates'][1]['gpu_end_to_completion_return_ns'], 16)

    def test_uncalibrated(self):
        r = self.fixture(); r['timing']['calibrations'] = []
        self.assertFalse(analyze(r)['calibrated'])

    def test_cube_pacing_and_invalid_captures(self):
        header = '# bits=8 period_ns=2 errors=0 overflow=0\nframe,cycle_start_ns,after_wait_ns,after_acquire_ns,before_submit_ns,after_submit_ns,before_present_ns,after_present_ns,gpu_start,gpu_end,submitted,ready\n'
        trace = header + '0,100,110,120,130,140,150,160,250,260,1,1\n1,200,210,220,230,240,250,260,10,20,1,1\n'
        result = cube_analyze(trace)
        self.assertEqual(result['frames'], 2)
        self.assertEqual(result['summary_ms']['gpu_interval_ms']['p50'], .00002)
        self.assertEqual(result['summary_ms']['between_draw_calls_ms']['p50'], .00004)
        for broken in [trace.replace('errors=0', 'errors=1'), trace.replace('overflow=0', 'overflow=1'), trace.replace('10,20,1,1', '10,20,1,0'), trace.replace('1,200,210', '1,150,210')]:
            with self.assertRaises(ValueError): cube_analyze(broken)

    def test_reject_invalid(self):
        base = self.fixture()
        for key, value in [('valid_bits', 0), ('period_ns', float('nan')), ('calibrations', base['timing']['calibrations'][:1]), ('rounds', [])]:
            r = copy.deepcopy(base); r['timing'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): analyze(r)
        for ticks, cpu in [([50, 40, 60, 70], [100, 105, 140]), ([250, 254, 258, 260], [100, 90, 140])]:
            r = copy.deepcopy(base); r['timing']['rounds'][0].update(ticks=ticks, cpu_ns=cpu)
            with self.assertRaises(ValueError): analyze(r)

    def test_cube_window_requires_complete_intervals_and_validates_all_rows(self):
        header = '# bits=8 period_ns=2 errors=0 overflow=0\nframe,cycle_start_ns,after_wait_ns,after_acquire_ns,before_submit_ns,after_submit_ns,before_present_ns,after_present_ns,gpu_start,gpu_end,submitted,ready\n'
        trace=header+''.join(f'{i},{100+i*100},{110+i*100},{120+i*100},{130+i*100},{140+i*100},{150+i*100},{160+i*100},10,20,1,1\n' for i in range(4))
        result=cube_analyze(trace,200,400)
        self.assertEqual((result['frames'],result['captured_frames']),(1,4))
        self.assertEqual(result['longest_present_intervals'][0]['frame'],2)
        self.assertEqual(result['summary_ms']['present_return_interval_ms']['p95'],.0001)
        self.assertEqual(cube_analyze(trace,160,460)['frames'],3)
        for start,end in [(None,400),(200,None),(400,200),(0,1),(True,400),(-1,400),(0,1<<64)]:
            with self.assertRaises(ValueError):cube_analyze(trace,start,end)
        # A clipped analysis must not hide an invalid row outside the window.
        with self.assertRaises(ValueError):cube_analyze(trace.replace('0,100,110','0,100,90'),200,400)


if __name__ == '__main__': unittest.main()
