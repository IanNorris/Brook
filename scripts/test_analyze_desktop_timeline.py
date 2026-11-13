import unittest

from analyze_desktop_timeline import analyze


class TimelineAnalysisTests(unittest.TestCase):
    def test_partitions_frame_and_keeps_gpu_separate(self):
        data = ('DESKTOP_TIMELINE frame=1 ticket=2 window=3 incarnation=4 commit=5 '
                'received=110 start=100 submit_start=160 submitted=170 completed=200 '
                'presented=210 finish=220 gpu_ns=22 gpu_valid=1\n'
                'DESKTOP_TIMELINE_DETAIL frame=1 ticket=2 channel=103 query=105 clients=110 backdrop=120 '
                'chrome=130 overlay=140 pixels=4096 layers=3 regions=2 graphics=1 partial=1\n'
                'DESKTOP_TIMELINE_SUMMARY rows=1 dropped=0 scope=test\n')
        result = analyze(data)
        self.assertEqual(result['invalid_rows'], 0)
        self.assertEqual(result['unpaired_timeline'], 0)
        self.assertEqual(result['stages']['total']['p50_ns'], 120)
        self.assertEqual(result['stages']['channel_poll']['p50_ns'], 3)
        self.assertEqual(result['stages']['prepare_after_overlay']['p50_ns'], 20)
        self.assertEqual(result['stages']['completion_wait']['p50_ns'], 30)
        self.assertEqual(result['stages']['gpu_interval']['p50_ns'], 22)
        self.assertEqual(result['summary']['dropped'], '0')
        path = 'graphics=1 partial=1 pixels=4096 layers=3 regions=2'
        self.assertEqual(result['stages_by_path'][path]['completion_wait']['p50_ns'], 30)
        self.assertEqual(analyze(data, (100, 220))['outside_markers'], 0)
        excluded = analyze(data, (101, 220))
        self.assertEqual(excluded['outside_markers'], 1)
        self.assertIsNone(excluded['stages']['total'])

    def test_invalid_and_unpaired_are_reported(self):
        data = ('DESKTOP_TIMELINE frame=1 ticket=2 start=100 submit_start=160 submitted=170 '
                'completed=200 presented=210 finish=220 gpu_ns=0 gpu_valid=0\n'
                'DESKTOP_TIMELINE_DETAIL frame=1 ticket=2 channel=103 query=105 clients=110 backdrop=120 chrome=130 '
                'overlay=90 pixels=4096 layers=3 regions=2 graphics=1 partial=1\n'
                'DESKTOP_TIMELINE frame=2 ticket=3 start=100\n')
        result = analyze(data)
        self.assertEqual(result['invalid_rows'], 1)
        self.assertEqual(result['unpaired_timeline'], 1)
        self.assertIsNone(result['stages']['total'])


if __name__ == '__main__':
    unittest.main()
