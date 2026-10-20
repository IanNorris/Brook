#!/usr/bin/env python3
"""Independent known-discard fixture and malformed/alignment rejection checks."""
from copy import deepcopy
from datetime import datetime,timezone
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).parent/'workloads'))
from analyze_hda_trace import events,summary,analyze

class TraceAnalysisTests(unittest.TestCase):
    def fixture(self):
        wall=datetime(2026,10,20,tzinfo=timezone.utc).timestamp()
        stamp=lambda t:datetime.fromtimestamp(wall+t,timezone.utc).isoformat()
        trace='\n'.join(f'{stamp(t)} hda_audio_overrun st dac' for t in (2,4,6))
        cap=dict(host_monotonic_interval=[100,108],frames=352800,sample_rate=44100,channels=2,sample_width=2)
        anchor=lambda m:dict(monotonic_ns=int(m*1e9),realtime_ns=int((wall+m-100)*1e9))
        return events(trace),cap,anchor(99),anchor(109)
    def test_known_discards_explain_forward_source_gaps(self):
        rows,cap,before,after=self.fixture();runs=[]
        # Construct symbols at 100-ms source increments with three known 46-ms
        # deletions. Centers remain quantized; discard compensation improves fit.
        for packet in range(79):
            source=(packet+.5)/10;t=source
            for boundary in (2,4,6):
                if t>=boundary:t-=8192/176400
            runs.append(dict(packet=packet,center_pcm_s=t))
        result=analyze(rows,cap,before,after,dict(status='ok',pcm_s=8,pitch_scale=1,symbol_runs=runs,approximate_source_seconds_per_pcm_second=1.017))
        self.assertEqual(result['capture_window']['overruns'],3)
        self.assertAlmostEqual(result['capture_window']['codec_buffer_discard_seconds'],3*8192/176400)
        self.assertLess(result['coded_source']['after_discard_accounting_residual_std_s'],.015)
        self.assertLess(result['coded_source']['after_discard_accounting_residual_std_s'],result['coded_source']['residual_std_s']/2)
    def test_correction_thresholds_and_input_separation(self):
        text='\n'.join('2026-10-20T00:00:00Z hda_audio_adjust st dac, pos '+str(x) for x in (1025,-1025,-2048,-2049))
        self.assertEqual(summary(events(text))['summed_origin_correction_ms'],-5)
        rows,cap,before,after=self.fixture();rows.append(dict(wall_s=rows[0]['wall_s'],kind='overrun',stream='adc'))
        self.assertEqual(analyze(rows,cap,before,after,None)['whole_trace']['overruns'],3)
    def test_inactive_capture_and_clock_step_rejected(self):
        rows,cap,before,after=self.fixture();bad=deepcopy(cap);bad['frames']=44100
        with self.assertRaisesRegex(ValueError,'inactive/gapped'):analyze(rows,bad,before,after,None)
        after['realtime_ns']+=100000000
        with self.assertRaisesRegex(ValueError,'anchors differ'):analyze(rows,cap,before,after,None)
    def test_unknown_trace_payload_rejected(self):
        for line in ('2026-10-20T00:00:00Z hda_audio_adjust st dac, pos 0','hda_audio_overrun st dac','2026-10-20T00:00:00Z hda_audio_new st dac'):
            with self.assertRaises(ValueError):events(line)

if __name__=='__main__':unittest.main()
