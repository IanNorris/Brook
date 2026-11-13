import json
from pathlib import Path
import tempfile
import unittest

from analyze_frame_swap_overlap import analyze


class OverlapTests(unittest.TestCase):
    def test_clips_and_unions_swaps_without_double_counting(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            swaps = root / 'swap.csv'
            swaps.write_text('# brook_swap_trace_v1 count=3 dropped=0 incomplete=0 invalid_clock=0\n'
                             'index,tid,begin_ns,end_ns\n0,1,90,120\n1,2,110,150\n2,1,180,220\n')
            renderer = root / 'render.log'
            renderer.write_text('DESKTOP_TIMELINE frame=1 window=2 start=100 submit_start=120 '
                                'submitted=130 completed=180 presented=190 finish=200\n'
                                'DESKTOP_TIMELINE_SUMMARY rows=1 dropped=4 scope=test\n')
            timing = root / 'timing.json'
            timing.write_text(json.dumps({'markers': {'START': {'after_ns': 100}, 'END': {'after_ns': 200}}}))
            result = analyze(swaps, renderer, timing)
            self.assertEqual(result['swap_union_ns'], 70)
            self.assertEqual(result['stages']['preparation']['swap_overlap_ns'], 20)
            self.assertEqual(result['stages']['completion_wait']['swap_overlap_ns'], 20)
            self.assertEqual(result['renderer_summary']['dropped'], '4')

    def test_rejects_trace_count_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'swap').write_text('# brook_swap_trace_v1 count=1 dropped=0 incomplete=0 invalid_clock=0\nindex,tid,begin_ns,end_ns\n')
            (root / 'renderer').write_text('')
            (root / 'timing').write_text(json.dumps({'markers': {'START': {'after_ns': 1}, 'END': {'after_ns': 2}}}))
            with self.assertRaises(ValueError):
                analyze(root / 'swap', root / 'renderer', root / 'timing')


if __name__ == '__main__':
    unittest.main()
