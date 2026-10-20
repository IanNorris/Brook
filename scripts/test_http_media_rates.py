#!/usr/bin/env python3
import unittest
from workloads.analyze_http_media_rates import analyze_rows


def sample(sequence,lower,upper,media,page,epoch='e'):
    return dict(hostMonotonicNs=int(upper*1e9),state=dict(sequence=sequence,epoch=epoch,
        clock=dict(hostMonotonicNs=int(lower*1e9)),mediaTotal=media,performanceMs=page*1000,
        callbacks=int(media*24),paused=False,ready=4,rate=1,ambiguousGaps=0))


class CausalRates(unittest.TestCase):
    def summary(self,start=0,end=100):return dict(phase='test',load_host_start_latest_ns=start*1e9,load_host_end_earliest_ns=end*1e9)
    def test_page_clock_can_run_fast_without_biasing_media_rate(self):
        r=analyze_rows(self.summary(),[sample(1,0,.01,0,0),sample(2,2,2.01,1,2.6)],'e')['loaded']
        self.assertLess(r['media_host_rate_low'],.5);self.assertGreater(r['media_host_rate_high'],.5)
        self.assertLess(r['page_host_rate_low'],1.3);self.assertGreater(r['page_host_rate_high'],1.3)
        self.assertGreater(r['page_host_rate_low'],1)
    def test_reordered_late_posts_do_not_create_a_false_rate(self):
        r=analyze_rows(self.summary(),[sample(2,2,3,1,2),sample(1,0,10,0,0)],'e')['whole']
        self.assertEqual(r['status'],'insufficient_temporal_separation')
        self.assertNotIn('media_host_rate_low',r)
    def test_uncertain_load_edge_is_excluded(self):
        r=analyze_rows(self.summary(10,20),[sample(1,9,11,0,0),sample(2,12,12.1,1,1),sample(3,18,18.1,4,7),sample(4,19,21,5,8)],'e')['loaded']
        self.assertEqual(r['samples'],2);self.assertEqual(r['media_s'],3)
        self.assertLess(r['media_host_rate_low'],.5);self.assertGreater(r['media_host_rate_high'],.5)
    def test_noncausal_or_duplicate_receipts_are_rejected(self):
        with self.assertRaises(ValueError):analyze_rows(self.summary(),[sample(1,2,1,0,0)],'e')
        with self.assertRaises(ValueError):analyze_rows(self.summary(),[sample(1,0,1,0,0),sample(1,1,2,1,1)],'e')
    def test_other_epoch_never_supplies_missing_samples(self):
        r=analyze_rows(self.summary(),[sample(1,0,1,0,0),sample(2,2,3,1,2,'other')],'e')
        self.assertEqual(r['loaded']['status'],'insufficient_samples')


if __name__=='__main__':unittest.main()
